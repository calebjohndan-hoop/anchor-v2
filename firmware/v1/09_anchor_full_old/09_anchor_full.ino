// =====================================================================
// ANCHOR sketch 09 - the full system: noise cancelling + AI that decides
//
//   Core 1 (fast, every 1 ms): read mics -> cancel noise -> speaker
//   Core 0 (slow, every 0.5 s): AI listens to the REF mic and picks a mode
//
//   AI hears      Mode      What the headset does
//   ---------     -------   --------------------------------------------
//   machine/other CANCEL    cancelling ON and learning
//   voice         VOICE     cancelling at half strength, voice let in
//   alarm         ALARM     cancelling OFF, outside sound let in (hear-through)
//   quiet         IDLE      cancelling ON with the last learned anti-noise, not learning
//                           (set QUIET_KEEPS_ANC to 0 for cancelling OFF when quiet)
//   not sure      UNSURE    half cancelling (UNSURE_ANC) + half outside sound (UNSURE_PASS)
//   (SAFE = cancelling OFF, outside sound let in: used only after calibration or if the filter goes unstable)
//
// The AI only CHOOSES the mode. It never makes the anti-noise itself.
// Alarms win at once; cancelling only comes back after 2 calm "machine" answers.
//
// Serial Monitor (115200): x = emergency mute, u = un-mute, c = calibrate again
//
// Works with the BUILT-IN AI or YOUR Edge Impulse model: see "WHICH AI?" below.
// Labels the AI must have (lower case): alarm, machine, quiet, voice.
// "other" is optional: if your model has it, it is cancelled like a machine.
// A score below 60% (or any other unknown label) = UNSURE mode (half cancelling).
// =====================================================================
#include "anchor_audio.h"
#include "anchor_anc.h"
// ---------------------------------------------------------------------
// WHICH AI?  Change only ONE line:
//   - Built-in AI (starter, nothing to install): leave the next line as it is.
//   - Your Edge Impulse AI: add its library ZIP (guide Step 46), then remove
//     the two slashes at the start of the next line and put YOUR library name
//     there (it ends in _inferencing.h).
// #include <ANCHOR_inferencing.h>
// ---------------------------------------------------------------------
#ifndef EI_CLASSIFIER_FREQUENCY          // no Edge Impulse library included -> built-in AI
  #define ANCHOR_QUIET_DBFS -55.0f       // built-in AI only: softer than this = "quiet" (see sketch 04)
  #include "anchor_ai.h"
  #define ANCHOR_AI_NAME "built-in AI"
#else
  #define ANCHOR_AI_NAME "Edge Impulse AI"
#endif

#if EI_CLASSIFIER_FREQUENCY != 16000
#error "The model must use 16000 Hz audio. In Edge Impulse set Frequency to 16000 Hz (guide Step 44)."
#endif

// ---------- settings you may tune ----------
#define OUT_VOLUME     30     // speaker volume 0..100. Start low (MAX98357A is loud)!
#define IN_VOLUME      25      // SAME as when recording the clips
#define BLOCK          8
#define DMA_COUNT      3
#define OUT_LIMIT      0.30f   // hard limit on what goes to the speaker
#define PASS_GAIN      0.2f    // hear-through loudness (outside sound -> speaker)
#define CONFIDENT      0.60f   // below this the AI is "not sure" -> UNSURE mode
#define ALARM_TRIGGER  0.40f   // alarm score that is enough to react (low on purpose)
#define ALARM_HOLD_MS  3000    // stay in ALARM at least this long
#define QUIET_KEEPS_ANC 1      // 1 = when quiet, keep cancelling with the last learned anti-noise
                               //     (learning paused, so it cannot drift on tiny signals)
                               // 0 = when quiet, cancelling OFF (speaker silent)
#define CAL_SECONDS    3

// When the AI is NOT sure (score below CONFIDENT): cancel part of the noise but still let outside sound in.
//   UNSURE_ANC 0.5 + UNSURE_PASS 0.5 = half cancelling (same as VOICE).
//   More cancelling = quieter, but an unrecognised alarm is harder to hear. Keep UNSURE_PASS above 0.
#define UNSURE_ANC     0.5f
#define UNSURE_PASS    0.5f

enum Mode { M_CAL, M_IDLE, M_CANCEL, M_VOICE, M_ALARM, M_SAFE, M_UNSURE, M_MUTE };
const char* modeName[] = { "CALIBRATING", "IDLE", "CANCEL", "VOICE", "ALARM", "SAFE", "UNSURE", "MUTED" };

AncEngine anc;
volatile Mode mode = M_CAL;
volatile bool wantCal = false;
volatile bool muted = false;

