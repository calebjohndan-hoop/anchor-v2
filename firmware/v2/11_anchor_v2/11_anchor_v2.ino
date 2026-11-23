// =====================================================================
// ANCHOR sketch 11 - v2 board: noise cancelling in BOTH ears + AI
//
// Same cancelling engine (anchor_anc2.h) and the same AI (built-in or Edge
// Impulse) as sketch 09. What is new:
//   - two ears, each on its own I2S port, wired exactly like v1 (anchor_audio_v2.h)
//   - each ear calibrates, cancels and protects itself on its own
//   - the AI listens to the LEFT REF mic and asks BOTH ears for a mode
//
//   Core 1: LEFT ear audio (top priority)
//   Core 0: RIGHT ear audio (top priority) + AI every 0.5 s (low priority)
//
// Files in this folder:
//   11_anchor_v2.ino, anchor_audio_v2.h, anchor_anc2.h, anchor_ai.h, anchor_ai_model.h
//
// Arduino IDE: board "ESP32S3 Dev Module", Flash 16MB, PSRAM "OPI PSRAM",
//              USB CDC On Boot "Enabled" (the v2 board uses the ESP32-S3's own USB).
//
// Serial Monitor (115200). Type a letter, press Enter:
//   x = mute            u = un-mute          c = calibrate both ears again
//   o = cancelling OFF (measures the "before" level)
//   m = cancelling ON by hand (ignores the AI)
//   a = back to automatic (the AI decides)
//   f = "remove speaker leak from REF mic" on/off
//   + / - = learn faster / slower
//   1 / 2 / 3 = run LEFT only / RIGHT only / BOTH ears (for testing one cup at a time)
//
// HONEST NOTE: this sketch compiles, but it has NOT run on v2 hardware yet
// (the boards are not built). The audio and cancelling code per ear is the
// same as sketch 09.
// =====================================================================
#define SAMPLE_RATE 16000
#include "anchor_audio_v2.h"
#include "anchor_anc2.h"
// ---------------------------------------------------------------------
// WHICH AI?  Change only ONE line:
//   - Built-in AI: leave the next line as it is.
//   - Your Edge Impulse AI: remove the two slashes and put YOUR library name.
// #include <ANCHOR_inferencing.h>
// ---------------------------------------------------------------------
#ifndef EI_CLASSIFIER_FREQUENCY
  #define ANCHOR_QUIET_DBFS -55.0f
  #include "anchor_ai.h"
  #define ANCHOR_AI_NAME "built-in AI"
#else
  #define ANCHOR_AI_NAME "Edge Impulse AI"
#endif

#if EI_CLASSIFIER_FREQUENCY != 16000
#error "The model must use 16000 Hz audio."
#endif

// ---------- settings you may tune (same meaning as sketch 09) ----------
#define ANC_ENABLED    1
#define OUT_VOLUME     70      // v2 uses 32 ohm drivers: they are quieter than v1's 8 ohm speaker, re-tune this
#define IN_VOLUME      25      // SAME as when you recorded the training clips
#define BLOCK          8
#define DMA_COUNT      3
#define OUT_LIMIT      0.30f
#define PASS_GAIN      0.10f
#define CAL_AMP        0.06f
#define CAL_SECONDS    3
#define ANC_MU         0.01f
#define CONFIDENT      0.60f
#define ALARM_TRIGGER  0.40f
#define ALARM_HOLD_MS  3000
#define QUIET_KEEPS_ANC 1
#define VOICE_ANC      0.5f
#define VOICE_PASS     0.5f
#define UNSURE_ANC     0.5f
#define UNSURE_PASS    0.5f
#define ENTER_VOTES    2
#define LEAVE_VOTES    2
#define COOLDOWN_MS    3000
#define AUTO_RECAL     1
#define RUNAWAY_DB     6.0f

enum Mode { M_CAL, M_IDLE, M_CANCEL, M_VOICE, M_ALARM, M_UNSURE, M_SAFE, M_MUTE };
const char* const modeName[] = { "CALIBRATING", "IDLE", "CANCEL", "VOICE", "ALARM", "UNSURE", "SAFE", "MUTED" };

// ---- requests shared by both ears (from the AI and the keyboard) ----
volatile Mode wantMode = M_SAFE;
volatile int forceMode = -1;
volatile bool muted = false;
volatile int earsOn = 3;               // bit 0 = left, bit 1 = right
volatile int lastBest = -1;
volatile float lastConf = 0, lastAlarm = 0;
volatile uint32_t lastInferMs = 0;

