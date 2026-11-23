// =====================================================================
// ANCHOR sketch 09 (version 2) - noise cancelling + AI that picks the mode
//
// Files that must be in this folder:
//   09_anchor_full.ino   (this file - keep only ONE .ino in the folder)
//   anchor_audio.h       (unchanged)
//   anchor_anc2.h        (NEW cancelling engine)
//   anchor_ai.h + anchor_ai_model.h   (unchanged built-in AI)
//
//   Core 1 (fast): read mics -> cancel noise -> speaker
//   Core 0 (slow, every 0.5 s): AI listens to the REF mic and asks for a mode
//
//   AI hears          Mode     What the headset does
//   ---------------   -------  ------------------------------------------
//   machine / other   CANCEL   cancelling ON and learning
//   voice             VOICE    half cancelling + half hear-through
//   alarm             ALARM    cancelling OFF, hear-through (reacts at once)
//   quiet             IDLE     cancelling ON with the last filter, not learning
//   not sure (<60%)   UNSURE   half cancelling + half hear-through
//   (after calibration, or if the filter runs away)  SAFE = speaker silent
//
// Serial Monitor (115200). Type a letter, press Enter:
//   x = mute            u = un-mute          c = calibrate again (3 s hiss)
//   o = cancelling OFF (measures the "before" level)
//   m = cancelling ON by hand (ignores the AI - use this to test cancelling)
//   a = back to automatic (the AI decides)
//   f = switch "remove speaker leak from REF mic" on/off (to compare)
//   + / - = learn faster / slower
//
// HONEST NOTE: this version was compiled and its logic was run in a computer
// simulation, but it has NOT been run on real hardware. Test with the cup
// OFF your head first.
// =====================================================================
#include "anchor_audio.h"
#include "anchor_anc2.h"
// ---------------------------------------------------------------------
// WHICH AI?  Change only ONE line:
//   - Built-in AI: leave the next line as it is.
//   - Your Edge Impulse AI: remove the two slashes and put YOUR library name.
// #include <ANCHOR_inferencing.h>
// ---------------------------------------------------------------------
#ifndef EI_CLASSIFIER_FREQUENCY          // no Edge Impulse library -> built-in AI
  #define ANCHOR_QUIET_DBFS -55.0f       // built-in AI only: softer than this = "quiet"
  #include "anchor_ai.h"
  #define ANCHOR_AI_NAME "built-in AI"
#else
  #define ANCHOR_AI_NAME "Edge Impulse AI"
#endif

#if EI_CLASSIFIER_FREQUENCY != 16000
#error "The model must use 16000 Hz audio."
#endif

// ---------- settings you may tune ----------
#define ANC_ENABLED    1       // 0 = no cancelling at all (AI + hear-through only). Demo fallback.
#define OUT_VOLUME     70      // speaker volume 0..100
#define IN_VOLUME      25      // SAME as when you recorded the training clips
#define BLOCK          8       // samples per block (8 = 0.5 ms)
#define DMA_COUNT      3
#define OUT_LIMIT      0.30f   // hard limit on what goes to the speaker
#define PASS_GAIN      0.10f   // hear-through loudness
#define CAL_AMP        0.06f   // calibration hiss (0.06 at volume 70 = your old 0.10 at volume 40)
#define CAL_SECONDS    3
#define ANC_MU         0.01f   // learning speed
#define CONFIDENT      0.60f   // below this the AI is "not sure"
#define ALARM_TRIGGER  0.40f   // alarm score that is enough to react
#define ALARM_HOLD_MS  3000
#define QUIET_KEEPS_ANC 1      // 1 = when quiet keep the last filter, 0 = speaker silent
#define VOICE_ANC      0.5f
#define VOICE_PASS     0.5f
#define UNSURE_ANC     0.5f
#define UNSURE_PASS    0.5f
#define ENTER_VOTES    2       // "machine" answers in a row before cancelling starts
#define LEAVE_VOTES    2       // other answers in a row before cancelling stops (alarm = at once)
#define COOLDOWN_MS    3000    // stay in SAFE this long after the filter ran away
#define AUTO_RECAL     1       // 1 = after 3 run-aways in 20 s, calibrate again by itself
#define RUNAWAY_DB     6.0f    // ERR this much louder than "speaker off" (compared with REF) = run-away

