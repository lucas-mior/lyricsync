#if !defined(CTC_MODEL_H)
#define CTC_MODEL_H

#include "cbase.h"
#include "errors.h"
#include "ctc_audio.h"
#define LRC_CTC_MODEL_DEFAULT_INPUTS_TO_LOGITS_RATIO 320
#define LRC_CTC_MODEL_DEFAULT_WINDOW_SECONDS 30
#define LRC_CTC_MODEL_DEFAULT_CONTEXT_SECONDS 2
#define LRC_CTC_MODEL_INPUT_RANK 2

typedef struct LrcCtcModelConfig {
    int32 sample_rate;
    int32 inputs_to_logits_ratio;
    int32 window_seconds;
    int32 context_seconds;
} LrcCtcModelConfig;

typedef struct LrcCtcModelInputResult {
    LrcResultHeader header;

    int64 sample_index;
} LrcCtcModelInputResult;

typedef struct LrcCtcModelIoInfo {
    int64 shape[LRC_CTC_MODEL_INPUT_RANK];

    int32 shape_len;
    int32 count;
    bool is_float32;
} LrcCtcModelIoInfo;

typedef struct LrcCtcModelChunk {
    int64 source_start_frame;
    int64 source_frame_count;
    int64 padded_start_frame;
    int64 padded_frame_count;
    int64 left_context_frames;
    int64 right_context_frames;
    int64 valid_output_start_frame;
    int64 valid_output_frame_count;

    int64 raw_emission_start;
    int64 raw_emission_count;
    int64 trim_left_emissions;
    int64 trim_right_emissions;
    int64 kept_emission_start;
    int64 kept_emission_count;
} LrcCtcModelChunk;

typedef struct LrcCtcModelInput {
    float *samples;

    int64 sample_count;
    int64 original_sample_count;
    int64 extension_sample_count;
    int64 window_sample_count;
    int64 context_sample_count;
    int64 row_count;
    int64 row_sample_count;
    int64 shape[LRC_CTC_MODEL_INPUT_RANK];
    int64 window_frame_count;
    int64 context_frame_count;
    int64 original_emission_count;
    int64 extension_emission_count;
    int64 kept_emission_count;
    int64 raw_chunk_emission_count;
    int64 chunk_count;

    LrcCtcModelChunk *chunks;

    int32 shape_len;
    int32 sample_rate;
    int32 inputs_to_logits_ratio;

    double stride_ms;
    bool chunked;
} LrcCtcModelInput;

static void lrc_ctc_model_config_init(LrcCtcModelConfig *config);
static void lrc_ctc_model_input_result_init(LrcCtcModelInputResult *result);
static void lrc_ctc_model_input_destroy(LrcCtcModelInput *input);
static bool lrc_ctc_model_samples_to_emission_frames(
    int64 sample_count, int32 inputs_to_logits_ratio, int64 *frame_count);
static bool lrc_ctc_model_samples_to_emission_frames_floor(
    int64 sample_count, int32 inputs_to_logits_ratio, int64 *frame_count);
static bool lrc_ctc_model_input_prepare(LrcCtcModelInput *input,
                                        LrcCtcAudio *audio,
                                        LrcCtcModelConfig *config,
                                        LrcCtcModelInputResult *result);
static bool lrc_ctc_model_input_validate_model_io(
    LrcCtcModelInput *input, LrcCtcModelIoInfo *info,
    LrcCtcModelInputResult *result);

#endif /* CTC_MODEL_H */