// ---------- ring buffer: the last seconds of the LEFT REF mic for the AI ----------
#define RING_LEN 32768
static int16_t ring[RING_LEN];
static volatile uint32_t ringPos = 0;
static int16_t* aiWindow;

static int getData(size_t offset, size_t length, float* out) {
  for (size_t i = 0; i < length; i++) out[i] = (float)aiWindow[offset + i];
  return 0;
}

// ================= one ear: the sketch 09 audio code, per ear =================
struct Ear {
  int id = 0;
  AncEngine2 anc;
  volatile Mode mode = M_CAL;
  float ancTarget = 0, passTarget = 0;
  bool adaptAllowed = false;
  volatile bool wantCal = false;
  // status numbers
  volatile float errDb = -99, refDb = -99, offDb = NAN;
  volatile float calQ = 0, calLeak = 0;
  volatile int calPeak = 0;
  volatile uint32_t calCount = 0, trips = 0, autoRecals = 0, clipSamples = 0, busyUs = 0;
  // what were "static" variables inside audioBlock() in sketch 09
  uint32_t calN = 0, badSamples = 0, cooldownUntil = 0, tripTime[3] = { 0, 0, 0 };
  float gAnc = 0, gPass = 0;
  float pE = 0, pX = 0, pO = 0, ratio0 = 1.0f, clipLvl = 0, sGain2 = 0;
  float eDcM = 0, xDcM = 0;
  double eAcc = 0, xAcc = 0;
  uint32_t accN = 0;

  void applyMode(Mode m) {
    mode = m;
    switch (m) {
      case M_CANCEL: ancTarget = 1.0f;       passTarget = 0.0f;        adaptAllowed = true;  break;
      case M_VOICE:  ancTarget = VOICE_ANC;  passTarget = VOICE_PASS;  adaptAllowed = false; break;
      case M_ALARM:  ancTarget = 0.0f;       passTarget = 1.0f;        adaptAllowed = false; break;
      case M_UNSURE: ancTarget = UNSURE_ANC; passTarget = UNSURE_PASS; adaptAllowed = false; break;
      case M_IDLE:   ancTarget = QUIET_KEEPS_ANC ? 1.0f : 0.0f; passTarget = 0.0f; adaptAllowed = false; break;
      default:       ancTarget = 0.0f;       passTarget = 0.0f;        adaptAllowed = false; break;
    }
#if !ANC_ENABLED
    ancTarget = 0.0f;
    adaptAllowed = false;
#endif
  }

