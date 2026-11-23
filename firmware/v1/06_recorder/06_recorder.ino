// =====================================================================
// ANCHOR sketch 06 - record sound clips from the REF mic to your laptop
// Use it with tools/record_clips.py (the laptop side).
// Close the Arduino Serial Monitor first - only one program can use the port.
//
// How it works: the laptop sends "R10" (record 10 s). The board answers
// "AUD0", the number of samples, the raw 16-bit samples, then "END0".
// =====================================================================
#include "anchor_audio.h"

#define OUT_VOLUME 0       // speaker silent
#define IN_VOLUME  10      // SAME mic gain you will use later. Do not change it after recording!
#define BLOCK      256
#define BAUD       921600

int16_t buf[BLOCK * 2];
int16_t mono[BLOCK];

void setup() {
  Serial.setTxBufferSize(16384);
  Serial.setRxBufferSize(256);
  Serial.begin(BAUD);
  delay(500);
  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, 8)) {
    while (true) delay(1000);
  }
  Serial.println("ANCHOR 06 recorder ready. Waiting for the laptop...");
}

void sendClip(int seconds) {
  uint32_t total = (uint32_t)seconds * SAMPLE_RATE;
  Serial.write((const uint8_t*)"AUD0", 4);
  Serial.write((const uint8_t*)&total, 4);       // little-endian on ESP32
  uint32_t sent = 0;
  while (sent < total) {
    size_t got = audioRead(buf, BLOCK);
    size_t n = got;
    if (sent + n > total) n = total - sent;
    for (size_t i = 0; i < n; i++) mono[i] = buf[2 * i + CH_REF];
    Serial.write((const uint8_t*)mono, n * 2);
    sent += n;
  }
  Serial.write((const uint8_t*)"END0", 4);
  Serial.flush(); 
}

String line;
uint32_t lastLevel = 0;
double acc = 0; uint32_t accN = 0;

void loop() {
  // keep reading the mic so the data is always fresh
  size_t got = audioRead(buf, BLOCK);
  for (size_t i = 0; i < got; i++) { float v = fromPcm(buf[2 * i + CH_REF]); acc += v * v; accN++; }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 1 && line[0] == 'R') {
        int sec = line.substring(1).toInt();
        if (sec < 1) sec = 1;
        if (sec > 60) sec = 60;
        sendClip(sec);
      }
      line = "";
    } else if (line.length() < 16) {
      line += c;
    }
  }

  if (millis() - lastLevel > 2000 && accN > 0) {     // level report while idle
    Serial.printf("LEVEL REF %.1f dBFS\n", 10.0f * log10f(acc / accN + 1e-12));
    acc = 0; accN = 0; lastLevel = millis();
  }
}