// gains the audio task moves towards (smoothly, so there are no clicks)
volatile float ancTarget = 0, passTarget = 0;
volatile bool adaptAllowed = false;

// what the AI said last time (for printing)
volatile int lastBest = -1;
volatile float lastConf = 0, lastAlarm = 0;
volatile uint32_t lastInferMs = 0, unstableCount = 0;
volatile float errDb = -99;
volatile float calQ = 0;

// ---------- ring buffer: last 1+ seconds of REF mic for the AI ----------
#define RING_LEN 32768                 // must be >= model window
static int16_t ring[RING_LEN];
static volatile uint32_t ringPos = 0;  // total samples written
static int16_t* aiWindow;

static int getData(size_t offset, size_t length, float* out) {
  for (size_t i = 0; i < length; i++) out[i] = (float)aiWindow[offset + i];
  return 0;
}

void applyMode(Mode m) {
  mode = m;
  switch (m) {
    case M_CANCEL: ancTarget = 1.0f; passTarget = 0.0f; adaptAllowed = true;  break;
    case M_VOICE:  ancTarget = 0.5f; passTarget = 0.5f; adaptAllowed = false; break;
    case M_ALARM:  ancTarget = 0.0f; passTarget = 1.0f; adaptAllowed = false; break;
    case M_SAFE:   ancTarget = 0.0f; passTarget = 0.7f; adaptAllowed = false; break;
    case M_UNSURE: ancTarget = UNSURE_ANC; passTarget = UNSURE_PASS; adaptAllowed = false; break;
#if QUIET_KEEPS_ANC
    case M_IDLE:   ancTarget = 1.0f; passTarget = 0.0f; adaptAllowed = false; break;
#endif
    default:       ancTarget = 0.0f; passTarget = 0.0f; adaptAllowed = false; break;
  }
}

// ================= CORE 1: audio, every 1 ms =================
void audioTask(void*) {
  static int16_t inb[BLOCK * 2], outb[BLOCK * 2];
  uint32_t calN = 0;
  float gAnc = 0, gPass = 0;
  float ePow = 0, ePowStart = 0; uint32_t badMs = 0;
  double eAcc = 0; uint32_t accN = 0;
  bool wasCancelling = false;

  for (;;) {
    audioRead(inb, BLOCK);
    if (wantCal) { wantCal = false; anc.resetAll(); calN = 0; mode = M_CAL; gAnc = gPass = 0; }

    bool cancelling = (mode == M_CANCEL) && !muted;
    for (int i = 0; i < BLOCK; i++) {
      float x = fromPcm(inb[2 * i + CH_REF]);
      float e = fromPcm(inb[2 * i + CH_ERR]);
      ring[ringPos % RING_LEN] = inb[2 * i + CH_REF];
      ringPos = ringPos + 1;
      float out = 0;

      if (mode == M_CAL) {
        out = anc.calibrateStep(e);
        if (++calN == SAMPLE_RATE * CAL_SECONDS / 2) anc.calibrationStatsReset();
        if (calN >= SAMPLE_RATE * CAL_SECONDS) { calQ = anc.calibrationQualityDb(); applyMode(M_SAFE); }
      } else {
        // smooth gain changes (about 30 ms)
        float ta = muted ? 0 : ancTarget, tp = muted ? 0 : passTarget;
        gAnc += 0.002f * (ta - gAnc);
        gPass += 0.002f * (tp - gPass);
        // learn only when fully in CANCEL mode, otherwise the hear-through would confuse it
        bool adapt = cancelling && adaptAllowed && gAnc > 0.98f && gPass < 0.02f;
        float y = anc.ancStep(x, e, adapt);
        out = gAnc * y + gPass * PASS_GAIN * x;
      }
      if (out > OUT_LIMIT) out = OUT_LIMIT;
      if (out < -OUT_LIMIT) out = -OUT_LIMIT;
      outb[2 * i] = toPcm(out);
      outb[2 * i + 1] = toPcm(out);

      ePow += 0.002f * (e * e - ePow);
      eAcc += e * e; accN++;
    }
    audioWrite(outb, BLOCK);

    // safety: ERR mic 6 dB louder than when cancelling started, for 0.3 s -> stop
    if (cancelling && !wasCancelling) { ePowStart = ePow; badMs = 0; }
    if (cancelling) {
      if (ePow > 4.0f * ePowStart + 1e-6f) badMs++; else badMs = 0;
      if (badMs > 300) { anc.resetW(); applyMode(M_SAFE); unstableCount = unstableCount + 1; badMs = 0; }
    }
    wasCancelling = cancelling;

    if (accN >= SAMPLE_RATE / 2) { errDb = 10.0f * log10f(eAcc / accN + 1e-12); eAcc = 0; accN = 0; }
  }
}