  void block(const int16_t* inb, int16_t* outb) {
    const uint32_t now = millis();
    if (wantCal) {
      wantCal = false;
      anc.resetAll();
      calN = 0; gAnc = gPass = 0; pE = pX = 0; badSamples = 0; clipLvl = 0;
      applyMode(M_CAL);
    }
    if (mode != M_CAL) {
      Mode t;
      if (muted || !(earsOn & (1 << id))) t = M_MUTE;
      else if ((int32_t)(cooldownUntil - now) > 0) t = (forceMode < 0 && wantMode == M_ALARM) ? M_ALARM : M_SAFE;
      else if (forceMode >= 0) t = (Mode)forceMode;
      else t = wantMode;
      if (t != mode) applyMode(t);
    }

    int clipped = 0;
    for (int i = 0; i < BLOCK; i++) {
      float x = fromPcm(inb[2 * i + CH_REF]);
      float e = fromPcm(inb[2 * i + CH_ERR]);
      if (id == 0) { ring[ringPos % RING_LEN] = inb[2 * i + CH_REF]; ringPos = ringPos + 1; }
      float out = 0;

      if (mode == M_CAL) {
        out = anc.calibrateStep(e, x);
        if (++calN == SAMPLE_RATE * CAL_SECONDS / 2) anc.calibrationStatsReset();
        if (calN >= SAMPLE_RATE * CAL_SECONDS) {
          calQ = anc.calibrationQualityDb();
          calPeak = anc.modelPeakIndex();
          calLeak = anc.leakDb();
          sGain2 = 0;
          for (int k = 0; k < ANC_NS; k++) sGain2 += anc.S[k] * anc.S[k];
          calCount = calCount + 1;
          applyMode(M_SAFE);
        }
      } else {
        gAnc += 0.002f * (ancTarget - gAnc);
        gPass += 0.002f * (passTarget - gPass);
        float xc = anc.cleanRef(x);
        float y = anc.antiNoise(xc);
        out = gAnc * y + gPass * PASS_GAIN * xc;
        bool c = false;
        if (out > OUT_LIMIT) { out = OUT_LIMIT; c = true; }
        if (out < -OUT_LIMIT) { out = -OUT_LIMIT; c = true; }
        if (c) clipped++;
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
      if (gAnc < 0.02f && gPass < 0.02f && pX > 1e-7f) {
        float r = pE / pX;
        if (r > 4.0f) r = 4.0f;
        ratio0 += 0.00005f * BLOCK * (r - ratio0);
      }
      if (gAnc > 0.5f) {
        if (clipped > 0) for (int k = 0; k < ANC_NW; k++) anc.W[k] *= 0.99f;
        clipLvl += (BLOCK / 2000.0f) * ((float)clipped / BLOCK - clipLvl);
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
          if (tripTime[0] != 0 && now - tripTime[0] < 20000) {
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

    if (accN >= SAMPLE_RATE / 2) {
      errDb = 10.0f * log10f((float)(eAcc / accN) + 1e-12f);
      refDb = 10.0f * log10f((float)(xAcc / accN) + 1e-12f);
      if (mode == M_SAFE && forceMode == M_SAFE && gAnc < 0.02f && gPass < 0.02f) offDb = errDb;
      eAcc = xAcc = 0; accN = 0;
    }
  }
};

static Ear ears[2];

// ================= the AI: same decision rules as sketch 09 =================
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

  if (cand == M_ALARM) { aiMode = M_ALARM; votes = 0; }
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

void aiTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (ears[0].mode == M_CAL || ears[1].mode == M_CAL) continue;
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
    wantMode = aiDecide(score, millis());
  }
}

// ================= audio tasks: one per ear =================
void audioTask(void* arg) {
  Ear& ear = ears[(int)(intptr_t)arg];
  AnchorPort& port = anchorPort[ear.id];
  static int16_t inbuf[2][BLOCK * 2], outbuf[2][BLOCK * 2];
  int16_t* inb = inbuf[ear.id];
  int16_t* outb = outbuf[ear.id];
  port.primeSilence(BLOCK);
  for (;;) {
    port.read(inb, BLOCK);
    uint32_t t0 = micros();
    ear.block(inb, outb);
    ear.busyUs = ear.busyUs + (micros() - t0);
    port.write(outb, BLOCK);
  }
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 11 - v2 board, two ears (" ANCHOR_AI_NAME ")");
  pinMode(PIN_LED_STATUS, OUTPUT);
  pinMode(PIN_LED_MODE, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  digitalWrite(PIN_LED_STATUS, HIGH);

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
  for (int i = 0; i < 2; i++) {
    ears[i].id = i;
    ears[i].anc.begin();
    ears[i].anc.mu = ANC_MU;
    ears[i].anc.cal_amp = CAL_AMP;
  }
  Serial.printf("Battery %.2f V\n", anchorBatteryVolts());
  Serial.println("Calibrating both ears: hiss for 3 s. Wear the headset...");
  xTaskCreatePinnedToCore(audioTask, "earL", 8192, (void*)0, configMAX_PRIORITIES - 1, nullptr, 1);
  xTaskCreatePinnedToCore(audioTask, "earR", 8192, (void*)1, configMAX_PRIORITIES - 1, nullptr, 0);
  xTaskCreatePinnedToCore(aiTask, "ai", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t lastPrint = 0;
  static uint32_t shownCal[2] = { 0, 0 }, shownTrips[2] = { 0, 0 }, shownAuto[2] = { 0, 0 };
  static uint32_t lastBusy[2] = { 0, 0 }, lastClip[2] = { 0, 0 }, lastLost[2] = { 0, 0 };
  static uint32_t lastPress = 0;

  for (int i = 0; i < 2; i++) {
    Ear& e = ears[i];
    if (e.calCount != shownCal[i]) {
      shownCal[i] = e.calCount;
      Serial.printf("%s calibration: quality %.1f dB (want -6 or lower), delay %d samples = %.2f ms, speaker leak into REF %.1f dB\n",
                    EAR_NAME[i], e.calQ, e.calPeak, 1000.0f * e.calPeak / SAMPLE_RATE, e.calLeak);
      if (e.calQ > -3.0f) Serial.printf("  WARNING: poor %s calibration. Check that ear's ERR mic, raise CAL_AMP, quieter room, then type c.\n", EAR_NAME[i]);
      if (e.calLeak > -6.0f) Serial.printf("  WARNING: %s REF mic hears the speaker almost as well as the ERR mic. Seal the cup / move the REF mic.\n", EAR_NAME[i]);
    }
    if (e.trips != shownTrips[i]) { shownTrips[i] = e.trips; Serial.printf("%s RUN-AWAY: filter reset, SAFE for 3 s.\n", EAR_NAME[i]); }
    if (e.autoRecals != shownAuto[i]) { shownAuto[i] = e.autoRecals; Serial.printf("%s: 3 run-aways in 20 s -> calibrating again.\n", EAR_NAME[i]); }
  }

  // mode button: short press = recalibrate both ears
  if (digitalRead(PIN_BUTTON) == LOW && millis() - lastPress > 800) {
    lastPress = millis();
    ears[0].wantCal = ears[1].wantCal = true;
    Serial.println(">> button: calibrating again");
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'x') { muted = true; Serial.println(">> MUTED"); }
    if (c == 'u') { muted = false; Serial.println(">> un-muted"); }
    if (c == 'c') { ears[0].wantCal = ears[1].wantCal = true; Serial.println(">> calibrating both ears again (3 s hiss)"); }
    if (c == 'o') { forceMode = M_SAFE; Serial.println(">> cancelling OFF by hand"); }
    if (c == 'm') { forceMode = M_CANCEL; Serial.println(">> cancelling ON by hand (AI ignored)"); }
    if (c == 'a') { forceMode = -1; Serial.println(">> automatic (AI decides)"); }
    if (c == 'f') { for (auto& e : ears) e.anc.neutralise = !e.anc.neutralise; Serial.printf(">> remove speaker leak from REF: %s\n", ears[0].anc.neutralise ? "ON" : "OFF"); }
    if (c == '+') { for (auto& e : ears) e.anc.mu = fminf(e.anc.mu * 2, 0.08f); Serial.printf(">> mu = %.4f\n", ears[0].anc.mu); }
    if (c == '-') { for (auto& e : ears) e.anc.mu = fmaxf(e.anc.mu / 2, 0.0005f); Serial.printf(">> mu = %.4f\n", ears[0].anc.mu); }
    if (c == '1') { earsOn = 1; Serial.println(">> LEFT ear only"); }
    if (c == '2') { earsOn = 2; Serial.println(">> RIGHT ear only"); }
    if (c == '3') { earsOn = 3; Serial.println(">> both ears"); }
  }

  uint32_t nowMs = millis();
  if (nowMs - lastPrint >= 1000) {
    uint32_t dt = nowMs - lastPrint;
    lastPrint = nowMs;
    digitalWrite(PIN_LED_MODE, ears[0].mode == M_CANCEL || ears[1].mode == M_CANCEL);
    for (int i = 0; i < 2; i++) {
      Ear& e = ears[i];
      uint32_t b = e.busyUs, cl = e.clipSamples, lost = anchorPort[i].overflows;
      float load = 0.1f * (b - lastBusy[i]) / dt;
      float clipPct = 100.0f * (cl - lastClip[i]) / (SAMPLE_RATE * dt / 1000.0f);
      Mode m = e.mode;
      Serial.printf("%-5s %-11s ERR %6.1f  REF %6.1f dBFS", EAR_NAME[i], modeName[m], e.errDb, e.refDb);
      if ((m == M_CANCEL || m == M_IDLE) && !isnan(e.offDb)) Serial.printf("  REDUCTION %5.1f dB", e.offDb - e.errDb);
      Serial.printf("  load %2.0f%%", load);
      if (clipPct >= 0.5f) Serial.printf("  CLIP %2.0f%%", clipPct);
      if (lost != lastLost[i]) Serial.printf("  LOST DATA +%lu", (unsigned long)(lost - lastLost[i]));
      if (i == 0 && lastBest >= 0 && forceMode < 0)
        Serial.printf("  AI: %s %.2f (alarm %.2f, %lu ms)", ei_classifier_inferencing_categories[lastBest],
                      lastConf, lastAlarm, (unsigned long)lastInferMs);
      if (i == 0 && forceMode >= 0) Serial.print("  [by hand]");
      Serial.println();
      lastBusy[i] = b; lastClip[i] = cl; lastLost[i] = lost;
    }
    Serial.printf("Battery %.2f V\n", anchorBatteryVolts());
  }
  delay(20);
}
