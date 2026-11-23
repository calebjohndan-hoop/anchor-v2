// =====================================================================
//  ANCHOR v2 - audio for TWO ears (anchor_audio_v2.h)
//  Hardware: ANCHOR v2 main board (ESP32-S3-WROOM-1-N16R8) + right cup hub.
//
//  Each ear is wired exactly like v1, on its own I2S port:
//    LEFT  ear = I2S port 0: BCLK 5,  WS 6,  DOUT 7,  DIN 15   (same pins as v1)
//    RIGHT ear = I2S port 1: BCLK 17, WS 18, DOUT 21, DIN 16   (over the headband cable)
//  On each port, frame[0] = REF mic (L/R to GND, "REF" mic board)
//                frame[1] = ERR mic (L/R to 3.3 V, "ERR" mic board)
//  and the ear's MAX98357A plays the same sound on both channels, as in v1.
//
//  GPIO 8 (AMP_SD) has a pull-up on the board, so the amps are ON by default.
//  Pull it LOW to shut both amps down.
//
//  Same sample format and gains as v1's anchor_audio.h, so the AI training
//  clips and the cancelling settings carry over.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#ifndef SAMPLE_RATE
#define SAMPLE_RATE 16000
#endif

struct EarPins { int bclk, ws, dout, din; };
static const EarPins EAR_PINS[2] = { { 5, 6, 7, 15 }, { 17, 18, 21, 16 } };
static const char* const EAR_NAME[2] = { "LEFT", "RIGHT" };

#define PIN_AMP_SD      8
#define PIN_BUTTON      4
#define PIN_LED_STATUS  47
#define PIN_LED_MODE    48
#define PIN_VBAT        1      // 100k/100k divider: volts = 2 x pin
#define PIN_CHG_STAT    2      // high = charge complete, low = charging (through a divider)

#define CH_REF 0
#define CH_ERR 1

static int32_t anchor_out_gain_q15 = 0;   // speaker gain, 32768 = 1.0
static int32_t anchor_in_gain_q8 = 256;   // mic gain, 256 = 1.0 (0 dB)

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

static void anchorSetVolumes(int out_volume, int in_volume) {
  out_volume = constrain(out_volume, 0, 100);
  in_volume = constrain(in_volume, 0, 100);
  anchor_out_gain_q15 = (int32_t)(out_volume * 32768L / 100);
  anchor_in_gain_q8 = (int32_t)(256.0f * powf(10.0f, (0.24f * in_volume) / 20.0f));
}

static bool IRAM_ATTR onPortOverflow(i2s_chan_handle_t h, i2s_event_data_t* e, void* ctx);

// One I2S port = one ear (two mics + one amplifier), full duplex, master.
struct AnchorPort {
  i2s_chan_handle_t tx = nullptr, rx = nullptr;
  volatile uint32_t overflows = 0;
  int32_t rawIn[2 * 128];
  int32_t rawOut[2 * 128];

