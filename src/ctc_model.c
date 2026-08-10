#include "cbase.h"
#include "lyricsync.h"
#include "ctc_model.h"

#if !defined(TESTING_ctc_model)
#define TESTING_ctc_model 0
#endif

static void
lrc_ctc_model_config_init(LrcCtcModelConfig *config) {
    if (config == NULL) {
        return;
    }

    config->sample_rate = LRC_CTC_AUDIO_DEFAULT_SAMPLE_RATE;
    config->inputs_to_logits_ratio =
        LRC_CTC_MODEL_DEFAULT_INPUTS_TO_LOGITS_RATIO;
    config->window_seconds = LRC_CTC_MODEL_DEFAULT_WINDOW_SECONDS;
    config->context_seconds = LRC_CTC_MODEL_DEFAULT_CONTEXT_SECONDS;

    return;
}

static void
lrc_ctc_model_input_result_init(LrcCtcModelInputResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_init(&result->header);

    result->sample_index = -1;

    return;
}

static void
lrc_ctc_model_input_result_set(
    LrcCtcModelInputResult *result,
    enum LsError error,
    char *message,
    int64 sample_index
) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_set(&result->header, error, message);

    result->sample_index = sample_index;

    return;
}


static void
lrc_ctc_model_input_destroy(LrcCtcModelInput *input) {
    if (input == NULL) {
        return;
    }

    free2(input->samples, input->sample_count*SIZEOF(*input->samples));
    free2(input->chunks, input->chunk_count*SIZEOF(*input->chunks));

    memset64(input, 0, SIZEOF(*input));

    return;
}

static bool
lrc_ctc_model_seconds_to_samples(
    int32 seconds,
    int32 sample_rate,
    int64 *samples
) {
    if (samples == NULL) {
        return false;
    }
    *samples = 0;
    if (seconds < 0) {
        return false;
    }
    if (sample_rate <= 0) {
        return false;
    }
    if ((int64)seconds > INT64_MAX/(int64)sample_rate) {
        return false;
    }

    *samples = (int64)seconds*(int64)sample_rate;

    return true;
}

