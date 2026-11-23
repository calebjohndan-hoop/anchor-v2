// =====================================================================
//  ANCHOR - noise cancelling engine, version 2 (anchor_anc2.h)
//  Keep this file in the SAME folder as 09_anchor_full.ino.
//
//  What is new compared with anchor_anc.h (version 1):
//   1. FEEDBACK NEUTRALISATION. The REF mic (outside) also hears our own
//      speaker. Version 1 ignored that, so the anti-noise went round and
//      round (speaker -> REF mic -> filter -> speaker) = whine / louder noise.
//      Now calibration learns TWO paths with the same hiss:
//          S = speaker -> ERR mic   (as before)
//          F = speaker -> REF mic   (the leak)
//      and while running we subtract "what the speaker put into the REF mic"
//      from the REF signal before using it.
//   2. BAND LIMIT. The cancelling filter only sees 60..400 Hz (steep cut).
//      Above that, a timing error of only 0.5 ms between calibration and
//      running turns "cancel" into "add" and the filter runs away (whine).
//      Low hum is also the only thing a 3.5 ms system can cancel.
//   3. The filter stops learning while the output is clipping, and the
//      ERR mic's DC offset is removed before learning.
//
//  Maths (feedforward leaky normalised FxLMS):
//    xc = x - F * out_history         clean reference (speaker removed)
//    xb = bandpass(xc)
//    y  = W * xb                      anti-noise
//    xf = S * xb                      filtered reference
//    W  = W*(1-leak) - mu * e * xf / power(xf)
// =====================================================================
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define ANC_NS 256   // taps of speaker->ERR model (16 ms at 16 kHz)
#define ANC_NF 192   // taps of speaker->REF leak model (12 ms)
#define ANC_NW 128   // taps of the cancelling filter (8 ms)

#ifndef ANC_BAND_LO_HZ
#define ANC_BAND_LO_HZ 60.0f
#endif
#ifndef ANC_BAND_HI_HZ
#define ANC_BAND_HI_HZ 400.0f
#endif
#ifndef ANC_FS
#define ANC_FS 16000.0f
#endif

struct AncBiquad {                       // one 2nd-order filter section
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void setLowpass(float fc, float fs, float q = 0.70710678f) {
    float w = 2.0f * (float)M_PI * fc / fs, c = cosf(w), al = sinf(w) / (2.0f * q);
    float a0 = 1.0f + al;
    b0 = (1 - c) * 0.5f / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
  }
  void setHighpass(float fc, float fs) {
    float w = 2.0f * (float)M_PI * fc / fs, c = cosf(w), al = sinf(w) / 1.41421356f;
    float a0 = 1.0f + al;
    b0 = (1 + c) * 0.5f / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
  }
  inline float step(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void clear() { z1 = z2 = 0; }
};

struct AncEngine2 {
  float S[ANC_NS];                      // speaker -> ERR mic
  float F[ANC_NF];                      // speaker -> REF mic (leak)
  float W[ANC_NW];                      // the cancelling filter
  // histories, stored twice so we never need "% N" (newest value is at [pos])
  float xb[2 * ANC_NS]; int xp = 0;     // band-limited clean REF
  float fb[2 * ANC_NW]; int fp = 0;     // filtered REF
  float ob[2 * ANC_NS]; int op = 0;     // what we really sent to the speaker
  float Pf = 0, Po = 0;
  uint32_t tick = 0;
  float eDc = 0;
  AncBiquad hp, lp, lp2;

  float mu = 0.01f;         // learning speed (bigger = faster, less stable)
  float leak = 0.00002f;    // slowly forgets, stops drift
  float mu_s = 0.02f;       // calibration learning speed
  float cal_amp = 0.06f;    // calibration hiss loudness (before OUT_VOLUME)
  bool neutralise = true;   // subtract the speaker leak from the REF mic

  double calResid = 0, calTotal = 0;
  uint32_t rng = 0x1234567u;

