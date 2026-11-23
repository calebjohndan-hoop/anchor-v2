// =====================================================================
//  ANCHOR - noise cancelling engine (anchor_anc.h)
//  Keep this file in the SAME folder as the .ino sketch that uses it.
//
//  Feedforward FxLMS (filtered-x least mean squares):
//    x = REF mic (noise outside), e = ERR mic (what is left at the ear)
//    y = W * x          -> played by the speaker ("anti-noise")
//    x' = S * x         -> x filtered by our model of speaker-to-ERR-mic path
//    W = W - mu * e * x' / power(x')     (normalised update)
//
//  S (the "secondary path") is learned first by playing quiet hiss
//  for a few seconds: calibrateStep().
// =====================================================================
#pragma once
#include <math.h>
#include <string.h>

#define ANC_NS 256   // taps of the speaker->ERR-mic model (256 = 16 ms at 16 kHz)
#define ANC_NW 128   // taps of the cancelling filter     (128 = 8 ms)

struct AncEngine {
  // filters
  float S[ANC_NS];
  float W[ANC_NW];
  // histories, stored twice so we never need "% N" (newest value is at [pos])
  float xb[2 * ANC_NS];  int xp = 0;   // REF history
  float fb[2 * ANC_NW];  int fp = 0;   // filtered REF history
  float ub[2 * ANC_NS];  int up = 0;   // hiss history (calibration)
  float Pf = 0;                        // power of filtered REF over ANC_NW samples
  float Pu = 0;                        // power of hiss over ANC_NS samples
  uint32_t tick = 0;                   // to refresh Pf/Pu exactly now and then

  float mu = 0.005f;        // cancelling step size (bigger = faster but less stable)
  float leak = 0.00002f;    // slowly forgets, stops the filter drifting
  float mu_s = 0.02f;       // calibration step size
  float cal_amp = 0.02f;    // calibration hiss loudness (2% of full scale)

  // calibration quality numbers
  double calResid = 0, calTotal = 0;
  uint32_t rng = 0x1234567u;

  void resetW() { memset(W, 0, sizeof(W)); }

  void resetAll() {
    memset(S, 0, sizeof(S));  memset(W, 0, sizeof(W));
    memset(xb, 0, sizeof(xb)); memset(fb, 0, sizeof(fb)); memset(ub, 0, sizeof(ub));
    xp = fp = up = 0; Pf = Pu = 0;
    calResid = calTotal = 0;
  }

  static inline void push(float* b, int N, int& p, float v) {
    p = (p == 0) ? N - 1 : p - 1;
    b[p] = v;
    b[p + N] = v;
  }

  inline float white() {            // fast random number in [-1, 1]
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(int32_t)rng / 2147483648.0f;
  }

  // ---- calibration: call once per sample. e = ERR mic. Returns the hiss to play.
  float calibrateStep(float e) {
    float u = cal_amp * white();
    float old = ub[up + ANC_NS - 1];
    push(ub, ANC_NS, up, u);
    Pu += u * u - old * old;
    const float* h = &ub[up];
    if ((++tick & 4095) == 0 || Pu < 0) { Pu = 0; for (int k = 0; k < ANC_NS; k++) Pu += h[k] * h[k]; }
    float yhat = 0;
    for (int k = 0; k < ANC_NS; k++) yhat += S[k] * h[k];
    float r = e - yhat;
    float g = mu_s * r / (1e-7f + Pu);
    for (int k = 0; k < ANC_NS; k++) S[k] += g * h[k];
    calResid += (double)r * r;
    calTotal += (double)e * e;
    return u;
  }

  void calibrationStatsReset() { calResid = 0; calTotal = 0; }

  // How well S explains the ERR mic during calibration (more negative = better)
  float calibrationQualityDb() {
    if (calTotal <= 0) return 0;
    return 10.0f * log10f((float)(calResid / calTotal) + 1e-12f);
  }

  // Where the model has its biggest tap = rough delay in samples
  int modelPeakIndex() {
    int best = 0; float bv = 0;
    for (int k = 0; k < ANC_NS; k++) if (fabsf(S[k]) > bv) { bv = fabsf(S[k]); best = k; }
    return best;
  }

  // ---- cancelling: call once per sample. x = REF mic, e = ERR mic.
  // adapt = true lets W learn. Returns the anti-noise to play.
  float ancStep(float x, float e, bool adapt) {
    push(xb, ANC_NS, xp, x);
    const float* xh = &xb[xp];

    float y = 0;
    for (int k = 0; k < ANC_NW; k++) y += W[k] * xh[k];

    float xf = 0;
    for (int k = 0; k < ANC_NS; k++) xf += S[k] * xh[k];
    float old = fb[fp + ANC_NW - 1];
    push(fb, ANC_NW, fp, xf);
    Pf += xf * xf - old * old;
    if ((++tick & 4095) == 0 || Pf < 0) {
      Pf = 0;
      for (int k = 0; k < ANC_NW; k++) Pf += fb[fp + k] * fb[fp + k];
    }

    if (adapt) {
      const float* fh = &fb[fp];
      float g = mu * e / (1e-7f + Pf);
      float keep = 1.0f - leak;
      for (int k = 0; k < ANC_NW; k++) W[k] = W[k] * keep - g * fh[k];
    }
    if (!isfinite(y)) { resetW(); y = 0; }
    return y;
  }
};