// ================= CORE 0: the AI, every 0.5 s =================
int labelIndex(const char* name) {
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    if (strcmp(ei_classifier_inferencing_categories[i], name) == 0) return i;
  return -1;
}

void aiTask(void*) {
  const int iAlarm = labelIndex("alarm"), iMachine = labelIndex("machine");
  const int iVoice = labelIndex("voice"), iQuiet = labelIndex("quiet");
  const int iOther = labelIndex("other");   // -1 if your model has no "other" label
  uint32_t alarmUntil = 0;
  int machineVotes = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (mode == M_CAL) continue;

    // copy the newest window out of the ring buffer
    uint32_t end = ringPos;
    uint32_t start = end - EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    for (uint32_t i = 0; i < EI_CLASSIFIER_RAW_SAMPLE_COUNT; i++) aiWindow[i] = ring[(start + i) % RING_LEN];

    signal_t sig;
    sig.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    sig.get_data = &getData;
    ei_impulse_result_t result = { 0 };
    uint32_t t0 = millis();
    if (run_classifier(&sig, &result, false) != EI_IMPULSE_OK) continue;
    lastInferMs = millis() - t0;

    int best = 0;
    for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
      if (result.classification[i].value > result.classification[best].value) best = i;
    float conf = result.classification[best].value;
    float alarmScore = iAlarm >= 0 ? result.classification[iAlarm].value : 0;
    lastBest = best; lastConf = conf; lastAlarm = alarmScore;

    // ---- the decision rules (safety first) ----
    Mode next;
    if (alarmScore >= ALARM_TRIGGER) {               // 1. any real chance of an alarm
      next = M_ALARM; alarmUntil = millis() + ALARM_HOLD_MS;
    } else if (millis() < alarmUntil) {              // 2. keep listening after an alarm
      next = M_ALARM;
    } else if (conf < CONFIDENT) {                   // 3. not sure -> partial cancelling, outside sound still let in
      next = M_UNSURE;
    } else if (best == iVoice) {
      next = M_VOICE;
    } else if (best == iQuiet) {
      next = M_IDLE;
    } else if (best == iMachine || best == iOther) {   // machine or "other" -> cancel
      next = M_CANCEL;
    } else {
      next = M_UNSURE;
    }
    // cancelling only comes back after 2 "machine" answers in a row
    if (next == M_CANCEL) {
      machineVotes++;
      if (machineVotes < 2 && mode != M_CANCEL) next = M_UNSURE;
    } else {
      machineVotes = 0;
    }
    if (!muted && next != mode) applyMode(next);
  }
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 09 - full system (" ANCHOR_AI_NAME ")");
  aiWindow = (int16_t*)malloc(EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(int16_t));
  if (!aiWindow || EI_CLASSIFIER_RAW_SAMPLE_COUNT > RING_LEN) {
    Serial.println("ERROR: memory / window size problem");
    while (true) delay(1000);
  }
  const char* need[] = { "alarm", "machine", "quiet", "voice" };
  for (auto n : need)
    if (labelIndex(n) < 0) Serial.printf("WARNING: your model has no label called '%s'\n", n);

  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, DMA_COUNT)) {
    while (true) delay(1000);
  }
  anc.resetAll();
  Serial.println("Calibrating: quiet hiss for 3 s. Keep the room quiet...");
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 1, nullptr, 1);
  xTaskCreatePinnedToCore(aiTask, "ai", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastPrint = 0, lastUnstable = 0;
  static bool calReported = false;

  if (!calReported && mode != M_CAL) {
    calReported = true;
    Serial.printf("Calibration quality %.1f dB (aim for -6 dB or lower).\n", calQ);
  }
  if (mode == M_CAL) calReported = false;

  if (unstableCount != lastUnstable) {
    lastUnstable = unstableCount;
    Serial.println("UNSTABLE: cancelling stopped and reset (safety).");
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'x') { muted = true; applyMode(M_MUTE); Serial.println(">> MUTED"); }
    if (c == 'u') { muted = false; applyMode(M_SAFE); Serial.println(">> un-muted"); }
    if (c == 'c') { wantCal = true; Serial.println(">> calibrating again"); }
  }
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.printf("%-11s ERR %6.1f dBFS", modeName[mode], errDb);
    if (lastBest >= 0)
      Serial.printf("  AI: %s %.2f (alarm %.2f, %lu ms)", ei_classifier_inferencing_categories[lastBest],
                    lastConf, lastAlarm, (unsigned long)lastInferMs);
    if (i2s_rx_overflows) Serial.printf("  lost data %lu", (unsigned long)i2s_rx_overflows);
    Serial.println();
  }
  delay(20);
}