enum Mode { M_CAL, M_IDLE, M_CANCEL, M_VOICE, M_ALARM, M_UNSURE, M_SAFE, M_MUTE };
const char* const modeName[] = { "CALIBRATING", "IDLE", "CANCEL", "VOICE", "ALARM", "UNSURE", "SAFE", "MUTED" };

AncEngine2 anc;

// ---- only the audio task changes these ----
volatile Mode mode = M_CAL;
static float ancTarget = 0, passTarget = 0;
static bool adaptAllowed = false;

// ---- requests from the AI and the keyboard ----
volatile Mode wantMode = M_SAFE;       // what the AI asks for
volatile int forceMode = -1;           // -1 = automatic, otherwise a Mode chosen by key
volatile bool wantCal = false, muted = false;

// ---- numbers for the status line ----
volatile float errDb = -99, refDb = -99, offDb = NAN;
volatile float calQ = 0, calLeak = 0;
volatile int calPeak = 0;
volatile uint32_t calCount = 0, trips = 0, autoRecals = 0;
volatile uint32_t clipSamples = 0, busyUs = 0;
volatile int lastBest = -1;
volatile float lastConf = 0, lastAlarm = 0;
volatile uint32_t lastInferMs = 0;

// ---------- ring buffer: the last seconds of REF mic for the AI ----------
#define RING_LEN 32768                 // must be >= model window
static int16_t ring[RING_LEN];
static volatile uint32_t ringPos = 0;
static int16_t* aiWindow;

static int getData(size_t offset, size_t length, float* out) {
  for (size_t i = 0; i < length; i++) out[i] = (float)aiWindow[offset + i];
  return 0;
}

static void applyMode(Mode m) {
  mode = m;
  switch (m) {
    case M_CANCEL: ancTarget = 1.0f;       passTarget = 0.0f;        adaptAllowed = true;  break;
    case M_VOICE:  ancTarget = VOICE_ANC;  passTarget = VOICE_PASS;  adaptAllowed = false; break;
    case M_ALARM:  ancTarget = 0.0f;       passTarget = 1.0f;        adaptAllowed = false; break;
    case M_UNSURE: ancTarget = UNSURE_ANC; passTarget = UNSURE_PASS; adaptAllowed = false; break;
    case M_IDLE:   ancTarget = QUIET_KEEPS_ANC ? 1.0f : 0.0f; passTarget = 0.0f; adaptAllowed = false; break;
    default:       ancTarget = 0.0f;       passTarget = 0.0f;        adaptAllowed = false; break;  // SAFE, MUTE
  }
#if !ANC_ENABLED
  ancTarget = 0.0f;
  adaptAllowed = false;
#endif
}