  void begin() {
    hp.setHighpass(ANC_BAND_LO_HZ, ANC_FS);
    lp.setLowpass(ANC_BAND_HI_HZ, ANC_FS, 0.54119610f);   // two sections = steep 4th-order
    lp2.setLowpass(ANC_BAND_HI_HZ, ANC_FS, 1.30656296f);
    resetAll();
  }
  void resetW() { memset(W, 0, sizeof(W)); }
  void resetAll() {
    memset(S, 0, sizeof(S)); memset(F, 0, sizeof(F)); memset(W, 0, sizeof(W));
    memset(xb, 0, sizeof(xb)); memset(fb, 0, sizeof(fb)); memset(ob, 0, sizeof(ob));
    xp = fp = op = 0; Pf = Po = 0; eDc = 0;
    hp.clear(); lp.clear(); lp2.clear();
    calResid = calTotal = 0;
  }
  static inline void push(float* b, int N, int& p, float v) {
    p = (p == 0) ? N - 1 : p - 1;
    b[p] = v;
    b[p + N] = v;
  }
  inline float white() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(int32_t)rng / 2147483648.0f;
  }

  // ---- calibration: once per sample. e = ERR mic, x = REF mic. Returns the hiss to play.
  float calibrateStep(float e, float x) {
    float u = cal_amp * white();
    float old = ob[op + ANC_NS - 1];
    push(ob, ANC_NS, op, u);
    Po += u * u - old * old;
    const float* h = &ob[op];
    if ((++tick & 4095) == 0 || Po < 0) { Po = 0; for (int k = 0; k < ANC_NS; k++) Po += h[k] * h[k]; }
    float inv = mu_s / (1e-7f + Po);
    float ys = 0;
    for (int k = 0; k < ANC_NS; k++) ys += S[k] * h[k];
    float rs = e - ys, gs = rs * inv;
    for (int k = 0; k < ANC_NS; k++) S[k] += gs * h[k];
    float yf = 0;
    for (int k = 0; k < ANC_NF; k++) yf += F[k] * h[k];
    float gf = (x - yf) * inv;
    for (int k = 0; k < ANC_NF; k++) F[k] += gf * h[k];
    calResid += (double)rs * rs;
    calTotal += (double)e * e;
    return u;
  }
  void calibrationStatsReset() { calResid = 0; calTotal = 0; }
  float calibrationQualityDb() {       // more negative = better
    if (calTotal <= 0) return 0;
    return 10.0f * log10f((float)(calResid / calTotal) + 1e-12f);
  }
  int modelPeakIndex() {               // rough speaker -> ERR delay in samples
    int best = 0; float bv = 0;
    for (int k = 0; k < ANC_NS; k++) if (fabsf(S[k]) > bv) { bv = fabsf(S[k]); best = k; }
    return best;
  }
  // How loud the speaker is at the REF mic compared with the ERR mic (rough).
  // -20 dB or lower = good seal.  Near 0 dB = the REF mic hears the speaker almost
  // as well as the ERR mic does: seal the cup / move the REF mic.
  float leakDb() {
    double s = 0, f = 0;
    for (int k = 0; k < ANC_NS; k++) s += (double)S[k] * S[k];
    for (int k = 0; k < ANC_NF; k++) f += (double)F[k] * F[k];
    return 10.0f * log10f((float)(f / (s + 1e-20)) + 1e-12f);
  }

  // ---- running: call these three, in this order, once per sample ----
  // 1) REF mic with our own speaker sound removed
  inline float cleanRef(float x) {
    if (!neutralise) return x;
    const float* h = &ob[op];          // h[0] = the sample we sent last time
    float s = 0;
    for (int k = 0; k < ANC_NF - 1; k++) s += F[k + 1] * h[k];
    return x - s;
  }
  // 2) anti-noise for this sample
  inline float antiNoise(float xc) {
    float v = lp2.step(lp.step(hp.step(xc)));
    push(xb, ANC_NS, xp, v);
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
    if (!isfinite(y)) { resetW(); hp.clear(); lp.clear(); lp2.clear(); y = 0; }
    return y;
  }
  // 3) tell the engine what really went to the speaker, and let it learn
  inline void finish(float out, float e, bool adapt) {
    push(ob, ANC_NS, op, out);
    eDc += 0.001f * (e - eDc);         // remove the mic's DC offset
    if (adapt) {
      const float* fh = &fb[fp];
      float g = mu * (e - eDc) / (1e-6f + Pf);
      float keep = 1.0f - leak;
      for (int k = 0; k < ANC_NW; k++) W[k] = W[k] * keep - g * fh[k];
    }
  }
};
