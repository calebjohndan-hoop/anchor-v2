// =====================================================================
//  ANCHOR - shared audio code (anchor_audio.h)
//  Hardware: ESP32-S3 (N16R8) + 2 x INMP441 digital mics + 1 x MAX98357A amplifier
//  Keep this file in the SAME folder as the .ino sketch that uses it.
//
//  Needs only: ESP32 Arduino core 3.x ("esp32 by Espressif Systems").
//  No extra library, no I2C, no codec settings.
//
//  How it works:
//   ONE I2S port runs both ways at once (full duplex). The ESP32 makes the
//   BCLK and WS clocks; both mics and the MAX98357A listen to the same clocks,
//   so recording and playback always stay in step.
//     - Both mics share one data wire (SD). The mic with L/R = GND talks in
//       the LEFT half of each frame, the mic with L/R = 3.3 V in the RIGHT half.
//     - The MAX98357A gets the speaker sound on its DIN pin and drives the
//       speaker directly (speaker on its two output screws, never to GND).
//       Its SD pin is tied to VIN, so it plays the LEFT channel. We always
//       write the same sound to both channels, so either setting works.
//
//  audioRead() gives stereo 16-bit frames:
//      frame[0] = LEFT  = REF mic (outside, L/R pin to GND)
//      frame[1] = RIGHT = ERR mic (inside,  L/R pin to 3.3 V)
//  audioWrite() takes stereo 16-bit frames for the MAX98357A.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "driver/i2s_std.h"   // part of the ESP32 Arduino core 3.x
#include "driver/gpio.h"

// ---------- Pins: must match the wiring in the build guide ----------
#define PIN_I2S_BCLK  5    // -> SCK of both mics + BCLK of MAX98357A
#define PIN_I2S_WS    6    // -> WS of both mics  + LRCLK of MAX98357A
#define PIN_I2S_DOUT  7    // -> DIN of MAX98357A (sound to the speaker)
#define PIN_I2S_DIN   15   // <- SD of both mics (joined together)

#define SAMPLE_RATE 16000

// If sketch 04 shows the REF and ERR bars the wrong way round, change 0 to 1.
#define SWAP_MIC_CHANNELS 0
#if SWAP_MIC_CHANNELS
  #define CH_REF 1
  #define CH_ERR 0
#else
  #define CH_REF 0
  #define CH_ERR 1
#endif

static i2s_chan_handle_t i2s_tx = nullptr;
static i2s_chan_handle_t i2s_rx = nullptr;

// Software volume (the MAX98357A has only a fixed gain, set by its GAIN pin)
static int32_t anchor_out_gain_q15 = 0;   // speaker gain, 32768 = 1.0
static int32_t anchor_in_gain_q8 = 256;   // mic gain, 256 = 1.0 (0 dB)

// ---------- small helpers ----------
static inline float fromPcm(int16_t s) { return (float)s / 32768.0f; }
static inline int16_t toPcm(float v) {
  if (v > 0.999f) v = 0.999f;
  if (v < -0.999f) v = -0.999f;
  return (int16_t)(v * 32767.0f);
}
static inline int16_t sat16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

// Counts how often mic data was lost because the code was too slow.
static volatile uint32_t i2s_rx_overflows = 0;
static bool IRAM_ATTR onRxOverflow(i2s_chan_handle_t h, i2s_event_data_t* e, void* ctx) {
  i2s_rx_overflows = i2s_rx_overflows + 1;
  return false;
}

// Volumes: 0..100, like before.
//   out_volume: speaker loudness, 0 = silent, 100 = full scale.  START LOW (10-20):
//               the MAX98357A is an amplifier and can be LOUD.
//   in_volume : mic gain, 0 = 0 dB ... 100 = +24 dB (done in software).
static void anchorSetVolumes(int out_volume, int in_volume) {
  if (out_volume < 0) out_volume = 0;
  if (out_volume > 100) out_volume = 100;
  if (in_volume < 0) in_volume = 0;
  if (in_volume > 100) in_volume = 100;
  anchor_out_gain_q15 = (int32_t)(out_volume * 32768L / 100);
  anchor_in_gain_q8 = (int32_t)(256.0f * powf(10.0f, (0.24f * in_volume) / 20.0f));
}

