// =====================================================================
// ANCHOR sketch 08 - test the AI sound classifier on the board (no ANC)
//
// Works with the BUILT-IN AI (anchor_ai.h + anchor_ai_model.h in this folder)
// or with YOUR Edge Impulse model (Arduino library). See "WHICH AI?" below.
// Prints the score of every label about once a second, the winner, and how
// many milliseconds the AI took.
// =====================================================================
#include "anchor_audio.h"

// ---------------------------------------------------------------------
// WHICH AI?  Change only ONE line:
//   - Built-in AI (starter, nothing to install): leave the next line as it is.
//   - Your Edge Impulse AI: add its library ZIP (guide Step 46), then remove
//     the two slashes at the start of the next line and put YOUR library name
//     there (it ends in _inferencing.h).
//#include <ANCHOR_inferencing.h>
// ---------------------------------------------------------------------
#ifndef EI_CLASSIFIER_FREQUENCY          // no Edge Impulse library included -> built-in AI
  #define ANCHOR_QUIET_DBFS -55.0f       // built-in AI only: softer than this = "quiet" (see sketch 04)
  #include "anchor_ai.h"
  #define ANCHOR_AI_NAME "built-in AI"
#else
  #define ANCHOR_AI_NAME "Edge Impulse AI"
#endif

#if EI_CLASSIFIER_FREQUENCY != 16000
#error "The model must use 16000 Hz audio. In Edge Impulse set Frequency to 16000 Hz (guide Step 44)."
#endif

#define OUT_VOLUME 0      // speaker silent
#define IN_VOLUME  10     // MUST be the same as when you recorded the clips (sketch 06)
#define BLOCK      256

static int16_t* window_buf;         // one window of REF mic audio
static int16_t blockBuf[BLOCK * 2];

static int getData(size_t offset, size_t length, float* out) {
  for (size_t i = 0; i < length; i++) out[i] = (float)window_buf[offset + i];
  return 0;
}

void setup() {
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ANCHOR 08 - AI classifier test (" ANCHOR_AI_NAME ")");
  window_buf = (int16_t*)malloc(EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(int16_t));
  if (!window_buf) { Serial.println("ERROR: not enough memory"); while (true) delay(1000); }
  if (!anchorAudioBegin(OUT_VOLUME, IN_VOLUME, BLOCK, 4)) {
    while (true) delay(1000);
  }
  Serial.printf("Model window: %d samples (%.2f s). Labels:", EI_CLASSIFIER_RAW_SAMPLE_COUNT,
                (float)EI_CLASSIFIER_RAW_SAMPLE_COUNT / EI_CLASSIFIER_FREQUENCY);
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) Serial.printf(" %s", ei_classifier_inferencing_categories[i]);
  Serial.println();
}

void loop() {
  // 1) record one window from the REF mic
  size_t filled = 0;
  while (filled < EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
    size_t got = audioRead(blockBuf, BLOCK);
    for (size_t i = 0; i < got && filled < EI_CLASSIFIER_RAW_SAMPLE_COUNT; i++)
      window_buf[filled++] = blockBuf[2 * i + CH_REF];
  }
  int pk = 0;
  for (int i = 0; i < EI_CLASSIFIER_RAW_SAMPLE_COUNT; i++) { int a = abs((int)window_buf[i]); if (a > pk) pk = a; }
  Serial.printf("peak %d, lost-audio count %lu\n", pk, (unsigned long)i2s_rx_overflows);

  // 2) ask the AI what it is
  signal_t sig;
  sig.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
  sig.get_data = &getData;
  ei_impulse_result_t result = { 0 };
  uint32_t t0 = millis();
  EI_IMPULSE_ERROR err = run_classifier(&sig, &result, false);
  uint32_t ms = millis() - t0;
  if (err != EI_IMPULSE_OK) { Serial.printf("run_classifier error %d\n", (int)err); return; }

  // 3) print the answer
  int best = 0;
  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    Serial.printf("%s %.2f   ", result.classification[i].label, result.classification[i].value);
    if (result.classification[i].value > result.classification[best].value) best = i;
  }
  Serial.printf("=> %s  (%lu ms)\n", result.classification[best].label, (unsigned long)ms);
}
