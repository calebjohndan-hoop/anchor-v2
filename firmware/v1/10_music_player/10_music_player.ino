// =====================================================================
// ANCHOR sketch 10 - play MUSIC from your laptop through the speaker
//
//   1. Upload this sketch (same board settings as the other sketches).
//   2. CLOSE the Serial Monitor.
//   3. On the laptop, in the tools folder:
//        python play_music.py --port COM17 --file song.wav
//
// How it works: the laptop script sends the song over the USB cable in small
// pieces (16 kHz, mono, 16-bit). The board asks for each piece by sending the
// letter N ("next"), so it never runs out and never overflows.
//
// Sound quality: 16 kHz means notes up to 8 kHz. That is fine for testing the
// speaker (a bit like a radio). It is not hi-fi.
//
// Needs the same shared/anchor_audio.h as the other sketches (copy it into this
// folder, like for the other sketches).
// =====================================================================
#include "anchor_audio.h"

#define BAUD         921600
#define DEFAULT_VOL  20      // speaker volume 0..100 (the script can change it)
#define MAX_VOL      60      // safety limit: the board never goes above this
#define BLOCK        256     // frames per audio write
#define CHUNK        1024    // samples the laptop sends each time (2048 bytes)
#define RING         16384   // about 1 second of sound
#define PREBUF       4096    // start playing when this many samples are waiting

static int16_t ring[RING];
static volatile uint32_t wrCount = 0;   // samples written by the serial side
static volatile uint32_t rdCount = 0;   // samples played by the audio side
static volatile bool playing = false;   // audio side: playing or waiting
static volatile bool streaming = false; // laptop has said "P" (play)

// ---------- audio side (runs on core 0) ----------
static void audioTask(void*) {
  static int16_t inb[BLOCK * 2];
  static int16_t outb[BLOCK * 2];
  for (;;) {
    audioRead(inb, BLOCK);                       // keeps the I2S clocks and the mic side flowing
    uint32_t have = wrCount - rdCount;
    if (!playing && have >= PREBUF) playing = true;
    if (playing && have < BLOCK) playing = false;   // ran dry: wait for more
    if (playing) {
      for (int i = 0; i < BLOCK; i++) {
        int16_t s = ring[(rdCount + i) % RING];
        outb[2 * i] = s;
        outb[2 * i + 1] = s;
      }
      rdCount = rdCount + BLOCK;
    } else {
      memset(outb, 0, sizeof(outb));             // silence
    }
    audioWrite(outb, BLOCK);
  }
}

static bool readExact(uint8_t* dst, size_t n, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  size_t got = 0;
  while (got < n) {
    int avail = Serial.available();
    if (avail > 0) {
      size_t want = n - got;
      if ((size_t)avail < want) want = (size_t)avail;
      size_t r = Serial.readBytes(dst + got, want);
      got += r;
      t0 = millis();
    } else if (millis() - t0 > timeoutMs) {
      return false;
    }
  }
  return true;
}

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(BAUD);
  delay(500);
  Serial.println("ANCHOR 10 - music player. Close the Serial Monitor and run play_music.py");
  if (!anchorAudioBegin(DEFAULT_VOL, 0, BLOCK, 6)) {
    while (true) delay(1000);
  }
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 0);
}

void loop() {
  // ---- idle: wait for the laptop to say P + volume ----
  if (!streaming) {
    if (Serial.available() && Serial.read() == 'P') {
      uint8_t v = 0;
      if (readExact(&v, 1, 500)) {
        if (v > MAX_VOL) v = MAX_VOL;
        anchorSetVolumes(v, 0);
        wrCount = rdCount = 0;
        playing = false;
        streaming = true;
      }
    }
    return;
  }

  // ---- streaming: ask for a piece when there is room ----
  uint32_t used = wrCount - rdCount;
  if (RING - used < CHUNK) { delay(2); return; }

  Serial.write('N');
  uint8_t head = 0;
  if (!readExact(&head, 1, 1000)) { streaming = false; return; }   // laptop went away
  if (head == 'Q') {                    // song finished
    streaming = false;
    return;
  }
  if (head != 'D') return;              // unknown byte: ignore and ask again

  static uint8_t buf[CHUNK * 2];
  if (!readExact(buf, CHUNK * 2, 1000)) { streaming = false; return; }
  for (int i = 0; i < CHUNK; i++) {
    int16_t s = (int16_t)(buf[2 * i] | (buf[2 * i + 1] << 8));
    ring[(wrCount + i) % RING] = s;
  }
  wrCount = wrCount + CHUNK;
}
