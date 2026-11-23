// =====================================================================
// ANCHOR sketch 05 - THE MOST IMPORTANT TEST: how long is the delay?
// Put the cup on a table (or on your ear at LOW volume). The speaker plays
// a tiny click and we time how long until the ERR mic hears it.
// Expected: 5 measurements in ms. Write them down - they go in your report.
// =====================================================================
#include "anchor_audio.h"

#define OUT_VOLUME 70     // raise a little only if the click is not heard
#define IN_VOLUME  25
#define BLOCK      8     // same small blocks as the noise cancelling uses
#define DMA_COUNT  2
#define CLICK_AMP  0.25f  // click loudness (it is very short)
#define MAX_WAIT   (SAMPLE_RATE / 10)   // give up after 100 ms

int16_t inbuf[BLOCK * 2], outbuf[BLOCK * 2];

// run one block: send 'out' (mono), return ERR samples in errOut
void runBlock(const float* out, float* errOut) {
  audioRead(inbuf, BLOCK);
  for (int i = 0; i < BLOCK; i++) {
    errOut[i] = fromPcm(inbuf[2 * i + CH_ERR]);
    outbuf[2 * i] = toPcm(out[i]);
    outbuf[2 * i + 1] = toPcm(out[i]);
  }
  audioWrite(outbuf, BLOCK);
}

// keep the audio running (silence) instead of using delay()
void idle(int ms) {
  float z[BLOCK], e[BLOCK];
  for (int i = 0; i < BLOCK; i++) z[i] = 0;
  for (int b = 0; b < ms * SAMPLE_RATE / 1000 / BLOCK; b++) runBlock(z, e);
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 05 - delay test");
  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, DMA_COUNT)) {
    while (true) delay(1000);
  }
}

void loop() {
  float out[BLOCK], err[BLOCK];
  float results[5];
  int okCount = 0;
  for (int t = 0; t < 5; t++) {
    // 1) listen to silence for 0.5 s to learn the background level
    for (int i = 0; i < BLOCK; i++) out[i] = 0;
    double sum = 0, sum2 = 0; int cnt = 0;
    for (int b = 0; b < SAMPLE_RATE / 2 / BLOCK; b++) {
      runBlock(out, err);
      for (int i = 0; i < BLOCK; i++) { sum += err[i]; sum2 += err[i] * err[i]; cnt++; }
    }
    float mean = sum / cnt;
    float noise = sqrtf(fmaxf(sum2 / cnt - mean * mean, 0));
    float thresh = fmaxf(6.0f * noise, 0.01f);

    // 2) send the click at the start of a block, then count samples
    for (int i = 0; i < BLOCK; i++) out[i] = (i < 3) ? CLICK_AMP : 0.0f;
    long counted = -1, n = 0;
    runBlock(out, err);   // click goes out in this block
    for (int i = 0; i < BLOCK; i++) out[i] = 0;
    // samples in this first block are older than the click, so start counting from the next block
    while (n < MAX_WAIT && counted < 0) {
      runBlock(out, err);
      for (int i = 0; i < BLOCK; i++) {
        if (counted < 0 && fabsf(err[i] - mean) > thresh) counted = n + i + BLOCK;
      }
      n += BLOCK;
    }
    if (counted < 0) {
      Serial.printf("Try %d: no click heard. Raise OUT_VOLUME or CLICK_AMP a little, or check the ERR mic.\n", t + 1);
    } else {
      float ms = 1000.0f * counted / SAMPLE_RATE;
      results[okCount++] = ms;
      Serial.printf("Try %d: delay = %ld samples = %.2f ms\n", t + 1, counted, ms);
    }
    idle(300);
  }
  if (okCount > 0) {
    float s = 0;
    for (int i = 0; i < okCount; i++) s += results[i];
    Serial.printf("AVERAGE delay: %.2f ms (sound travels about %.0f mm in that time)\n\n",
                  s / okCount, 343.0f * s / okCount);
  }
  Serial.println("Measuring again in 5 seconds...\n");
  idle(5000);
}