static bool
lrc_ctc_model_config_prepare(
    LrcCtcModelConfig *config,
    int64 *window_samples,
    int64 *context_samples,
    LrcCtcModelInputResult *result
) {
    if (config == NULL) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_ARGUMENT,
            "CTC model input configuration is missing",
            -1
        );
        return false;
    }
    if (config->sample_rate <= 0) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_SAMPLE_RATE,
            "CTC model input sample rate is invalid",
            -1
        );
        return false;
    }
    if (config->inputs_to_logits_ratio <= 0) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_RATIO,
            "CTC model input stride ratio is invalid",
            -1
        );
        return false;
    }
    if (!lrc_ctc_model_seconds_to_samples(config->window_seconds,
                                          config->sample_rate,
                                          window_samples)
        || (*window_samples <= 0)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_WINDOW,
            "CTC model input window is invalid",
            -1
        );
        return false;
    }
    if (!lrc_ctc_model_seconds_to_samples(config->context_seconds,
                                          config->sample_rate,
                                          context_samples)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_CONTEXT,
            "CTC model input context is invalid",
            -1
        );
        return false;
    }
    if ((*window_samples % config->inputs_to_logits_ratio) != 0) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_WINDOW,
            "CTC model input window is not aligned to model stride",
            -1
        );
        return false;
    }
    if ((*context_samples % config->inputs_to_logits_ratio) != 0) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_CONTEXT,
            "CTC model input context is not aligned to model stride",
            -1
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_model_audio_valid(
    LrcCtcAudio *audio,
    LrcCtcModelConfig *config,
    LrcCtcModelInputResult *result
) {
    if (audio == NULL) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_ARGUMENT,
            "CTC model input audio is missing",
            -1
        );
        return false;
    }
    if (audio->sample_rate != config->sample_rate) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_AUDIO_SAMPLE_RATE,
            "CTC model input audio sample rate does not match config",
            -1
        );
        return false;
    }
    if ((audio->samples == NULL) || (audio->sample_count <= 0)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_EMPTY_AUDIO,
            "CTC model input audio is empty",
            -1
        );
        return false;
    }

    for (int64 i = 0; i < audio->sample_count; i += 1) {
        if (!isfinite((double)audio->samples[i])) {
            lrc_ctc_model_input_result_set(
                result,
                LS_ERROR_CTC_MODEL_INPUT_NON_FINITE_SAMPLE,
                "CTC model input audio contains a non-finite sample",
                i
            );
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_model_ceil_to_multiple(
    int64 value,
    int64 multiple,
    int64 *result
) {
    int64 remainder;

    if (result == NULL) {
        return false;
    }
    *result = 0;
    if ((value <= 0) || (multiple <= 0)) {
        return false;
    }

    remainder = value%multiple;
    if (remainder == 0) {
        *result = value;
        return true;
    }
    if (value > INT64_MAX - (multiple - remainder)) {
        return false;
    }

    *result = value + multiple - remainder;

    return true;
}

static bool
lrc_ctc_model_samples_to_emission_frames_floor(
    int64 sample_count,
    int32 inputs_to_logits_ratio,
    int64 *frame_count
) {
    if (frame_count == NULL) {
        return false;
    }
    *frame_count = 0;
    if ((sample_count < 0) || (inputs_to_logits_ratio <= 0)) {
        return false;
    }

    *frame_count = sample_count/(int64)inputs_to_logits_ratio;

    return true;
}

static bool
lrc_ctc_model_samples_to_emission_frames(
    int64 sample_count,
    int32 inputs_to_logits_ratio,
    int64 *frame_count
) {
    int64 ratio;

    if (frame_count == NULL) {
        return false;
    }
    *frame_count = 0;
    if ((sample_count < 0) || (inputs_to_logits_ratio <= 0)) {
        return false;
    }
    if (sample_count == 0) {
        return true;
    }

    ratio = (int64)inputs_to_logits_ratio;
    if (sample_count > INT64_MAX - (ratio - 1)) {
        return false;
    }

    *frame_count = (sample_count + ratio - 1)/ratio;

    return true;
}

static bool
lrc_ctc_model_input_shape_short(
    LrcCtcAudio *audio,
    LrcCtcModelInput *input
) {
    input->row_count = 1;
    input->chunk_count = 1;
    input->row_sample_count = audio->sample_count;
    input->sample_count = audio->sample_count;
    input->extension_sample_count = 0;
    input->context_sample_count = 0;
    input->chunked = false;

    return true;
}

static bool
lrc_ctc_model_input_shape_chunked(
    LrcCtcAudio *audio,
    int64 window_samples,
    int64 context_samples,
    LrcCtcModelInput *input
) {
    int64 padded_audio_samples;
    int64 context_total;

    if (!lrc_ctc_model_ceil_to_multiple(audio->sample_count,
                                        window_samples,
                                        &padded_audio_samples)) {
        return false;
    }

    if (context_samples > INT64_MAX/2) {
        return false;
    }
    context_total = 2*context_samples;
    if (window_samples > INT64_MAX - context_total) {
        return false;
    }
    if (padded_audio_samples < audio->sample_count) {
        return false;
    }

    input->row_count = padded_audio_samples/window_samples;
    input->chunk_count = input->row_count;
    input->row_sample_count = window_samples + context_total;
    if (input->row_count <= 0) {
        return false;
    }
    if (input->row_count > INT64_MAX/input->row_sample_count) {
        return false;
    }

    input->sample_count = input->row_count*input->row_sample_count;
    input->extension_sample_count = padded_audio_samples - audio->sample_count;
    input->context_sample_count = context_samples;
    input->chunked = true;

    return true;
}

static bool
lrc_ctc_model_input_allocate(
    LrcCtcModelInput *input,
    LrcCtcModelInputResult *result
) {
    if ((input->sample_count <= 0) || (input->chunk_count <= 0)) {
        return false;
    }
    if (input->sample_count > INT64_MAX/SIZEOF(*input->samples)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_TOO_MANY_SAMPLES,
            "CTC model input tensor is too large",
            -1
        );
        return false;
    }
    if (input->chunk_count > INT64_MAX/SIZEOF(*input->chunks)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_TOO_MANY_SAMPLES,
            "CTC model input chunk metadata is too large",
            -1
        );
        return false;
    }

    input->samples = malloc2(input->sample_count*SIZEOF(*input->samples));
    input->chunks = malloc2(input->chunk_count*SIZEOF(*input->chunks));
    memset64(input->chunks, 0, input->chunk_count*SIZEOF(*input->chunks));

    return true;
}

static void
lrc_ctc_model_input_copy_short(
    LrcCtcModelInput *input,
    LrcCtcAudio *audio
) {
    memcpy64(input->samples,
             audio->samples,
             audio->sample_count*SIZEOF(*audio->samples));

    return;
}

static void
lrc_ctc_model_input_copy_chunked(
    LrcCtcModelInput *input,
    LrcCtcAudio *audio
) {
    int64 window_samples = input->window_sample_count;
    int64 context_samples = input->context_sample_count;

    for (int64 row = 0; row < input->row_count; row += 1) {
        int64 source_start = row*window_samples - context_samples;
        int64 output_start = row*input->row_sample_count;

        for (int64 i = 0; i < input->row_sample_count; i += 1) {
            int64 source_index = source_start + i;
            float sample = 0.0f;

            if ((source_index >= 0) && (source_index < audio->sample_count)) {
                sample = audio->samples[source_index];
            }

            input->samples[output_start + i] = sample;
        }
    }

    return;
}

