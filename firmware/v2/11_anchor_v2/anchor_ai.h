// =====================================================================
//  ANCHOR - built-in sound classifier (anchor_ai.h)
//  A small AI that runs on the ESP32 without Edge Impulse.
//  It copies the Edge Impulse function names, so sketches 08 and 09 work
//  with either one: just change the #include line.
//
//  Needs anchor_ai_model.h (made by tools/train_anchor_ai.py) in the same folder.
//
//  How it decides, every time run_classifier() is called on 1 s of sound:
//   1. If the sound is very soft (below ANCHOR_QUIET_DBFS)  -> "quiet"
//   2. Otherwise: 61 short slices -> FFT -> 32 pitch bands (mel)
//      -> 154 numbers (band shape, how much / how fast bands change,
//         how "peaky" the sound is, how its loudness repeats)
//      -> 1200 small decision trees (gradient boosting) -> scores for
//         alarm, machine, other, voice
// =====================================================================
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include "anchor_ai_model.h"

// Sound softer than this (dB of full scale, over 1 s) counts as "quiet".
// Use sketch 04 to see your REF level in a quiet room and set this a few dB above it.
#ifndef ANCHOR_QUIET_DBFS
#define ANCHOR_QUIET_DBFS -55.0f
#endif

#define EI_CLASSIFIER_FREQUENCY 16000
#define EI_CLASSIFIER_RAW_SAMPLE_COUNT 16000
#define EI_CLASSIFIER_LABEL_COUNT 5
typedef int EI_IMPULSE_ERROR;
#define EI_IMPULSE_OK 0
#define EI_IMPULSE_ALLOC_FAILED -1

static const char* const ei_classifier_inferencing_categories[EI_CLASSIFIER_LABEL_COUNT] = {
  "alarm", "machine", "other", "quiet", "voice" };

typedef struct {
  std::function<int(size_t offset, size_t length, float* out_ptr)> get_data;
  size_t total_length;
} signal_t;

typedef struct { const char* label; float value; } ei_impulse_result_classification_t;
typedef struct {
  ei_impulse_result_classification_t classification[EI_CLASSIFIER_LABEL_COUNT];
  float level_dbfs;     // loudness of the window (extra, not in Edge Impulse)
} ei_impulse_result_t;

// ---------------- internal ----------------
#define AAI_FS 16000
#define AAI_WIN 16000
#define AAI_NFFT 512
#define AAI_HOP 256
#define AAI_NFRAMES (1 + (AAI_WIN - AAI_NFFT) / AAI_HOP)
#define AAI_TON_LO 10
#define AAI_TON_HI 128

struct AaiState {
  float* x = nullptr;                 // 1 s of sound, scaled to -1..1
  float hann[AAI_NFFT];
  float cosT[AAI_NFFT / 2], sinT[AAI_NFFT / 2];
  uint16_t rev[AAI_NFFT];
  float re[AAI_NFFT], im[AAI_NFFT];
  float db[AAI_NFRAMES][AAI_NMEL];
  float ton[AAI_NFRAMES], flat[AAI_NFRAMES];
  bool ready = false;
};
static AaiState aai;

static bool aaiInit() {
  if (aai.ready) return true;
  aai.x = (float*)malloc(AAI_WIN * sizeof(float));
  if (!aai.x) return false;
  for (int n = 0; n < AAI_NFFT; n++) aai.hann[n] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * n / AAI_NFFT);
  for (int k = 0; k < AAI_NFFT / 2; k++) {
    aai.cosT[k] = cosf(2.0f * (float)M_PI * k / AAI_NFFT);
    aai.sinT[k] = -sinf(2.0f * (float)M_PI * k / AAI_NFFT);
  }
  int bits = 0;
  while ((1 << bits) < AAI_NFFT) bits++;
  for (int i = 0; i < AAI_NFFT; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    aai.rev[i] = r;
  }
  aai.ready = true;
  return true;
}

