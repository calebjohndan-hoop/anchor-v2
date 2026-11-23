// =====================================================================
// ANCHOR sketch 02 - are both INMP441 mics talking on the I2S wires?
// Copy anchor_audio.h into this folder first.
//
// Expected, every second:
//   clock: 16000 frames/s
//   LEFT  (REF, L/R=GND):  OK   (changes, level -70 dBFS or louder)
//   RIGHT (ERR, L/R=3V3):  OK
// "SILENT" = that mic sends only zeros: check its 3.3 V, GND, SD wire and L/R pin.
// "STUCK"  = the data never changes: SD wire touching 3.3 V or GND, or broken mic.
// Both SILENT = check the BCLK (GPIO 5) and WS (GPIO 6) wires.
// =====================================================================
#include "anchor_audio.h"

static int32_t raw[2 * 256];

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("ANCHOR 02 - INMP441 wiring check");
  if (!anchorAudioBegin(0, 0, 256, 4)) {
    while (true) delay(1000);
  }
}

void loop() {
  uint32_t t0 = millis();
  uint32_t frames = 0;
  int32_t first[2] = {0, 0};
  bool changed[2] = {false, false}, nonzero[2] = {false, false};
  double sum2[2] = {0, 0};
  while (millis() - t0 < 1000) {
    size_t got = 0;
    i2s_channel_read(i2s_rx, raw, sizeof(raw), &got, portMAX_DELAY);
    size_t n = got / (2 * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) {
      for (int ch = 0; ch < 2; ch++) {
        int32_t v = raw[2 * i + ch] >> 8;          // 24-bit sample
        if (frames == 0 && i == 0) first[ch] = v;
        if (v != first[ch]) changed[ch] = true;
        if (v != 0) nonzero[ch] = true;
        float f = v / 8388608.0f;
        sum2[ch] += f * f;
      }
    }
    frames += n;
  }
  uint32_t ms = millis() - t0;
  Serial.printf("clock: %lu frames/s (should be about %d)\n", (unsigned long)(frames * 1000UL / ms), SAMPLE_RATE);
  const char* name[2] = {"LEFT  (REF, L/R=GND)", "RIGHT (ERR, L/R=3V3)"};
  for (int ch = 0; ch < 2; ch++) {
    float db = 10.0f * log10f(sum2[ch] / (frames ? frames : 1) + 1e-12f);
    const char* st = !nonzero[ch] ? "SILENT - check this mic" : (!changed[ch] ? "STUCK - check SD wire" : "OK");
    Serial.printf("%s: %-24s level %6.1f dBFS\n", name[ch], st, db);
  }
  Serial.println();
}