  bool begin(i2s_port_t port, const EarPins& p, int frames_per_dma, int dma_count) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = dma_count;
    chan_cfg.dma_frame_num = frames_per_dma;
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx, &rx) != ESP_OK) return false;
    // 32-bit slots: ICS-43434 (and INMP441) send 24-bit samples in 32-bit slots.
    i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = (gpio_num_t)p.bclk,
        .ws = (gpio_num_t)p.ws,
        .dout = (gpio_num_t)p.dout,
        .din = (gpio_num_t)p.din,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
      },
    };
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK) return false;
    gpio_pulldown_en((gpio_num_t)p.din);   // the two mics take turns on the data wire
    i2s_event_callbacks_t cbs = {};
    cbs.on_recv_q_ovf = onPortOverflow;
    i2s_channel_register_event_callback(rx, &cbs, this);
    if (i2s_channel_enable(tx) != ESP_OK) return false;
    if (i2s_channel_enable(rx) != ESP_OK) return false;
    memset(rawOut, 0, sizeof(rawOut));
    for (int i = 0; i < dma_count; i++) {
      size_t done = 0;
      int f = frames_per_dma > 128 ? 128 : frames_per_dma;
      i2s_channel_write(tx, rawOut, f * 2 * sizeof(int32_t), &done, 100);
    }
    return true;
  }

  // stereo int16 frames: [REF, ERR, REF, ERR, ...]
  size_t read(int16_t* buf, size_t frames) {
    size_t done_frames = 0;
    while (done_frames < frames) {
      size_t want = frames - done_frames;
      if (want > 128) want = 128;
      size_t got = 0;
      i2s_channel_read(rx, rawIn, want * 2 * sizeof(int32_t), &got, portMAX_DELAY);
      size_t n = got / (2 * sizeof(int32_t));
      for (size_t i = 0; i < n; i++) {
        int32_t l = rawIn[2 * i] >> 8;
        int32_t r = rawIn[2 * i + 1] >> 8;
        buf[2 * (done_frames + i)]     = sat16((int32_t)(((int64_t)l * anchor_in_gain_q8) >> 16));
        buf[2 * (done_frames + i) + 1] = sat16((int32_t)(((int64_t)r * anchor_in_gain_q8) >> 16));
      }
      done_frames += n;
      if (n == 0) break;
    }
    return done_frames;
  }

  size_t write(const int16_t* buf, size_t frames) {
    size_t done_frames = 0;
    while (done_frames < frames) {
      size_t want = frames - done_frames;
      if (want > 128) want = 128;
      for (size_t i = 0; i < 2 * want; i++) {
        int32_t s = ((int32_t)buf[2 * done_frames + i] * anchor_out_gain_q15) >> 15;
        rawOut[i] = s << 16;
      }
      size_t done = 0;
      i2s_channel_write(tx, rawOut, want * 2 * sizeof(int32_t), &done, portMAX_DELAY);
      size_t n = done / (2 * sizeof(int32_t));
      done_frames += n;
      if (n == 0) break;
    }
    return done_frames;
  }

  // Fill the speaker queue so the delay is the longest it can be (and therefore fixed).
  void primeSilence(int frames) {
    memset(rawOut, 0, sizeof(rawOut));
    for (int i = 0; i < 8; i++) {
      size_t done = 0;
      if (i2s_channel_write(tx, rawOut, frames * 2 * sizeof(int32_t), &done, 0) != ESP_OK ||
          done < frames * 2 * sizeof(int32_t)) break;
    }
  }
};

static bool IRAM_ATTR onPortOverflow(i2s_chan_handle_t h, i2s_event_data_t* e, void* ctx) {
  AnchorPort* p = (AnchorPort*)ctx;
  p->overflows = p->overflows + 1;
  return false;
}

static AnchorPort anchorPort[2];

static bool anchorAudioBegin(int out_volume, int in_volume, int frames_per_dma, int dma_count) {
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, HIGH);          // amps on
  anchorSetVolumes(out_volume, in_volume);
  for (int ear = 0; ear < 2; ear++) {
    if (!anchorPort[ear].begin(ear == 0 ? I2S_NUM_0 : I2S_NUM_1, EAR_PINS[ear], frames_per_dma, dma_count)) {
      Serial.printf("ERROR: I2S for the %s ear did not start.\n", EAR_NAME[ear]);
      return false;
    }
  }
  static int16_t junk[2 * 64];
  for (int i = 0; i < 13; i++)             // mics need a moment to wake up
    for (int ear = 0; ear < 2; ear++) anchorPort[ear].read(junk, 64);
  Serial.println("Audio ready: two ears (ICS-43434 mics + MAX98357A amps).");
  return true;
}

static void anchorAmpsEnable(bool on) { digitalWrite(PIN_AMP_SD, on ? HIGH : LOW); }

static float anchorBatteryVolts() { return 2.0f * analogReadMilliVolts(PIN_VBAT) / 1000.0f; }