static bool
lrc_ctc_model_input_prepare_emission_counts(
    LrcCtcModelInput *input
) {
    int64 extension_emissions;

    if ((input == NULL) || (input->inputs_to_logits_ratio <= 0)) {
        return false;
    }
    if (!lrc_ctc_model_samples_to_emission_frames(
            input->original_sample_count,
            input->inputs_to_logits_ratio,
            &input->original_emission_count)) {
        return false;
    }
    if (!lrc_ctc_model_samples_to_emission_frames_floor(
            input->extension_sample_count,
            input->inputs_to_logits_ratio,
            &extension_emissions)) {
        return false;
    }

    input->extension_emission_count = extension_emissions;
    if (input->chunked) {
        if ((input->row_sample_count%(int64)input->inputs_to_logits_ratio)
            != 0) {
            return false;
        }
        input->raw_chunk_emission_count = input->row_sample_count
                                          /input->inputs_to_logits_ratio;
        if (input->row_count > INT64_MAX/input->window_frame_count) {
            return false;
        }
        input->kept_emission_count = input->row_count
                                     *input->window_frame_count;
    } else {
        input->raw_chunk_emission_count = input->original_emission_count;
        input->kept_emission_count = input->original_emission_count;
    }
    if (input->kept_emission_count < input->original_emission_count) {
        return false;
    }
    if ((input->kept_emission_count - input->original_emission_count)
        != input->extension_emission_count) {
        return false;
    }

    return true;
}

static void
lrc_ctc_model_input_prepare_short_chunk(
    LrcCtcModelInput *input
) {
    LrcCtcModelChunk *chunk = &input->chunks[0];

    chunk->source_start_frame = 0;
    chunk->source_frame_count = input->original_sample_count;
    chunk->padded_start_frame = 0;
    chunk->padded_frame_count = input->row_sample_count;
    chunk->left_context_frames = 0;
    chunk->right_context_frames = 0;
    chunk->valid_output_start_frame = 0;
    chunk->valid_output_frame_count = input->original_sample_count;

    chunk->raw_emission_start = 0;
    chunk->raw_emission_count = input->raw_chunk_emission_count;
    chunk->trim_left_emissions = 0;
    chunk->trim_right_emissions = 0;
    chunk->kept_emission_start = 0;
    chunk->kept_emission_count = input->original_emission_count;

    return;
}

static bool
lrc_ctc_model_input_prepare_chunk_metadata(
    LrcCtcModelInput *input
) {
    int64 center_start;
    int64 center_end;
    int64 source_start;
    int64 source_end;
    int64 expected_valid_start;
    int64 total_kept_emissions;

    if ((input == NULL) || (input->chunks == NULL)) {
        return false;
    }
    if (!lrc_ctc_model_input_prepare_emission_counts(input)) {
        return false;
    }
    if (!input->chunked) {
        lrc_ctc_model_input_prepare_short_chunk(input);
        return true;
    }

    expected_valid_start = 0;
    total_kept_emissions = 0;
    for (int64 i = 0; i < input->chunk_count; i += 1) {
        LrcCtcModelChunk *chunk = &input->chunks[i];
        int64 output_end;

        center_start = i*input->window_sample_count;
        center_end = center_start + input->window_sample_count;
        source_start = MAX(0, center_start - input->context_sample_count);
        source_end = MIN(input->original_sample_count,
                         center_end + input->context_sample_count);
        output_end = MIN(center_end, input->original_sample_count);

        chunk->source_start_frame = source_start;
        chunk->source_frame_count = source_end - source_start;
        chunk->padded_start_frame = i*input->row_sample_count;
        chunk->padded_frame_count = input->row_sample_count;
        chunk->left_context_frames = center_start - source_start;
        chunk->right_context_frames = source_end - output_end;
        chunk->valid_output_start_frame = center_start;
        chunk->valid_output_frame_count = output_end - center_start;

        chunk->raw_emission_start = i*input->raw_chunk_emission_count;
        chunk->raw_emission_count = input->raw_chunk_emission_count;
        chunk->trim_left_emissions = input->context_frame_count;
        chunk->trim_right_emissions = input->context_frame_count;
        chunk->kept_emission_start = chunk->raw_emission_start
                                     + chunk->trim_left_emissions;
        chunk->kept_emission_count = input->window_frame_count;

        if ((chunk->source_frame_count < 0)
            || (chunk->left_context_frames < 0)
            || (chunk->right_context_frames < 0)
            || (chunk->valid_output_frame_count < 0)) {
            return false;
        }
        if (chunk->valid_output_start_frame != expected_valid_start) {
            return false;
        }
        if ((chunk->trim_left_emissions + chunk->trim_right_emissions)
            > chunk->raw_emission_count) {
            return false;
        }
        if (chunk->kept_emission_count
            != chunk->raw_emission_count - chunk->trim_left_emissions
                                      - chunk->trim_right_emissions) {
            return false;
        }
        expected_valid_start += chunk->valid_output_frame_count;
        total_kept_emissions += chunk->kept_emission_count;
    }
    if (expected_valid_start != input->original_sample_count) {
        return false;
    }
    if (total_kept_emissions != input->kept_emission_count) {
        return false;
    }

    return true;
}