// ---------- I2S: the sound wires ----------
// frames_per_dma small (16) = low delay, for noise cancelling
// frames_per_dma big (256)  = never drops samples, for recording
static bool anchorI2SBegin(int frames_per_dma, int dma_count) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = dma_count;
  chan_cfg.dma_frame_num = frames_per_dma;
  chan_cfg.auto_clear = true;  // play silence if we are ever late
  if (i2s_new_channel(&chan_cfg, &i2s_tx, &i2s_rx) != ESP_OK) return false;

  // 32-bit slots: the INMP441 sends 24-bit samples inside 32-bit slots.
  // BCLK = 64 x 16 kHz = 1.024 MHz (the INMP441 accepts 0.5 to 3.2 MHz).
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,   // not needed: MAX98357A and INMP441 use only BCLK and WS
      .bclk = (gpio_num_t)PIN_I2S_BCLK,
      .ws = (gpio_num_t)PIN_I2S_WS,
      .dout = (gpio_num_t)PIN_I2S_DOUT,
      .din = (gpio_num_t)PIN_I2S_DIN,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };

  if (i2s_channel_init_std_mode(i2s_tx, &std_cfg) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(i2s_rx, &std_cfg) != ESP_OK) return false;

  // The two mics take turns on the SD wire. The INMP441 datasheet asks for a
  // pull-down on SD; we use the ESP32's built-in one.
  gpio_pulldown_en((gpio_num_t)PIN_I2S_DIN);

  i2s_event_callbacks_t cbs = {};
  cbs.on_recv_q_ovf = onRxOverflow;
  i2s_channel_register_event_callback(i2s_rx, &cbs, nullptr);

  if (i2s_channel_enable(i2s_tx) != ESP_OK) return false;
  if (i2s_channel_enable(i2s_rx) != ESP_OK) return false;

  // Fill the speaker queue with silence so the delay is always the same
  static int32_t zeros[2 * 256] = {0};
  for (int i = 0; i < dma_count; i++) {
    size_t done = 0;
    int f = frames_per_dma > 256 ? 256 : frames_per_dma;
    i2s_channel_write(i2s_tx, zeros, f * 2 * sizeof(int32_t), &done, 100);
  }
  return true;
}

// Call this once in setup(). Returns false if something failed.
static bool anchorAudioBegin(int out_volume, int in_volume,
                             int frames_per_dma = 16, int dma_count = 3) {
  anchorSetVolumes(out_volume, in_volume);
  if (!anchorI2SBegin(frames_per_dma, dma_count)) {
    Serial.println("ERROR: I2S did not start. Check the board settings (ESP32S3 Dev Module).");
    return false;
  }
  // Throw away the first 50 ms: the INMP441 needs a moment to wake up.
  static int32_t junk[2 * 64];
  size_t got;
  for (int i = 0; i < 13; i++) i2s_channel_read(i2s_rx, junk, sizeof(junk), &got, 100);
  Serial.println("Audio ready (INMP441 mics + MAX98357A).");
  return true;
}

// Read 'frames' stereo frames (L,R,L,R... as int16). Waits until they arrive.
static size_t audioRead(int16_t* buf, size_t frames) {
  static int32_t raw[2 * 128];
  size_t done_frames = 0;
  while (done_frames < frames) {
    size_t want = frames - done_frames;
    if (want > 128) want = 128;
    size_t got = 0;
    i2s_channel_read(i2s_rx, raw, want * 2 * sizeof(int32_t), &got, portMAX_DELAY);
    size_t n = got / (2 * sizeof(int32_t));
    for (size_t i = 0; i < n; i++) {
      // 24-bit sample sits in the top of the 32-bit slot -> 24-bit value -> apply gain -> 16-bit
      int32_t l = raw[2 * i] >> 8;
      int32_t r = raw[2 * i + 1] >> 8;
      buf[2 * (done_frames + i)]     = sat16((int32_t)(((int64_t)l * anchor_in_gain_q8) >> 16));
      buf[2 * (done_frames + i) + 1] = sat16((int32_t)(((int64_t)r * anchor_in_gain_q8) >> 16));
    }
    done_frames += n;
    if (n == 0) break;
  }
  return done_frames;
}

// Write 'frames' stereo frames (int16) to the MAX98357A.
static size_t audioWrite(const int16_t* buf, size_t frames) {
  static int32_t raw[2 * 128];
  size_t done_frames = 0;
  while (done_frames < frames) {
    size_t want = frames - done_frames;
    if (want > 128) want = 128;
    for (size_t i = 0; i < 2 * want; i++) {
      int32_t s = ((int32_t)buf[2 * done_frames + i] * anchor_out_gain_q15) >> 15;  // volume
      raw[i] = s << 16;   // 16-bit sample into the top of the 32-bit slot
    }
    size_t done = 0;
    i2s_channel_write(i2s_tx, raw, want * 2 * sizeof(int32_t), &done, portMAX_DELAY);
    size_t n = done / (2 * sizeof(int32_t));
    done_frames += n;
    if (n == 0) break;
  }
  return done_frames;
}
