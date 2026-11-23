// =====================================================================
// ANCHOR sketch 04 - check both microphones
// Expected: two bars. Tap or talk near the REF mic -> REF bar jumps.
// Then do the same near the ERR mic -> ERR bar jumps.
// "DC" should be near 0. A big DC number means a damaged mic or a bad wire.
// If the bars are the wrong way round, set SWAP_MIC_CHANNELS to 1 in anchor_audio.h.
// =====================================================================
#include "anchor_audio.h"

#define OUT_VOLUME 0      // speaker silent for this test
#define IN_VOLUME  10     // mic gain 0..100 (0 dB..+24 dB). Raise if bars are tiny, lower if CLIP shows.
#define BLOCK      160    // 10 ms

int16_t buf[BLOCK * 2];

void bar(const char* name, float rms, float peak, float dc) {
  float db = 20.0f * log10f(rms + 1e-9f);
  int len = (int)((db + 80.0f) / 2.0f);   // -80 dB = empty, 0 dB = full
  if (len < 0) len = 0;
  if (len > 40) len = 40;
  Serial.printf("%s %6.1f dBFS |", name, db);
  for (int i = 0; i < 40; i++) Serial.print(i < len ? '#' : ' ');
  Serial.printf("| DC %+6.3f%s\n", dc, peak > 0.98f ? "  CLIP!" : "");
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 04 - mic check");
  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, 4)) {
    while (true) delay(1000);
  }
}

void loop() {
  double s2[2] = {0, 0}, s1[2] = {0, 0};
  float pk[2] = {0, 0};
  int frames = 0;
  for (int b = 0; b < 20; b++) {              // 20 x 10 ms = 0.2 s
    size_t got = audioRead(buf, BLOCK);
    for (size_t i = 0; i < got; i++) {
      for (int ch = 0; ch < 2; ch++) {
        float v = fromPcm(buf[2 * i + ch]);
        s1[ch] += v;
        s2[ch] += v * v;
        if (fabsf(v) > pk[ch]) pk[ch] = fabsf(v);
      }
    }
    frames += got;
  }
  for (int ch = 0; ch < 2; ch++) {
    float dc = s1[ch] / frames;
    float var = s2[ch] / frames - dc * dc;
    float rms = sqrtf(var > 0 ? var : 0);
    bar(ch == CH_REF ? "REF" : "ERR", rms, pk[ch], dc);
  }
  Serial.println();
}