static bool
lrc_ctc_model_input_prepare(
    LrcCtcModelInput *input,
    LrcCtcAudio *audio,
    LrcCtcModelConfig *config,
    LrcCtcModelInputResult *result
) {
    LrcCtcModelConfig default_config;
    int64 window_samples;
    int64 context_samples;

    if (result) {
        lrc_ctc_model_input_result_init(result);
    }
    if (input == NULL) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_ARGUMENT,
            "CTC model input destination is missing",
            -1
        );
        return false;
    }

    lrc_ctc_model_input_destroy(input);
    if (config == NULL) {
        lrc_ctc_model_config_init(&default_config);
        config = &default_config;
    }

    if (!lrc_ctc_model_config_prepare(config,
                                      &window_samples,
                                      &context_samples,
                                      result)) {
        return false;
    }
    if (!lrc_ctc_model_audio_valid(audio, config, result)) {
        return false;
    }

    input->sample_rate = config->sample_rate;
    input->inputs_to_logits_ratio = config->inputs_to_logits_ratio;
    input->original_sample_count = audio->sample_count;
    input->window_sample_count = window_samples;
    input->window_frame_count =
        window_samples/config->inputs_to_logits_ratio;
    input->context_frame_count =
        context_samples/config->inputs_to_logits_ratio;
    input->stride_ms = (double)config->inputs_to_logits_ratio*1000.0
                       /(double)config->sample_rate;

    if (audio->sample_count < window_samples) {
        lrc_ctc_model_input_shape_short(audio, input);
    } else if (!lrc_ctc_model_input_shape_chunked(audio,
                                                 window_samples,
                                                 context_samples,
                                                 input)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_TOO_MANY_SAMPLES,
            "CTC model input tensor shape is too large",
            -1
        );
        lrc_ctc_model_input_destroy(input);
        return false;
    }

    if (!lrc_ctc_model_input_allocate(input, result)) {
        lrc_ctc_model_input_destroy(input);
        return false;
    }

    if (input->chunked) {
        lrc_ctc_model_input_copy_chunked(input, audio);
    } else {
        lrc_ctc_model_input_copy_short(input, audio);
    }

    if (!lrc_ctc_model_input_prepare_chunk_metadata(input)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_TOO_MANY_SAMPLES,
            "CTC model input chunk metadata could not be prepared",
            -1
        );
        lrc_ctc_model_input_destroy(input);
        return false;
    }

    input->shape_len = LRC_CTC_MODEL_INPUT_RANK;
    input->shape[0] = input->row_count;
    input->shape[1] = input->row_sample_count;

    return true;
}

static bool
lrc_ctc_model_dim_matches(int64 model_dim, int64 input_dim) {
    if (model_dim <= 0) {
        return true;
    }

    return model_dim == input_dim;
}

static bool
lrc_ctc_model_input_validate_model_io(
    LrcCtcModelInput *input,
    LrcCtcModelIoInfo *info,
    LrcCtcModelInputResult *result
) {
    if (result) {
        lrc_ctc_model_input_result_init(result);
    }
    if ((input == NULL) || (info == NULL)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_ARGUMENT,
            "CTC model input shape validation received invalid arguments",
            -1
        );
        return false;
    }
    if ((input->shape_len != LRC_CTC_MODEL_INPUT_RANK)
        || (input->sample_count <= 0)
        || (input->samples == NULL)) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_ARGUMENT,
            "CTC model input tensor is not prepared",
            -1
        );
        return false;
    }
    if ((info->count != 1) || (info->shape_len != LRC_CTC_MODEL_INPUT_RANK)
        || !info->is_float32) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_MODEL_IO,
            "CTC model input must be one rank-2 float tensor",
            -1
        );
        return false;
    }
    if (!lrc_ctc_model_dim_matches(info->shape[0], input->shape[0])
        || !lrc_ctc_model_dim_matches(info->shape[1], input->shape[1])) {
        lrc_ctc_model_input_result_set(
            result,
            LS_ERROR_CTC_MODEL_INPUT_INVALID_MODEL_IO,
            "CTC model input shape does not match prepared tensor",
            -1
        );
        return false;
    }

    return true;
}

#if TESTING_ctc_model
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "audio.c"
#include "ctc_audio.c"

static int32
ctc_model_test_fail(char *name) {
    error2("CTC model test failed: %s\n", name);

    return 1;
}

static bool
ctc_model_double_close(double a, double b, double max_error) {
    double diff;

    diff = fabs(a - b);

    return diff <= max_error;
}

