// =====================================================================
// ANCHOR sketch 03 - play a QUIET beep through the speaker
// Keep the speaker on the table, NOT on your ear, the first time.
// Expected: a soft 200 Hz hum, 1 second on, 1 second off.
// Serial Monitor (115200): type + or - and press Enter to change loudness.
// =====================================================================
#include "anchor_audio.h"

#define OUT_VOLUME 20     // speaker volume 0..100 (start low: the MAX98357A is loud)
#define IN_VOLUME  25     // mic gain 0..100 (not used here)
#define BLOCK      64

float amp = 0.03f;        // 3% of full scale = quiet
float phase = 0;
const float freq = 200.0f;
int16_t inbuf[BLOCK * 2], outbuf[BLOCK * 2];
uint32_t n = 0;

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 03 - quiet beep test");
  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, 4)) {
    while (true) delay(1000);
  }
  Serial.println("You should hear a soft hum: 1 s on, 1 s off. Type + or - to change.");
}

void loop() {
  audioRead(inbuf, BLOCK);  // keep the mic side flowing
  for (int i = 0; i < BLOCK; i++, n++) {
    bool on = ((n / SAMPLE_RATE) % 2) == 0;
    float s = on ? amp * sinf(phase) : 0.0f;
    phase += 2.0f * PI * freq / SAMPLE_RATE;
    if (phase > 2.0f * PI) phase -= 2.0f * PI;
    outbuf[2 * i] = toPcm(s);
    outbuf[2 * i + 1] = toPcm(s);
  }
  audioWrite(outbuf, BLOCK);

  if (Serial.available()) {
    char c = Serial.read();
    if (c == '+') amp *= 1.5f;
    if (c == '-') amp /= 1.5f;
    if (amp > 0.3f) amp = 0.3f;     // hard safety limit
    if (amp < 0.001f) amp = 0.001f;
    if (c == '+' || c == '-') Serial.printf("Beep level: %.3f of full scale\n", amp);
  }
}