// ================= the audio work for one block =================
static void audioBlock(const int16_t* inb, int16_t* outb) {
  static uint32_t calN = 0, badSamples = 0, cooldownUntil = 0, tripTime[3] = { 0, 0, 0 };
  static float gAnc = 0, gPass = 0;
  static float pE = 0, pX = 0, pO = 0, ratio0 = 1.0f, clipLvl = 0, sGain2 = 0;
  static float eDcM = 0, xDcM = 0;
  static double eAcc = 0, xAcc = 0;
  static uint32_t accN = 0;
  const uint32_t now = millis();

  if (wantCal) {
    wantCal = false;
    anc.resetAll();
    calN = 0; gAnc = gPass = 0; pE = pX = 0; badSamples = 0; clipLvl = 0;
    applyMode(M_CAL);
  }
  if (mode != M_CAL) {                       // who decides the mode right now?
    Mode t;
    if (muted) t = M_MUTE;
    else if ((int32_t)(cooldownUntil - now) > 0) t = (forceMode < 0 && wantMode == M_ALARM) ? M_ALARM : M_SAFE;
    else if (forceMode >= 0) t = (Mode)forceMode;
    else t = wantMode;
    if (t != mode) applyMode(t);
  }

  int clipped = 0;
  for (int i = 0; i < BLOCK; i++) {
    float x = fromPcm(inb[2 * i + CH_REF]);
    float e = fromPcm(inb[2 * i + CH_ERR]);
    ring[ringPos % RING_LEN] = inb[2 * i + CH_REF];
    ringPos = ringPos + 1;
    float out = 0;

    if (mode == M_CAL) {
      out = anc.calibrateStep(e, x);
      if (++calN == SAMPLE_RATE * CAL_SECONDS / 2) anc.calibrationStatsReset();
      if (calN >= SAMPLE_RATE * CAL_SECONDS) {
        calQ = anc.calibrationQualityDb();
        calPeak = anc.modelPeakIndex();
        calLeak = anc.leakDb();
        sGain2 = 0;
        for (int k = 0; k < ANC_NS; k++) sGain2 += anc.S[k] * anc.S[k];   // speaker -> ERR loudness
        calCount = calCount + 1;
        applyMode(M_SAFE);
      }
    } else {
      gAnc += 0.002f * (ancTarget - gAnc);       // smooth changes, about 30 ms
      gPass += 0.002f * (passTarget - gPass);
      float xc = anc.cleanRef(x);                // REF mic without our own speaker
      float y = anc.antiNoise(xc);
      out = gAnc * y + gPass * PASS_GAIN * xc;
      bool c = false;
      if (out > OUT_LIMIT) { out = OUT_LIMIT; c = true; }
      if (out < -OUT_LIMIT) { out = -OUT_LIMIT; c = true; }
      if (c) clipped++;
      // learn only when fully in CANCEL mode and the speaker is not clipping
      anc.finish(out, e, adaptAllowed && !c && gAnc > 0.98f && gPass < 0.02f);
      xDcM += 0.001f * (xc - xDcM);
      float xa = xc - xDcM;
      pX += 0.002f * (xa * xa - pX);
      xAcc += xa * xa;
    }
    pO += 0.002f * (out * out - pO);
    outb[2 * i] = toPcm(out);
    outb[2 * i + 1] = toPcm(out);

    eDcM += 0.001f * (e - eDcM);
    float ea = e - eDcM;
    pE += 0.002f * (ea * ea - pE);
    eAcc += ea * ea; accN++;
  }
  clipSamples = clipSamples + clipped;

  if (mode != M_CAL) {
    // speaker (nearly) silent -> learn the normal ERR/REF loudness ratio of the ear muff
    if (gAnc < 0.02f && gPass < 0.02f && pX > 1e-7f) {
      float r = pE / pX;
      if (r > 4.0f) r = 4.0f;
      ratio0 += 0.00005f * BLOCK * (r - ratio0);     // slow average, about 1 s
    }
    if (gAnc > 0.5f) {
      // the anti-noise wants to be louder than the limit: turn the filter down a little
      if (clipped > 0) for (int k = 0; k < ANC_NW; k++) anc.W[k] *= 0.99f;
      clipLvl += (BLOCK / 2000.0f) * ((float)clipped / BLOCK - clipLvl);
      // run-away check: ERR much louder (compared with REF) than with the speaker off,
      // AND most of that sound is coming from our own speaker (so talking does not trip it)
      static const float runaway = powf(10.0f, RUNAWAY_DB / 10.0f);
      bool bad = pE > 1e-6f && pE > runaway * ratio0 * pX && pO * sGain2 > 0.5f * pE;
      badSamples = bad ? badSamples + BLOCK : 0;
      if (clipLvl > 0.5f || badSamples > SAMPLE_RATE * 3 / 10) {
        anc.resetW();
        badSamples = 0; clipLvl = 0;
        trips = trips + 1;
        cooldownUntil = now + COOLDOWN_MS;
        tripTime[0] = tripTime[1]; tripTime[1] = tripTime[2]; tripTime[2] = now;
        applyMode(M_SAFE);
#if AUTO_RECAL
        if (tripTime[0] != 0 && now - tripTime[0] < 20000) {   // 3 run-aways in 20 s
          tripTime[0] = tripTime[1] = tripTime[2] = 0;
          autoRecals = autoRecals + 1;
          wantCal = true;
        }
#endif
      }
    } else {
      badSamples = 0;
      clipLvl *= 0.99f;
    }
  }

  if (accN >= SAMPLE_RATE / 2) {             // level report every 0.5 s
    errDb = 10.0f * log10f((float)(eAcc / accN) + 1e-12f);
    refDb = 10.0f * log10f((float)(xAcc / accN) + 1e-12f);
    if (mode == M_SAFE && forceMode == M_SAFE && gAnc < 0.02f && gPass < 0.02f) offDb = errDb;
    eAcc = xAcc = 0; accN = 0;
  }
}