static void
ctc_model_make_audio(
    LrcCtcAudio *audio,
    float *samples,
    int64 sample_count,
    int32 sample_rate
) {
    memset64(audio, 0, SIZEOF(*audio));
    audio->samples = samples;
    audio->sample_count = sample_count;
    audio->sample_rate = sample_rate;
    audio->channel_count = 1;
    if (sample_rate > 0) {
        audio->duration_seconds = (double)sample_count/(double)sample_rate;
    }

    return;
}

static void
ctc_model_test_defaults_and_invalid_inputs(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    float samples[] = {0.0f, 0.1f};

    lrc_ctc_model_config_init(&config);
    lrc_ctc_model_input_result_init(&result);
    ctc_model_make_audio(&audio, samples, LENGTH(samples), 16000);

    ASSERT(config.sample_rate == 16000);
    ASSERT(config.inputs_to_logits_ratio == 320);
    ASSERT(config.window_seconds == 30);
    ASSERT(config.context_seconds == 2);
    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.header.message, "ok"));
    ASSERT(result.sample_index == -1);
    ASSERT(input.samples == NULL);
    ASSERT(input.sample_count == 0);

    if (lrc_ctc_model_input_prepare(NULL, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("null input destination accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_ARGUMENT);

    config.sample_rate = 0;
    if (lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("invalid sample rate accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_SAMPLE_RATE);

    lrc_ctc_model_config_init(&config);
    config.inputs_to_logits_ratio = 0;
    if (lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("invalid stride ratio accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_RATIO);

    lrc_ctc_model_config_init(&config);
    config.sample_rate = 8000;
    if (lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("mismatched audio sample rate accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_AUDIO_SAMPLE_RATE);

    lrc_ctc_model_config_init(&config);
    samples[1] = NAN;
    if (lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("non-finite sample accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_NON_FINITE_SAMPLE);
    ASSERT(result.sample_index == 1);
    samples[1] = 0.1f;

    lrc_ctc_model_input_destroy(&input);

    return;
}

static void
ctc_model_test_prepares_short_input(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    float samples[] = {-0.50f, -0.25f, 0.0f, 0.25f, 0.50f};

    lrc_ctc_model_config_init(&config);
    ctc_model_make_audio(&audio, samples, LENGTH(samples), 16000);

    if (!lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("prepare short input"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(!input.chunked);
    ASSERT(input.sample_rate == 16000);
    ASSERT(input.inputs_to_logits_ratio == 320);
    ASSERT(input.original_sample_count == LENGTH(samples));
    ASSERT(input.sample_count == LENGTH(samples));
    ASSERT(input.row_count == 1);
    ASSERT(input.row_sample_count == LENGTH(samples));
    ASSERT(input.shape_len == 2);
    ASSERT(input.shape[0] == 1);
    ASSERT(input.shape[1] == LENGTH(samples));
    ASSERT(input.extension_sample_count == 0);
    ASSERT(input.context_sample_count == 0);
    ASSERT(input.window_sample_count == 480000);
    ASSERT(input.window_frame_count == 1500);
    ASSERT(input.context_frame_count == 100);
    ASSERT(input.chunk_count == 1);
    ASSERT(input.original_emission_count == 1);
    ASSERT(input.extension_emission_count == 0);
    ASSERT(input.raw_chunk_emission_count == 1);
    ASSERT(input.kept_emission_count == 1);
    ASSERT(input.chunks);
    ASSERT(input.chunks[0].source_start_frame == 0);
    ASSERT(input.chunks[0].source_frame_count == LENGTH(samples));
    ASSERT(input.chunks[0].padded_start_frame == 0);
    ASSERT(input.chunks[0].padded_frame_count == LENGTH(samples));
    ASSERT(input.chunks[0].left_context_frames == 0);
    ASSERT(input.chunks[0].right_context_frames == 0);
    ASSERT(input.chunks[0].valid_output_start_frame == 0);
    ASSERT(input.chunks[0].valid_output_frame_count == LENGTH(samples));
    ASSERT(input.chunks[0].raw_emission_start == 0);
    ASSERT(input.chunks[0].raw_emission_count == 1);
    ASSERT(input.chunks[0].trim_left_emissions == 0);
    ASSERT(input.chunks[0].trim_right_emissions == 0);
    ASSERT(input.chunks[0].kept_emission_start == 0);
    ASSERT(input.chunks[0].kept_emission_count == 1);
    ASSERT(ctc_model_double_close(input.stride_ms, 20.0, 0.00001));

    for (int64 i = 0; i < LENGTH(samples); i += 1) {
        ASSERT(input.samples[i] == samples[i]);
    }

    lrc_ctc_model_input_destroy(&input);

    return;
}

static void
ctc_model_test_prepares_chunked_input(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    float samples[20];

    for (int32 i = 0; i < LENGTH(samples); i += 1) {
        samples[i] = (float)(i + 1);
    }

    lrc_ctc_model_config_init(&config);
    config.sample_rate = 8;
    config.inputs_to_logits_ratio = 2;
    config.window_seconds = 2;
    config.context_seconds = 1;
    ctc_model_make_audio(&audio, samples, LENGTH(samples), 8);

    if (!lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("prepare chunked input"));
    }

    ASSERT(input.chunked);
    ASSERT(input.original_sample_count == 20);
    ASSERT(input.window_sample_count == 16);
    ASSERT(input.context_sample_count == 8);
    ASSERT(input.extension_sample_count == 12);
    ASSERT(input.row_count == 2);
    ASSERT(input.row_sample_count == 32);
    ASSERT(input.sample_count == 64);
    ASSERT(input.shape[0] == 2);
    ASSERT(input.shape[1] == 32);
    ASSERT(input.window_frame_count == 8);
    ASSERT(input.context_frame_count == 4);
    ASSERT(input.chunk_count == 2);
    ASSERT(input.original_emission_count == 10);
    ASSERT(input.extension_emission_count == 6);
    ASSERT(input.raw_chunk_emission_count == 16);
    ASSERT(input.kept_emission_count == 16);
    ASSERT(input.chunks);

    ASSERT(input.chunks[0].source_start_frame == 0);
    ASSERT(input.chunks[0].source_frame_count == 20);
    ASSERT(input.chunks[0].padded_start_frame == 0);
    ASSERT(input.chunks[0].padded_frame_count == 32);
    ASSERT(input.chunks[0].left_context_frames == 0);
    ASSERT(input.chunks[0].right_context_frames == 4);
    ASSERT(input.chunks[0].valid_output_start_frame == 0);
    ASSERT(input.chunks[0].valid_output_frame_count == 16);
    ASSERT(input.chunks[0].raw_emission_start == 0);
    ASSERT(input.chunks[0].raw_emission_count == 16);
    ASSERT(input.chunks[0].trim_left_emissions == 4);
    ASSERT(input.chunks[0].trim_right_emissions == 4);
    ASSERT(input.chunks[0].kept_emission_start == 4);
    ASSERT(input.chunks[0].kept_emission_count == 8);

    ASSERT(input.chunks[1].source_start_frame == 8);
    ASSERT(input.chunks[1].source_frame_count == 12);
    ASSERT(input.chunks[1].padded_start_frame == 32);
    ASSERT(input.chunks[1].padded_frame_count == 32);
    ASSERT(input.chunks[1].left_context_frames == 8);
    ASSERT(input.chunks[1].right_context_frames == 0);
    ASSERT(input.chunks[1].valid_output_start_frame == 16);
    ASSERT(input.chunks[1].valid_output_frame_count == 4);
    ASSERT(input.chunks[1].raw_emission_start == 16);
    ASSERT(input.chunks[1].raw_emission_count == 16);
    ASSERT(input.chunks[1].trim_left_emissions == 4);
    ASSERT(input.chunks[1].trim_right_emissions == 4);
    ASSERT(input.chunks[1].kept_emission_start == 20);
    ASSERT(input.chunks[1].kept_emission_count == 8);
    ASSERT(ctc_model_double_close(input.stride_ms, 250.0, 0.00001));

    for (int32 i = 0; i < 8; i += 1) {
        ASSERT(input.samples[i] == 0.0f);
    }
    for (int32 i = 0; i < 20; i += 1) {
        ASSERT(input.samples[8 + i] == samples[i]);
    }
    for (int32 i = 28; i < 32; i += 1) {
        ASSERT(input.samples[i] == 0.0f);
    }

    ASSERT(input.samples[32] == samples[8]);
    ASSERT(input.samples[39] == samples[15]);
    ASSERT(input.samples[40] == samples[16]);
    ASSERT(input.samples[43] == samples[19]);
    for (int32 i = 44; i < 64; i += 1) {
        ASSERT(input.samples[i] == 0.0f);
    }

    lrc_ctc_model_input_destroy(&input);

    return;
}

static void
ctc_model_test_emission_frame_conversion(void) {
    int64 frames;

    if (!lrc_ctc_model_samples_to_emission_frames(0, 2, &frames)) {
        fatal(ctc_model_test_fail("ceil zero emissions"));
    }
    ASSERT(frames == 0);

    if (!lrc_ctc_model_samples_to_emission_frames(20, 2, &frames)) {
        fatal(ctc_model_test_fail("exact emission frames"));
    }
    ASSERT(frames == 10);

    if (!lrc_ctc_model_samples_to_emission_frames(21, 2, &frames)) {
        fatal(ctc_model_test_fail("partial emission frames"));
    }
    ASSERT(frames == 11);

    if (!lrc_ctc_model_samples_to_emission_frames_floor(21, 2, &frames)) {
        fatal(ctc_model_test_fail("floor emission frames"));
    }
    ASSERT(frames == 10);

    if (lrc_ctc_model_samples_to_emission_frames(-1, 2, &frames)) {
        fatal(ctc_model_test_fail("negative emission frames accepted"));
    }
    if (lrc_ctc_model_samples_to_emission_frames(1, 0, &frames)) {
        fatal(ctc_model_test_fail("zero ratio accepted"));
    }

    return;
}

static void
ctc_model_test_chunk_metadata_three_chunks(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    int64 valid_total;
    float samples[40];

    for (int32 i = 0; i < LENGTH(samples); i += 1) {
        samples[i] = (float)(i + 1);
    }

    lrc_ctc_model_config_init(&config);
    config.sample_rate = 8;
    config.inputs_to_logits_ratio = 2;
    config.window_seconds = 2;
    config.context_seconds = 1;
    ctc_model_make_audio(&audio, samples, LENGTH(samples), 8);

    if (!lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("prepare three-chunk metadata"));
    }

    ASSERT(input.chunked);
    ASSERT(input.chunk_count == 3);
    ASSERT(input.row_count == 3);
    ASSERT(input.extension_sample_count == 8);
    ASSERT(input.original_emission_count == 20);
    ASSERT(input.extension_emission_count == 4);
    ASSERT(input.raw_chunk_emission_count == 16);
    ASSERT(input.kept_emission_count == 24);

    ASSERT(input.chunks[0].source_start_frame == 0);
    ASSERT(input.chunks[0].source_frame_count == 24);
    ASSERT(input.chunks[0].left_context_frames == 0);
    ASSERT(input.chunks[0].right_context_frames == 8);
    ASSERT(input.chunks[0].valid_output_start_frame == 0);
    ASSERT(input.chunks[0].valid_output_frame_count == 16);
    ASSERT(input.chunks[0].raw_emission_start == 0);
    ASSERT(input.chunks[0].kept_emission_start == 4);
    ASSERT(input.chunks[0].kept_emission_count == 8);

    ASSERT(input.chunks[1].source_start_frame == 8);
    ASSERT(input.chunks[1].source_frame_count == 32);
    ASSERT(input.chunks[1].left_context_frames == 8);
    ASSERT(input.chunks[1].right_context_frames == 8);
    ASSERT(input.chunks[1].valid_output_start_frame == 16);
    ASSERT(input.chunks[1].valid_output_frame_count == 16);
    ASSERT(input.chunks[1].raw_emission_start == 16);
    ASSERT(input.chunks[1].kept_emission_start == 20);
    ASSERT(input.chunks[1].kept_emission_count == 8);

    ASSERT(input.chunks[2].source_start_frame == 24);
    ASSERT(input.chunks[2].source_frame_count == 16);
    ASSERT(input.chunks[2].left_context_frames == 8);
    ASSERT(input.chunks[2].right_context_frames == 0);
    ASSERT(input.chunks[2].valid_output_start_frame == 32);
    ASSERT(input.chunks[2].valid_output_frame_count == 8);
    ASSERT(input.chunks[2].raw_emission_start == 32);
    ASSERT(input.chunks[2].kept_emission_start == 36);
    ASSERT(input.chunks[2].kept_emission_count == 8);

    valid_total = 0;
    for (int64 i = 0; i < input.chunk_count; i += 1) {
        ASSERT(input.chunks[i].padded_start_frame == i*32);
        ASSERT(input.chunks[i].padded_frame_count == 32);
        ASSERT(input.chunks[i].raw_emission_count == 16);
        ASSERT(input.chunks[i].trim_left_emissions == 4);
        ASSERT(input.chunks[i].trim_right_emissions == 4);
        valid_total += input.chunks[i].valid_output_frame_count;
    }
    ASSERT(valid_total == input.original_sample_count);

    lrc_ctc_model_input_destroy(&input);

    return;
}

static void
ctc_model_test_chunk_metadata_partial_stride(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    float samples[21];

    for (int32 i = 0; i < LENGTH(samples); i += 1) {
        samples[i] = (float)(i + 1);
    }

    lrc_ctc_model_config_init(&config);
    config.sample_rate = 8;
    config.inputs_to_logits_ratio = 2;
    config.window_seconds = 2;
    config.context_seconds = 1;
    ctc_model_make_audio(&audio, samples, LENGTH(samples), 8);

    if (!lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("prepare partial-stride metadata"));
    }

    ASSERT(input.chunked);
    ASSERT(input.chunk_count == 2);
    ASSERT(input.extension_sample_count == 11);
    ASSERT(input.original_emission_count == 11);
    ASSERT(input.extension_emission_count == 5);
    ASSERT(input.kept_emission_count == 16);
    ASSERT(input.chunks[1].valid_output_start_frame == 16);
    ASSERT(input.chunks[1].valid_output_frame_count == 5);
    ASSERT(input.chunks[1].right_context_frames == 0);
    ASSERT(input.chunks[1].kept_emission_count == 8);

    lrc_ctc_model_input_destroy(&input);

    return;
}

static void
ctc_model_test_rejects_unaligned_window_or_context(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    float samples[] = {0.0f, 0.1f, 0.2f};

    ctc_model_make_audio(&audio, samples, LENGTH(samples), 10);

    lrc_ctc_model_config_init(&config);
    config.sample_rate = 10;
    config.inputs_to_logits_ratio = 3;
    config.window_seconds = 1;
    config.context_seconds = 0;
    if (lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("unaligned window accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_WINDOW);

    config.inputs_to_logits_ratio = 4;
    config.window_seconds = 2;
    config.context_seconds = 1;
    if (lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("unaligned context accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_CONTEXT);

    lrc_ctc_model_input_destroy(&input);

    return;
}

static void
ctc_model_test_validates_model_io(void) {
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    LrcCtcAudio audio;
    LrcCtcModelIoInfo info = {0};
    float samples[] = {0.0f, 0.1f, 0.2f};

    lrc_ctc_model_config_init(&config);
    ctc_model_make_audio(&audio, samples, LENGTH(samples), 16000);
    if (!lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        fatal(ctc_model_test_fail("prepare IO validation input"));
    }

    info.count = 1;
    info.is_float32 = true;
    info.shape_len = 2;
    info.shape[0] = -1;
    info.shape[1] = -1;
    if (!lrc_ctc_model_input_validate_model_io(&input, &info, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("dynamic IO rejected"));
    }

    info.shape[0] = 1;
    info.shape[1] = LENGTH(samples);
    if (!lrc_ctc_model_input_validate_model_io(&input, &info, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("fixed matching IO rejected"));
    }

    info.is_float32 = false;
    if (lrc_ctc_model_input_validate_model_io(&input, &info, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("non-float IO accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_MODEL_IO);

    info.is_float32 = true;
    info.shape_len = 3;
    if (lrc_ctc_model_input_validate_model_io(&input, &info, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("rank-3 IO accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_MODEL_IO);

    info.shape_len = 2;
    info.shape[0] = 1;
    info.shape[1] = LENGTH(samples) + 1;
    if (lrc_ctc_model_input_validate_model_io(&input, &info, &result)) {
        lrc_ctc_model_input_destroy(&input);
        fatal(ctc_model_test_fail("mismatched IO width accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_MODEL_INPUT_INVALID_MODEL_IO);

    lrc_ctc_model_input_destroy(&input);

    return;
}

static char *
ctc_model_maxwell_vocals_path(void) {
    char *path;

    path = getenv("LRC_TEST_MAXWELL_VOCALS");
    if (!path_missing(path)) {
        return path;
    }
    if (util_file_exists("next-phase/maxwell_vocals.opus")) {
        return "next-phase/maxwell_vocals.opus";
    }
    if (util_file_exists("/mnt/data/maxwell_vocals.opus")) {
        return "/mnt/data/maxwell_vocals.opus";
    }

    return NULL;
}

static void
ctc_model_test_prepares_maxwell_shaped_input(void) {
    LrcCtcAudioConfig audio_config;
    LrcCtcAudioResult audio_result;
    LrcCtcAudio audio = {0};
    LrcCtcModelConfig config;
    LrcCtcModelInput input = {0};
    LrcCtcModelInputResult result;
    char *path;

    if (!test_command_exists("ffmpeg")) {
        return;
    }

    path = ctc_model_maxwell_vocals_path();
    if (path == NULL) {
        return;
    }

    lrc_ctc_audio_config_init(&audio_config);
    audio_config.sample_rate = 16000;
    if (!lrc_ctc_audio_decode_file(&audio, path, &audio_config,
                                    &audio_result)) {
        fatal(ctc_model_test_fail("decode Maxwell vocals"));
    }

    lrc_ctc_model_config_init(&config);
    if (!lrc_ctc_model_input_prepare(&input, &audio, &config, &result)) {
        lrc_ctc_audio_destroy(&audio);
        fatal(ctc_model_test_fail("prepare Maxwell-shaped input"));
    }

    ASSERT(!input.chunked);
    ASSERT(input.shape_len == 2);
    ASSERT(input.shape[0] == 1);
    ASSERT(input.shape[1] == audio.sample_count);
    ASSERT(input.sample_count == audio.sample_count);
    ASSERT((input.sample_count >= 340000) && (input.sample_count <= 343000));
    ASSERT(ctc_model_double_close(input.stride_ms, 20.0, 0.00001));

    lrc_ctc_model_input_destroy(&input);
    lrc_ctc_audio_destroy(&audio);

    return;
}

int32
main(void) {
    ctc_model_test_defaults_and_invalid_inputs();
    ctc_model_test_prepares_short_input();
    ctc_model_test_prepares_chunked_input();
    ctc_model_test_emission_frame_conversion();
    ctc_model_test_chunk_metadata_three_chunks();
    ctc_model_test_chunk_metadata_partial_stride();
    ctc_model_test_rejects_unaligned_window_or_context();
    ctc_model_test_validates_model_io();
    ctc_model_test_prepares_maxwell_shaped_input();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_ctc_model */