static void aaiFft() {   // in-place radix-2 FFT on aai.re / aai.im
  for (int i = 0; i < AAI_NFFT; i++) {
    int j = aai.rev[i];
    if (j > i) {
      float t = aai.re[i]; aai.re[i] = aai.re[j]; aai.re[j] = t;
      t = aai.im[i]; aai.im[i] = aai.im[j]; aai.im[j] = t;
    }
  }
  for (int len = 2; len <= AAI_NFFT; len <<= 1) {
    int half = len >> 1, step = AAI_NFFT / len;
    for (int i = 0; i < AAI_NFFT; i += len) {
      for (int k = 0; k < half; k++) {
        float wr = aai.cosT[k * step], wi = aai.sinT[k * step];
        int a = i + k, b = a + half;
        float tr = aai.re[b] * wr - aai.im[b] * wi;
        float ti = aai.re[b] * wi + aai.im[b] * wr;
        aai.re[b] = aai.re[a] - tr; aai.im[b] = aai.im[a] - ti;
        aai.re[a] += tr; aai.im[a] += ti;
      }
    }
  }
}

// Turn 1 s of sound (in aai.x) into the AAI_NFEAT features (same maths as train_anchor_ai.py)
static void aaiFeatures(float* f) {
  for (int t = 0; t < AAI_NFRAMES; t++) {
    const float* s = aai.x + t * AAI_HOP;
    for (int n = 0; n < AAI_NFFT; n++) { aai.re[n] = s[n] * aai.hann[n]; aai.im[n] = 0; }
    aaiFft();
    for (int k = 0; k <= AAI_NFFT / 2; k++) aai.re[k] = aai.re[k] * aai.re[k] + aai.im[k] * aai.im[k];  // power
    int wpos = 0;
    for (int m = 0; m < AAI_NMEL; m++) {
      float e = 0;
      int b0 = AAI_MEL_START[m];
      for (int c = 0; c < AAI_MEL_COUNT[m]; c++) e += AAI_MEL_W[wpos + c] * aai.re[b0 + c];
      wpos += AAI_MEL_COUNT[m];
      aai.db[t][m] = 10.0f * log10f(e + 1e-10f);
    }
    float mx = 0, am = 0, lg = 0;
    for (int k = AAI_TON_LO; k <= AAI_TON_HI; k++) {
      float v = aai.re[k];
      if (v > mx) mx = v;
      am += v;
      lg += logf(v + 1e-12f);
    }
    const int nb = AAI_TON_HI - AAI_TON_LO + 1;
    am /= nb;
    float gm = expf(lg / nb);
    aai.ton[t] = 10.0f * log10f((mx + 1e-12f) / (am + 1e-12f));
    aai.flat[t] = 10.0f * log10f((gm + 1e-12f) / (am + 1e-12f));
  }
  float meanAll = 0;
  for (int m = 0; m < AAI_NMEL; m++) {
    float sum = 0, sum2 = 0, dif = 0, mxv = -1e30f;
    for (int t = 0; t < AAI_NFRAMES; t++) {
      float v = aai.db[t][m];
      sum += v; sum2 += v * v;
      if (v > mxv) mxv = v;
      if (t > 0) dif += fabsf(v - aai.db[t - 1][m]);
    }
    float mean = sum / AAI_NFRAMES;
    float var = sum2 / AAI_NFRAMES - mean * mean;
    f[m] = mean;
    f[AAI_NMEL + m] = sqrtf(var > 0 ? var : 0);
    f[2 * AAI_NMEL + m] = dif / (AAI_NFRAMES - 1);
    f[3 * AAI_NMEL + m] = mxv - mean;
    meanAll += mean;
  }
  meanAll /= AAI_NMEL;
  for (int m = 0; m < AAI_NMEL; m++) f[m] -= meanAll;

  // tonality statistics
  float tm = 0, t2 = 0, tmx = -1e30f, fm = 0, f2 = 0, fmn = 1e30f;
  for (int t = 0; t < AAI_NFRAMES; t++) {
    tm += aai.ton[t]; t2 += aai.ton[t] * aai.ton[t]; if (aai.ton[t] > tmx) tmx = aai.ton[t];
    fm += aai.flat[t]; f2 += aai.flat[t] * aai.flat[t]; if (aai.flat[t] < fmn) fmn = aai.flat[t];
  }
  tm /= AAI_NFRAMES; fm /= AAI_NFRAMES;
  float tv = t2 / AAI_NFRAMES - tm * tm, fv = f2 / AAI_NFRAMES - fm * fm;
  int o = 4 * AAI_NMEL;
  f[o++] = tm; f[o++] = sqrtf(tv > 0 ? tv : 0); f[o++] = tmx;
  f[o++] = fm; f[o++] = sqrtf(fv > 0 ? fv : 0); f[o++] = fmn;

  // modulation: how the loudness of 4 pitch groups repeats (about 1 Hz steps)
  static const int lo[5] = { 1, 3, 5, 9, 17 }, hi[5] = { 2, 4, 8, 16, 30 };
  for (int g = 0; g < 4; g++) {
    float e[AAI_NFRAMES], em = 0;
    for (int t = 0; t < AAI_NFRAMES; t++) {
      float a = 0;
      for (int m = 8 * g; m < 8 * g + 8; m++) a += aai.db[t][m];
      e[t] = a / 8; em += e[t];
    }
    em /= AAI_NFRAMES;
    for (int t = 0; t < AAI_NFRAMES; t++) e[t] -= em;
    float X[31], tot = 0;
    for (int k = 1; k <= 30; k++) {
      float cr = 0, ci = 0;
      for (int t = 0; t < AAI_NFRAMES; t++) {
        float ang = 2.0f * (float)M_PI * (float)((k * t) % AAI_NFRAMES) / AAI_NFRAMES;
        cr += e[t] * cosf(ang); ci -= e[t] * sinf(ang);
      }
      X[k] = cr * cr + ci * ci; tot += X[k];
    }
    tot += 1e-9f;
    for (int r = 0; r < 5; r++) {
      float sum = 0;
      for (int k = lo[r]; k <= hi[r]; k++) sum += X[k];
      f[o++] = sum / tot;
    }
  }
}

