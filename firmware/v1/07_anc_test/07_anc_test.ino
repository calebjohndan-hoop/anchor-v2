// =====================================================================
// ANCHOR sketch 07 - noise cancelling test (no AI yet)
//
// 1. At start it plays QUIET HISS for 3 s to learn the speaker-to-ear path.
//    Keep the room quiet and the cup on the dummy head / your ear (LOW volume).
// 2. Then play a steady noise near the cup (phone playing tone_200Hz.wav or
//    hum_100Hz.wav from tools/make_test_sounds.py).
// 3. Type in the Serial Monitor (115200), then press Enter:
//      o = cancelling OFF (measure the "before" level)
//      a = cancelling ON  (watch the ERR level go down)
//      c = calibrate again      r = reset the filter
//      + / - = learn faster / slower
//    The line "REDUCTION" = how many dB quieter than the last OFF level.
//
// SAFETY: if the ERR mic gets much louder with cancelling ON, the code
// switches cancelling OFF by itself and prints "UNSTABLE".
// =====================================================================
#include "anchor_audio.h"
#include "anchor_anc.h"

#define OUT_VOLUME 40          // speaker volume 0..100. Start low (MAX98357A is loud)!
#define IN_VOLUME  25          // mic gain 0..100
#define BLOCK      8          // 1 ms blocks = low delay
#define DMA_COUNT  2
#define OUT_LIMIT  0.30f       // anti-noise can never be louder than this
#define CAL_SECONDS 3

AncEngine anc;

enum Mode { MODE_CAL, MODE_OFF, MODE_ON };
volatile Mode mode = MODE_CAL;
volatile bool wantCal = false, wantReset = false;

// levels shared with the print loop
volatile float errDb = -99, refDb = -99, offDb = NAN;
volatile uint32_t unstableCount = 0;
volatile float calQ = 0; volatile int calPeak = 0;

void audioTask(void*) {
  static int16_t inb[BLOCK * 2], outb[BLOCK * 2];
  uint32_t calN = 0;
  double eAcc = 0, rAcc = 0; uint32_t accN = 0;
  float ePowFast = 0, ePowStart = 0;
  uint32_t badMs = 0;

  for (;;) {
    audioRead(inb, BLOCK);

    if (wantCal) { wantCal = false; anc.resetAll(); calN = 0; mode = MODE_CAL; }
    if (wantReset) { wantReset = false; anc.resetW(); }

    for (int i = 0; i < BLOCK; i++) {
      float x = fromPcm(inb[2 * i + CH_REF]);
      float e = fromPcm(inb[2 * i + CH_ERR]);
      float out = 0;

      if (mode == MODE_CAL) {
        out = anc.calibrateStep(e);
        calN++;
        if (calN == SAMPLE_RATE * CAL_SECONDS / 2) anc.calibrationStatsReset();
        if (calN >= SAMPLE_RATE * CAL_SECONDS) {
          calQ = anc.calibrationQualityDb();
          calPeak = anc.modelPeakIndex();
          mode = MODE_OFF;
        }
      } else {
        float y = anc.ancStep(x, e, mode == MODE_ON);
        if (mode == MODE_ON) out = y;
      }
      if (out > OUT_LIMIT) out = OUT_LIMIT;
      if (out < -OUT_LIMIT) out = -OUT_LIMIT;
      outb[2 * i] = toPcm(out);
      outb[2 * i + 1] = toPcm(out);

      eAcc += e * e; rAcc += x * x; accN++;
      ePowFast += 0.002f * (e * e - ePowFast);   // about 30 ms smoothing
    }
    audioWrite(outb, BLOCK);

    // safety: watch for the ERR mic getting much louder while cancelling
    static Mode lastMode = MODE_OFF;
    if (mode == MODE_ON && lastMode != MODE_ON) { ePowStart = ePowFast; badMs = 0; }
    if (mode == MODE_ON) {
      if (ePowFast > 4.0f * ePowStart + 1e-6f) badMs++; else badMs = 0;   // +6 dB
      if (badMs > 300) {               // 0.3 s too loud -> stop
        mode = MODE_OFF; anc.resetW(); unstableCount = unstableCount + 1; badMs = 0;
      }
    }
    lastMode = mode;

    if (accN >= SAMPLE_RATE / 2) {     // every 0.5 s
      errDb = 10.0f * log10f(eAcc / accN + 1e-12);
      refDb = 10.0f * log10f(rAcc / accN + 1e-12);
      if (mode == MODE_OFF) offDb = errDb;
      eAcc = rAcc = 0; accN = 0;
    }
  }
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 07 - noise cancelling test");
  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, DMA_COUNT)) {
    while (true) delay(1000);
  }
  anc.resetAll();
  Serial.println("Calibrating: quiet hiss for 3 seconds. Keep the room quiet...");
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 1, nullptr, 1);
}

void loop() {
  static Mode shown = MODE_CAL;
  static uint32_t lastPrint = 0, lastUnstable = 0;

  if (shown == MODE_CAL && mode != MODE_CAL) {
    Serial.printf("Calibration done. Quality %.1f dB (more negative is better; aim for -6 dB or lower).\n", calQ);
    Serial.printf("Biggest model tap at %d samples = %.2f ms (compare with sketch 05).\n", calPeak, 1000.0f * calPeak / SAMPLE_RATE);
    if (calQ > -3.0f) Serial.println("WARNING: poor calibration. Check the ERR mic, raise OUT_VOLUME a little, or make the room quieter, then type c.");
    Serial.println("Play your test noise, type o (OFF) to measure, then a (ON).");
  }
  shown = mode;

  if (unstableCount != lastUnstable) {
    lastUnstable = unstableCount;
    Serial.println("UNSTABLE: ERR got louder, cancelling switched OFF and filter reset. Try '-' then 'a', or 'c'.");
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'a' && mode != MODE_CAL) { mode = MODE_ON; Serial.println(">> cancelling ON"); }
    if (c == 'o' && mode != MODE_CAL) { mode = MODE_OFF; Serial.println(">> cancelling OFF"); }
    if (c == 'c') { wantCal = true; Serial.println(">> calibrating again (3 s hiss)"); }
    if (c == 'r') { wantReset = true; Serial.println(">> filter reset"); }
    if (c == '+') { anc.mu = fminf(anc.mu * 2, 0.05f); Serial.printf(">> mu = %.4f\n", anc.mu); }
    if (c == '-') { anc.mu = fmaxf(anc.mu / 2, 0.0002f); Serial.printf(">> mu = %.4f\n", anc.mu); }
  }

  if (millis() - lastPrint > 1000 && mode != MODE_CAL) {
    lastPrint = millis();
    Serial.printf("%s  ERR %6.1f dBFS  REF %6.1f dBFS", mode == MODE_ON ? "ON " : "OFF", errDb, refDb);
    if (mode == MODE_ON && !isnan(offDb)) Serial.printf("  REDUCTION %5.1f dB", offDb - errDb);
    if (i2s_rx_overflows) Serial.printf("  (lost data %lu)", (unsigned long)i2s_rx_overflows);
    Serial.println();
  }
  delay(20);
}