// ================= the AI: turn one answer into a mode request =================
static int labelIndex(const char* name) {
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    if (strcmp(ei_classifier_inferencing_categories[i], name) == 0) return i;
  return -1;
}

static Mode aiDecide(const float* score, uint32_t now) {
  static const int iAlarm = labelIndex("alarm"), iMachine = labelIndex("machine");
  static const int iVoice = labelIndex("voice"), iQuiet = labelIndex("quiet"), iOther = labelIndex("other");
  static Mode aiMode = M_UNSURE, lastCand = M_UNSURE;
  static int votes = 0;
  static uint32_t alarmUntil = 0;

  int best = 0;
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    if (score[i] > score[best]) best = i;
  float conf = score[best];
  float alarmScore = iAlarm >= 0 ? score[iAlarm] : 0;
  lastBest = best; lastConf = conf; lastAlarm = alarmScore;

  Mode cand;
  if (alarmScore >= ALARM_TRIGGER) { cand = M_ALARM; alarmUntil = now + ALARM_HOLD_MS; }
  else if ((int32_t)(alarmUntil - now) > 0) cand = M_ALARM;
  else if (conf < CONFIDENT) cand = M_UNSURE;
  else if (best == iVoice) cand = M_VOICE;
  else if (best == iQuiet) cand = M_IDLE;
  else if (best == iMachine || best == iOther) cand = M_CANCEL;
  else cand = M_UNSURE;

  if (cand == M_ALARM) { aiMode = M_ALARM; votes = 0; }      // alarms win at once
  else if (cand == aiMode) votes = 0;
  else {
    votes = (cand == lastCand) ? votes + 1 : 1;
    int need = 1;
    if (cand == M_CANCEL) need = ENTER_VOTES;
    else if (aiMode == M_CANCEL) need = LEAVE_VOTES;
    if (votes >= need) { aiMode = cand; votes = 0; }
  }
  lastCand = cand;
  return aiMode;
}

// ================= CORE 0: the AI task, every 0.5 s =================
void aiTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (mode == M_CAL) continue;
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

    float score[EI_CLASSIFIER_LABEL_COUNT];
    for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) score[i] = result.classification[i].value;
    wantMode = aiDecide(score, millis());     // only a request: the audio task applies it
  }
}