// Gradient-boosted trees -> softmax probabilities
static void aaiTrees(const float* f, float* prob) {
  float raw[AAI_NCLASS];
  for (int c = 0; c < AAI_NCLASS; c++) raw[c] = AAI_BASE[c];
  for (int tr = 0; tr < AAI_NTREES; tr++) {
    uint32_t base = AAI_TREE_START[tr], n = 0;
    while (AAI_NODE_FEAT[base + n] >= 0) {
      n = (f[AAI_NODE_FEAT[base + n]] <= AAI_NODE_THR[base + n]) ? AAI_NODE_LEFT[base + n] : AAI_NODE_RIGHT[base + n];
    }
    raw[AAI_TREE_CLASS[tr]] += AAI_NODE_THR[base + n];   // leaf value
  }
  float mx = raw[0];
  for (int c = 1; c < AAI_NCLASS; c++) if (raw[c] > mx) mx = raw[c];
  float s = 0;
  for (int c = 0; c < AAI_NCLASS; c++) { raw[c] = expf(raw[c] - mx); s += raw[c]; }
  for (int c = 0; c < AAI_NCLASS; c++) prob[c] = raw[c] / s;
}

// Same call as Edge Impulse. The signal gives raw int16 values as floats.
static EI_IMPULSE_ERROR run_classifier(signal_t* sig, ei_impulse_result_t* res, bool debug = false) {
  (void)debug;
  if (!aaiInit()) return EI_IMPULSE_ALLOC_FAILED;
  size_t n = sig->total_length < AAI_WIN ? sig->total_length : AAI_WIN;
  for (size_t o = 0; o < n; o += 512) {
    size_t len = (n - o) < 512 ? (n - o) : 512;
    sig->get_data(o, len, aai.x + o);
  }
  for (size_t i = n; i < AAI_WIN; i++) aai.x[i] = 0;
  double p = 0;
  for (int i = 0; i < AAI_WIN; i++) { aai.x[i] *= (1.0f / 32768.0f); p += (double)aai.x[i] * aai.x[i]; }
  float level = 10.0f * log10f((float)(p / AAI_WIN) + 1e-12f);
  res->level_dbfs = level;

  float prob[AAI_NCLASS] = {0};
  float quiet = 0;
  if (level < ANCHOR_QUIET_DBFS) {
    quiet = 1.0f;
  } else {
    float feat[AAI_NFEAT];
    aaiFeatures(feat);
    aaiTrees(feat, prob);
  }
  // order: alarm, machine, other, quiet, voice
  const float vals[EI_CLASSIFIER_LABEL_COUNT] = { prob[0], prob[1], prob[2], quiet, prob[3] };
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    res->classification[i].label = ei_classifier_inferencing_categories[i];
    res->classification[i].value = vals[i];
  }
  return EI_IMPULSE_OK;
}