// ================= CORE 1: the audio task =================
void audioTask(void*) {
  static int16_t inb[BLOCK * 2], outb[BLOCK * 2];
  // Put the speaker queue into its "longest delay" state before calibrating.
  // In that state a late block cannot change the delay any more, so the delay
  // we calibrate with stays the delay we run with. Costs about one block.
  static int32_t zeros[2 * BLOCK];
  memset(zeros, 0, sizeof(zeros));
  for (int i = 0; i < 8; i++) {
    size_t done = 0;
    if (i2s_channel_write(i2s_tx, zeros, sizeof(zeros), &done, 0) != ESP_OK || done < sizeof(zeros)) break;
  }
  for (;;) {
    audioRead(inb, BLOCK);
    uint32_t t0 = micros();
    audioBlock(inb, outb);
    busyUs = busyUs + (micros() - t0);
    audioWrite(outb, BLOCK);
  }
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 09 v2 - full system (" ANCHOR_AI_NAME ")");
#if !ANC_ENABLED
  Serial.println("NOTE: ANC_ENABLED is 0 - cancelling is switched off, only AI + hear-through.");
#endif
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
  anc.begin();
  anc.mu = ANC_MU;
  anc.cal_amp = CAL_AMP;
  Serial.println("Calibrating: hiss for 3 s. Wear the headset (or keep it exactly as you will use it)...");
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 1, nullptr, 1);
  xTaskCreatePinnedToCore(aiTask, "ai", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastPrint = 0, shownCal = 0, shownTrips = 0, shownAuto = 0;
  static uint32_t lastBusy = 0, lastClip = 0, lastLost = 0;
  static int prevPeak = -1;

  if (calCount != shownCal) {                // a calibration just finished
    shownCal = calCount;
    Serial.printf("Calibration: quality %.1f dB (want -6 or lower), delay %d samples = %.2f ms, speaker leak into REF %.1f dB\n",
                  calQ, calPeak, 1000.0f * calPeak / SAMPLE_RATE, calLeak);
    if (calQ > -3.0f) Serial.println("  WARNING: poor calibration. Cancelling will not work. Check the ERR mic / raise CAL_AMP / quieter room, then type c.");
    if (calLeak > -6.0f) Serial.println("  WARNING: the REF mic hears the speaker almost as well as the ERR mic. Seal the cup / move the REF mic away.");
    if (prevPeak >= 0 && abs(calPeak - prevPeak) >= 4)
      Serial.printf("  NOTE: the delay changed since the last calibration (%d -> %d samples). The timing is not stable.\n", prevPeak, calPeak);
    prevPeak = calPeak;
  }
  if (trips != shownTrips) {
    shownTrips = trips;
    Serial.println("RUN-AWAY: the filter made things louder, so it was reset. SAFE for 3 s.");
  }
  if (autoRecals != shownAuto) {
    shownAuto = autoRecals;
    Serial.println("3 run-aways in 20 s -> calibrating again by itself (3 s hiss).");
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'x') { muted = true; Serial.println(">> MUTED"); }
    if (c == 'u') { muted = false; Serial.println(">> un-muted"); }
    if (c == 'c') { wantCal = true; Serial.println(">> calibrating again (3 s hiss)"); }
    if (c == 'o') { forceMode = M_SAFE; Serial.println(">> cancelling OFF by hand (measuring the 'before' level)"); }
    if (c == 'm') { forceMode = M_CANCEL; Serial.println(">> cancelling ON by hand (AI ignored)"); }
    if (c == 'a') { forceMode = -1; Serial.println(">> automatic (AI decides)"); }
    if (c == 'f') { anc.neutralise = !anc.neutralise; Serial.printf(">> remove speaker leak from REF: %s\n", anc.neutralise ? "ON" : "OFF"); }
    if (c == '+') { anc.mu = fminf(anc.mu * 2, 0.08f); Serial.printf(">> mu = %.4f\n", anc.mu); }
    if (c == '-') { anc.mu = fmaxf(anc.mu / 2, 0.0005f); Serial.printf(">> mu = %.4f\n", anc.mu); }
  }
  uint32_t nowMs = millis();
  if (nowMs - lastPrint >= 1000) {
    uint32_t dt = nowMs - lastPrint;
    lastPrint = nowMs;
    uint32_t b = busyUs, cl = clipSamples, lost = i2s_rx_overflows;
    float load = 0.1f * (b - lastBusy) / dt;                       // percent of one core
    float clipPct = 100.0f * (cl - lastClip) / (SAMPLE_RATE * dt / 1000.0f);
    Mode m = mode;
    Serial.printf("%-11s ERR %6.1f  REF %6.1f dBFS", modeName[m], errDb, refDb);
    if ((m == M_CANCEL || m == M_IDLE) && !isnan(offDb)) Serial.printf("  REDUCTION %5.1f dB", offDb - errDb);
    if (lastBest >= 0 && forceMode < 0)
      Serial.printf("  AI: %s %.2f (alarm %.2f, %lu ms)", ei_classifier_inferencing_categories[lastBest],
                    lastConf, lastAlarm, (unsigned long)lastInferMs);
    if (forceMode >= 0) Serial.print("  [by hand]");
    Serial.printf("  load %2.0f%%", load);
    if (clipPct >= 0.5f) Serial.printf("  CLIP %2.0f%%", clipPct);
    if (lost != lastLost) Serial.printf("  LOST DATA +%lu", (unsigned long)(lost - lastLost));
    Serial.println();
    lastBusy = b; lastClip = cl; lastLost = lost;
  }
  delay(20);
}
