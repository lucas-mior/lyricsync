#include "cbase.h"
#include "lyricsync.h"
#include "ctc_inference.h"
#include "progress.c"

#if !defined(TESTING_ctc_inference)
#define TESTING_ctc_inference 0
#endif

#if LRC_CTC_INFERENCE_ENABLE_ORT && TESTING_ctc_inference
#include <ort.c>
#endif

static void
lrc_ctc_inference_result_init(LrcCtcInferenceResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_init(&result->header);

    result->output_index = -1;

    return;
}

static void
lrc_ctc_inference_result_set(
    LrcCtcInferenceResult *result,
    enum LsError error,
    char *message,
    int64 output_index
) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_set(&result->header, error, message);

    result->output_index = (int32)CLAMP(output_index, INT32_MIN, INT32_MAX);

    return;
}


static void
lrc_ctc_emissions_destroy(LrcCtcEmissions *emissions) {
    if (emissions == NULL) {
        return;
    }

    free2(emissions->values,
          emissions->value_count*SIZEOF(*emissions->values));

    memset64(emissions, 0, SIZEOF(*emissions));

    return;
}

static bool
lrc_ctc_emissions_shape_valid(
    int64 *shape,
    int32 shape_len,
    int64 *row_count,
    int64 *row_emission_count,
    int64 *total_emission_count,
    int64 *vocabulary_size,
    int64 *value_count,
    LrcCtcInferenceResult *result
) {
    int64 rows;
    int64 row_emissions;
    int64 total_emissions;
    int64 vocab;
    int64 count;

    if ((shape == NULL) || (shape_len < 2)
        || (shape_len > LRC_CTC_EMISSIONS_MAX_RANK)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC emissions must have rank 2 or rank 3",
            -1
        );
        return false;
    }

    if (shape_len == 2) {
        rows = 1;
        row_emissions = shape[0];
        vocab = shape[1];
    } else {
        rows = shape[0];
        row_emissions = shape[1];
        vocab = shape[2];
    }
    if ((rows <= 0) || (row_emissions <= 0) || (vocab <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC emissions have invalid dimensions",
            -1
        );
        return false;
    }
    if (rows > INT64_MAX/row_emissions) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC emissions frame count is too large",
            -1
        );
        return false;
    }
    total_emissions = rows*row_emissions;
    if (total_emissions > INT64_MAX/vocab) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC emissions tensor is too large",
            -1
        );
        return false;
    }

    count = total_emissions*vocab;
    *row_count = rows;
    *row_emission_count = row_emissions;
    *total_emission_count = total_emissions;
    *vocabulary_size = vocab;
    *value_count = count;

    return true;
}

static bool
lrc_ctc_emissions_values_valid(
    float *values,
    int64 value_count,
    LrcCtcInferenceResult *result
) {
    if ((values == NULL) || (value_count <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC emissions values are missing",
            -1
        );
        return false;
    }

    for (int64 i = 0; i < value_count; i += 1) {
        if (!isfinite((double)values[i])) {
            lrc_ctc_inference_result_set(
                result,
                LS_ERROR_CTC_INFERENCE_NON_FINITE_OUTPUT,
                "CTC emissions contain a non-finite value",
                i
            );
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_emissions_copy_shape(
    LrcCtcEmissions *emissions,
    float *values,
    int64 value_count,
    int64 *shape,
    int32 shape_len,
    LrcCtcInferenceResult *result
) {
    int64 row_count;
    int64 row_emission_count;
    int64 total_emission_count;
    int64 vocabulary_size;
    int64 expected_count;

    if (result) {
        lrc_ctc_inference_result_init(result);
    }
    if (emissions == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC emissions destination is missing",
            -1
        );
        return false;
    }

    lrc_ctc_emissions_destroy(emissions);
    if (!lrc_ctc_emissions_shape_valid(shape,
                                       shape_len,
                                       &row_count,
                                       &row_emission_count,
                                       &total_emission_count,
                                       &vocabulary_size,
                                       &expected_count,
                                       result)) {
        return false;
    }
    if (value_count != expected_count) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC emissions value count does not match shape",
            -1
        );
        return false;
    }
    if (!lrc_ctc_emissions_values_valid(values, value_count, result)) {
        return false;
    }
    if (value_count > INT64_MAX/SIZEOF(*emissions->values)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC emissions copy is too large",
            -1
        );
        return false;
    }

    emissions->values = malloc2(value_count*SIZEOF(*emissions->values));
    memcpy64(emissions->values, values, value_count*SIZEOF(*values));
    emissions->value_count = value_count;
    emissions->row_count = row_count;
    emissions->row_frame_count = row_emission_count;
    emissions->frame_count = total_emission_count;
    emissions->vocabulary_size = vocabulary_size;
    emissions->shape_len = shape_len;
    for (int32 i = 0; i < shape_len; i += 1) {
        emissions->shape[i] = shape[i];
    }

    return true;
}

static bool
lrc_ctc_emissions_value_count(
    int64 emission_count,
    int64 vocabulary_size,
    int64 *value_count,
    LrcCtcInferenceResult *result
) {
    if (value_count == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC trimmed emission value count destination is missing",
            -1
        );
        return false;
    }
    *value_count = 0;
    if ((emission_count <= 0) || (vocabulary_size <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC trimmed emissions have invalid dimensions",
            -1
        );
        return false;
    }
    if (emission_count > INT64_MAX/vocabulary_size) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC trimmed emissions frame count is too large",
            -1
        );
        return false;
    }

    *value_count = emission_count*vocabulary_size;
    if (*value_count > INT64_MAX/SIZEOF(float)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC trimmed emissions copy is too large",
            -1
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_emissions_input_chunks_ready(
    LrcCtcModelInput *input,
    int64 raw_chunk_count,
    LrcCtcInferenceResult *result
) {
    if ((input == NULL) || (input->chunks == NULL)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_INPUT,
            "CTC rank-3 emission trimming requires chunk metadata",
            -1
        );
        return false;
    }
    if ((input->chunk_count <= 0) || (input->chunk_count != raw_chunk_count)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output chunk count does not match input metadata",
            -1
        );
        return false;
    }
    if (input->chunked) {
        if ((input->original_emission_count <= 0)
            || (input->window_frame_count <= 0)
            || (input->context_frame_count < 0)
            || (input->kept_emission_count
                < input->original_emission_count)) {
            lrc_ctc_inference_result_set(
                result,
                LS_ERROR_CTC_INFERENCE_INVALID_INPUT,
                "CTC input chunk metadata is invalid",
                -1
            );
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_emissions_output_frame_count(
    LrcCtcModelInput *input,
    int64 raw_chunk_emission_count,
    int64 *output_frame_count,
    LrcCtcInferenceResult *result
) {
    if (output_frame_count == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC trimmed output frame count destination is missing",
            -1
        );
        return false;
    }
    *output_frame_count = 0;
    if ((input == NULL) || (raw_chunk_emission_count <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC trimmed output frame count arguments are invalid",
            -1
        );
        return false;
    }

    if (input->chunked) {
        *output_frame_count = input->original_emission_count;
    } else {
        *output_frame_count = raw_chunk_emission_count;
    }
    if (*output_frame_count <= 0) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output trimming produced no frames",
            -1
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_emissions_chunk_trim_range(
    LrcCtcModelInput *input,
    LrcCtcModelChunk *chunk,
    int64 chunk_index,
    int64 raw_chunk_emission_count,
    int64 *kept_offset,
    int64 *kept_count,
    LrcCtcInferenceResult *result
) {
    int64 offset;
    int64 count;

    if ((input == NULL) || (chunk == NULL) || (kept_offset == NULL)
        || (kept_count == NULL)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_INPUT,
            "CTC input chunk metadata entry is missing",
            chunk_index
        );
        return false;
    }
    *kept_offset = 0;
    *kept_count = 0;
    if (raw_chunk_emission_count <= 0) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output chunk has invalid frame count",
            chunk_index
        );
        return false;
    }

    if (input->chunked) {
        offset = chunk->trim_left_emissions;
        count = chunk->kept_emission_count;
    } else {
        offset = 0;
        count = raw_chunk_emission_count;
    }

    if ((offset < 0) || (count <= 0) || (offset > raw_chunk_emission_count)
        || (count > raw_chunk_emission_count - offset)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output is too short for input chunk trimming",
            chunk_index
        );
        return false;
    }

    *kept_offset = offset;
    *kept_count = count;

    return true;
}

static bool
lrc_ctc_emissions_copy_rank3_trimmed(
    LrcCtcEmissions *emissions,
    LrcCtcModelInput *input,
    float *values,
    int64 value_count,
    int64 *shape,
    bool print_progress,
    LrcCtcInferenceResult *result
) {
    int64 raw_chunk_count;
    int64 raw_chunk_emission_count;
    int64 raw_emission_count;
    int64 vocabulary_size;
    int64 raw_value_count;
    int64 output_frame_count;
    int64 kept_value_count;
    int64 kept_frame;
    LrcProgress progress;

    if (!lrc_ctc_emissions_shape_valid(shape,
                                       3,
                                       &raw_chunk_count,
                                       &raw_chunk_emission_count,
                                       &raw_emission_count,
                                       &vocabulary_size,
                                       &raw_value_count,
                                       result)) {
        return false;
    }
    (void)raw_emission_count;
    if (value_count != raw_value_count) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output value count does not match shape",
            -1
        );
        return false;
    }
    if (values == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output values are missing",
            -1
        );
        return false;
    }
    if (!lrc_ctc_emissions_input_chunks_ready(input,
                                             raw_chunk_count,
                                             result)) {
        return false;
    }
    if (!lrc_ctc_emissions_output_frame_count(input,
                                              raw_chunk_emission_count,
                                              &output_frame_count,
                                              result)) {
        return false;
    }
    if (!lrc_ctc_emissions_value_count(output_frame_count,
                                       vocabulary_size,
                                       &kept_value_count,
                                       result)) {
        return false;
    }

    emissions->values = malloc2(
        kept_value_count*SIZEOF(*emissions->values)
    );
    lrc_progress_init(&progress,
                      print_progress,
                      "trim CTC emissions",
                      input->chunk_count);
    lrc_progress_begin(&progress);
    kept_frame = 0;
    for (int64 i = 0; i < input->chunk_count; i += 1) {
        LrcCtcModelChunk *chunk = &input->chunks[i];
        int64 kept_offset;
        int64 chunk_kept_count;
        int64 raw_value_offset;

        if (!lrc_ctc_emissions_chunk_trim_range(input,
                                                chunk,
                                                i,
                                                raw_chunk_emission_count,
                                                &kept_offset,
                                                &chunk_kept_count,
                                                result)) {
            lrc_progress_cancel(&progress);
            free2(emissions->values,
                  kept_value_count*SIZEOF(*emissions->values));
            emissions->values = NULL;
            return false;
        }
        raw_value_offset = i*raw_chunk_emission_count*vocabulary_size;
        for (int64 j = 0; j < chunk_kept_count; j += 1) {
            int64 source_offset;
            int64 output_offset;

            if (kept_frame >= output_frame_count) {
                break;
            }
            source_offset = raw_value_offset
                            + (kept_offset + j)*vocabulary_size;
            output_offset = kept_frame*vocabulary_size;
            memcpy64(emissions->values + output_offset,
                     values + source_offset,
                     vocabulary_size*SIZEOF(*values));
            kept_frame += 1;
        }
        lrc_progress_update(&progress, i + 1);
    }
    if (kept_frame != output_frame_count) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC rank-3 output trimming produced too few frames",
            kept_frame
        );
        lrc_progress_cancel(&progress);
        free2(emissions->values,
              kept_value_count*SIZEOF(*emissions->values));
        emissions->values = NULL;
        return false;
    }

    emissions->value_count = kept_value_count;
    emissions->row_count = 1;
    emissions->row_frame_count = output_frame_count;
    emissions->frame_count = output_frame_count;
    emissions->vocabulary_size = vocabulary_size;
    emissions->shape_len = 2;
    emissions->shape[0] = output_frame_count;
    emissions->shape[1] = vocabulary_size;
    if (!lrc_ctc_emissions_values_valid(emissions->values,
                                        emissions->value_count,
                                        result)) {
        lrc_progress_cancel(&progress);
        lrc_ctc_emissions_destroy(emissions);
        return false;
    }
    lrc_progress_finish(&progress);

    return true;
}

static bool
lrc_ctc_emissions_copy_model_output(
    LrcCtcEmissions *emissions,
    LrcCtcModelInput *input,
    float *values,
    int64 value_count,
    int64 *shape,
    int32 shape_len,
    bool print_progress,
    LrcCtcInferenceResult *result
) {
    if (result) {
        lrc_ctc_inference_result_init(result);
    }
    if (emissions == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC emissions destination is missing",
            -1
        );
        return false;
    }

    lrc_ctc_emissions_destroy(emissions);
    if (shape_len == 2) {
        return lrc_ctc_emissions_copy_shape(emissions,
                                            values,
                                            value_count,
                                            shape,
                                            shape_len,
                                            result);
    }
    if (shape_len == 3) {
        return lrc_ctc_emissions_copy_rank3_trimmed(emissions,
                                                    input,
                                                    values,
                                                    value_count,
                                                    shape,
                                                    print_progress,
                                                    result);
    }

    lrc_ctc_inference_result_set(
        result,
        LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
        "CTC emissions must have rank 2 or rank 3",
        -1
    );
    return false;
}

static bool
lrc_ctc_emissions_ready(
    LrcCtcEmissions *emissions,
    LrcCtcInferenceResult *result
) {
    if (emissions == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC emissions are missing",
            -1
        );
        return false;
    }
    if ((emissions->values == NULL) || (emissions->value_count <= 0)
        || (emissions->frame_count <= 0)
        || (emissions->vocabulary_size <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC emissions are not prepared",
            -1
        );
        return false;
    }
    if (emissions->frame_count
        > INT64_MAX/emissions->vocabulary_size) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC emissions dimensions are too large",
            -1
        );
        return false;
    }
    if (emissions->value_count
        != emissions->frame_count*emissions->vocabulary_size) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC emissions value count does not match dimensions",
            -1
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_emissions_log_softmax_row(
    float *row,
    int64 vocabulary_size,
    int64 row_offset,
    LrcCtcInferenceResult *result
) {
    double sum;
    double log_denom;
    float max_value;

    if ((row == NULL) || (vocabulary_size <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC log-softmax row arguments are invalid",
            row_offset
        );
        return false;
    }

    max_value = row[0];
    for (int64 i = 1; i < vocabulary_size; i += 1) {
        if (row[i] > max_value) {
            max_value = row[i];
        }
    }

    sum = 0.0;
    for (int64 i = 0; i < vocabulary_size; i += 1) {
        sum += exp((double)row[i] - (double)max_value);
    }
    if (!isfinite(sum) || (sum <= 0.0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_PROBABILITY,
            "CTC log-softmax row has invalid normalizer",
            row_offset
        );
        return false;
    }

    log_denom = (double)max_value + log(sum);
    for (int64 i = 0; i < vocabulary_size; i += 1) {
        row[i] = (float)((double)row[i] - log_denom);
    }

    return true;
}

static bool
lrc_ctc_emissions_log_probabilities_from_probabilities_row(
    float *row,
    int64 vocabulary_size,
    int64 row_offset,
    LrcCtcInferenceResult *result
) {
    if ((row == NULL) || (vocabulary_size <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC probability row arguments are invalid",
            row_offset
        );
        return false;
    }

    for (int64 i = 0; i < vocabulary_size; i += 1) {
        if (!isfinite((double)row[i]) || (row[i] <= 0.0f)) {
            lrc_ctc_inference_result_set(
                result,
                LS_ERROR_CTC_INFERENCE_INVALID_PROBABILITY,
                "CTC probabilities must be finite and positive",
                row_offset + i
            );
            return false;
        }
    }

    for (int64 i = 0; i < vocabulary_size; i += 1) {
        row[i] = logf(row[i]);
    }

    return true;
}

static bool
lrc_ctc_emissions_convert_to_log_probabilities(
    LrcCtcEmissions *emissions,
    enum LrcCtcEmissionValuesKind values_kind,
    LrcCtcInferenceResult *result
) {
    if (result) {
        lrc_ctc_inference_result_init(result);
    }
    if (!lrc_ctc_emissions_ready(emissions, result)) {
        return false;
    }

    switch (values_kind) {
    case LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES:
        return true;
    case LRC_CTC_EMISSION_VALUES_LOGITS:
        for (int64 frame = 0; frame < emissions->frame_count; frame += 1) {
            int64 offset = frame*emissions->vocabulary_size;

            if (!lrc_ctc_emissions_log_softmax_row(
                    emissions->values + offset,
                    emissions->vocabulary_size,
                    offset,
                    result)) {
                return false;
            }
        }
        return true;
    case LRC_CTC_EMISSION_VALUES_PROBABILITIES:
        for (int64 frame = 0; frame < emissions->frame_count; frame += 1) {
            int64 offset = frame*emissions->vocabulary_size;

            if (!lrc_ctc_emissions_log_probabilities_from_probabilities_row(
                    emissions->values + offset,
                    emissions->vocabulary_size,
                    offset,
                    result)) {
                return false;
            }
        }
        return true;
    case LRC_CTC_EMISSION_VALUES_COUNT:
    default:
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC emission value kind is invalid",
            -1
        );
        return false;
    }
}

static bool
lrc_ctc_emissions_build_trimmed_from_model_output(
    LrcCtcEmissions *emissions,
    LrcCtcModelInput *input,
    float *values,
    int64 value_count,
    int64 *shape,
    int32 shape_len,
    enum LrcCtcEmissionValuesKind values_kind,
    bool print_progress,
    LrcCtcInferenceResult *result
) {
    if (!lrc_ctc_emissions_copy_model_output(emissions,
                                             input,
                                             values,
                                             value_count,
                                             shape,
                                             shape_len,
                                             print_progress,
                                             result)) {
        return false;
    }
    if (!lrc_ctc_emissions_convert_to_log_probabilities(emissions,
                                                        values_kind,
                                                        result)) {
        lrc_ctc_emissions_destroy(emissions);
        return false;
    }

    return true;
}

static bool
lrc_ctc_inference_input_ready(
    LrcCtcModelInput *input,
    LrcCtcInferenceResult *result
) {
    if (input == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC inference input is missing",
            -1
        );
        return false;
    }
    if ((input->samples == NULL) || (input->sample_count <= 0)
        || (input->shape_len != LRC_CTC_MODEL_INPUT_RANK)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_INPUT,
            "CTC inference input tensor is not prepared",
            -1
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_inference_run(
    LrcCtcInferenceBackend *backend,
    LrcCtcModelInput *input,
    LrcCtcEmissions *emissions,
    LrcCtcInferenceResult *result
) {
    if (result) {
        lrc_ctc_inference_result_init(result);
    }
    if ((backend == NULL) || (backend->run == NULL)
        || (emissions == NULL)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC inference backend arguments are invalid",
            -1
        );
        return false;
    }
    if (!lrc_ctc_inference_input_ready(input, result)) {
        return false;
    }

    return backend->run(backend->backend,
                        input,
                        emissions,
                        backend->values_kind,
                        backend->print_progress,
                        result);
}


#if TESTING
static bool
lrc_ctc_fake_inference_set_shape(
    LrcCtcFakeInference *fake,
    float *values,
    int64 value_count,
    int64 *shape,
    int32 shape_len
) {
    int64 row_count;
    int64 row_emission_count;
    int64 total_emission_count;
    int64 vocabulary_size;
    int64 expected_count;

    if (fake == NULL) {
        return false;
    }
    if (!lrc_ctc_emissions_shape_valid(shape,
                                       shape_len,
                                       &row_count,
                                       &row_emission_count,
                                       &total_emission_count,
                                       &vocabulary_size,
                                       &expected_count,
                                       NULL)) {
        return false;
    }
    if ((values == NULL) || (value_count != expected_count)) {
        return false;
    }

    fake->values = values;
    fake->value_count = value_count;
    fake->shape_len = shape_len;
    for (int32 i = 0; i < shape_len; i += 1) {
        fake->shape[i] = shape[i];
    }

    (void)row_count;
    (void)row_emission_count;
    (void)total_emission_count;
    (void)vocabulary_size;

    return true;
}

static bool
lrc_ctc_fake_inference_set(
    LrcCtcFakeInference *fake,
    float *values,
    int64 frame_count,
    int64 vocabulary_size
) {
    int64 shape[2];

    shape[0] = frame_count;
    shape[1] = vocabulary_size;

    if ((frame_count <= 0) || (vocabulary_size <= 0)
        || (frame_count > INT64_MAX/vocabulary_size)) {
        return false;
    }

    return lrc_ctc_fake_inference_set_shape(fake,
                                            values,
                                            frame_count*vocabulary_size,
                                            shape,
                                            2);
}

static bool
lrc_ctc_fake_inference_run(
    void *backend,
    LrcCtcModelInput *input,
    LrcCtcEmissions *emissions,
    enum LrcCtcEmissionValuesKind values_kind,
    bool print_progress,
    LrcCtcInferenceResult *result
) {
    LrcCtcFakeInference *fake = backend;

    if (fake == NULL) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "fake CTC inference backend is missing",
            -1
        );
        return false;
    }

    return lrc_ctc_emissions_build_trimmed_from_model_output(
        emissions,
        input,
        fake->values,
        fake->value_count,
        fake->shape,
        fake->shape_len,
        values_kind,
        print_progress,
        result
    );
}

static void
lrc_ctc_fake_inference_backend(
    LrcCtcFakeInference *fake,
    LrcCtcInferenceBackend *backend
) {
    if (backend == NULL) {
        return;
    }

    backend->backend = fake;
    backend->run = lrc_ctc_fake_inference_run;
    backend->values_kind = LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES;
    backend->print_progress = false;

    return;
}
#endif


static void
lrc_ctc_onnx_inference_destroy(LrcCtcOnnxInference *onnx) {
    if (onnx == NULL) {
        return;
    }

#if LRC_CTC_INFERENCE_ENABLE_ORT
    if (onnx->loaded) {
        ort_model_destroy(&onnx->context, &onnx->model);
        ort_context_destroy(&onnx->context);
    }
#endif

    memset64(onnx, 0, SIZEOF(*onnx));

    return;
}

#if LRC_CTC_INFERENCE_ENABLE_ORT
static bool
lrc_ctc_onnx_model_input_info(
    LrcCtcOnnxInference *onnx,
    LrcCtcModelIoInfo *info
) {
    OrtModelIoInfo ort_info;

    if ((onnx == NULL) || (info == NULL)) {
        return false;
    }
    memset64(info, 0, SIZEOF(*info));
    if (!ort_model_input_info(&onnx->model, &ort_info)) {
        return false;
    }
    if ((ort_info.count != 1)
        || (ort_info.shape_len != LRC_CTC_MODEL_INPUT_RANK)) {
        return false;
    }

    info->count = ort_info.count;
    info->shape_len = ort_info.shape_len;
    info->is_float32 = true;
    for (int32 i = 0; i < LRC_CTC_MODEL_INPUT_RANK; i += 1) {
        info->shape[i] = ort_info.shape[i];
    }

    return true;
}
#endif

static bool
lrc_ctc_onnx_validate_model_input(
    LrcCtcModelInput *input,
    LrcCtcModelIoInfo *input_info,
    LrcCtcModelInputResult *input_result
) {
    LrcCtcModelInput chunk_input;

    if ((input == NULL) || (input_info == NULL)) {
        lrc_ctc_model_input_result_init(input_result);
        return false;
    }

    chunk_input = *input;
    if (input->chunked && (input->chunk_count > 1)) {
        chunk_input.sample_count = input->row_sample_count;
        chunk_input.shape[0] = 1;
        chunk_input.shape[1] = input->row_sample_count;
    }

    return lrc_ctc_model_input_validate_model_io(&chunk_input,
                                                 input_info,
                                                 input_result);
}

static bool
lrc_ctc_onnx_chunk_output_shape(
    OrtTensor *output,
    int64 chunk_index,
    int64 *chunk_emission_count,
    int64 *vocabulary_size,
    int64 *chunk_value_count,
    LrcCtcInferenceResult *result
) {
    int64 shape[3];
    int64 row_count;
    int64 row_emission_count;
    int64 total_emission_count;
    int64 vocab;
    int64 value_count;

    if ((output == NULL) || (output->data == NULL)
        || (chunk_emission_count == NULL) || (vocabulary_size == NULL)
        || (chunk_value_count == NULL)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC chunk output shape arguments are invalid",
            chunk_index
        );
        return false;
    }
    *chunk_emission_count = 0;
    *vocabulary_size = 0;
    *chunk_value_count = 0;

    if (output->shape_len == 2) {
        shape[0] = 1;
        shape[1] = output->shape[0];
        shape[2] = output->shape[1];
    } else if (output->shape_len == 3) {
        shape[0] = output->shape[0];
        shape[1] = output->shape[1];
        shape[2] = output->shape[2];
    } else {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC chunk output must have rank 2 or rank 3",
            chunk_index
        );
        return false;
    }

    if (!lrc_ctc_emissions_shape_valid(shape,
                                       3,
                                       &row_count,
                                       &row_emission_count,
                                       &total_emission_count,
                                       &vocab,
                                       &value_count,
                                       result)) {
        return false;
    }
    if (row_count != 1) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC chunk output batch size must be one",
            chunk_index
        );
        return false;
    }
    if (output->data_len != value_count) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC chunk output value count does not match shape",
            chunk_index
        );
        return false;
    }

    *chunk_emission_count = row_emission_count;
    *vocabulary_size = vocab;
    *chunk_value_count = value_count;
    (void)total_emission_count;

    return true;
}

static bool
lrc_ctc_onnx_inference_load(
    LrcCtcOnnxInference *onnx,
    char *model_path,
    OrtSessionConfig *session_config,
    LrcCtcInferenceResult *result
) {
    if (result) {
        lrc_ctc_inference_result_init(result);
    }
    if ((onnx == NULL) || (model_path == NULL) || (model_path[0] == '\0')) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC ONNX inference load received invalid arguments",
            -1
        );
        return false;
    }

#if LRC_CTC_INFERENCE_ENABLE_ORT
    lrc_ctc_onnx_inference_destroy(onnx);
    if (!ort_context_init(&onnx->context)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_MODEL_LOAD_FAILED,
            "could not initialize ONNX Runtime for CTC inference",
            -1
        );
        return false;
    }
    ort_context_session_config_set(&onnx->context, session_config);

    if (!ort_model_load(&onnx->context, &onnx->model, model_path)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_MODEL_LOAD_FAILED,
            "could not load CTC ONNX model",
            -1
        );
        ort_context_destroy(&onnx->context);
        return false;
    }

    onnx->loaded = true;

    return true;
#else
    (void)onnx;
    (void)model_path;
    (void)session_config;
    lrc_ctc_inference_result_set(
        result,
        LS_ERROR_CTC_INFERENCE_BACKEND_UNAVAILABLE,
        "CTC ONNX inference backend is not enabled in this build",
        -1
    );
    return false;
#endif
}

#if LRC_CTC_INFERENCE_ENABLE_ORT
static void
lrc_ctc_onnx_chunked_free_values(float **values, int64 value_count) {
    if ((values == NULL) || (*values == NULL)) {
        return;
    }

    free2(*values, value_count*SIZEOF(**values));
    *values = NULL;

    return;
}

static bool
lrc_ctc_onnx_chunked_prepare_values(
    LrcCtcModelInput *input,
    int64 chunk_value_count,
    float **values,
    int64 *value_count,
    LrcCtcInferenceResult *result
) {
    if ((input == NULL) || (values == NULL) || (value_count == NULL)
        || (chunk_value_count <= 0)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT,
            "CTC chunked output allocation arguments are invalid",
            -1
        );
        return false;
    }
    *values = NULL;
    *value_count = 0;

    if ((input->chunk_count <= 0)
        || (input->chunk_count > INT64_MAX/chunk_value_count)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC chunked output tensor is too large",
            -1
        );
        return false;
    }

    *value_count = input->chunk_count*chunk_value_count;
    if (*value_count > INT64_MAX/SIZEOF(**values)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_OUTPUT_TOO_LARGE,
            "CTC chunked output copy is too large",
            -1
        );
        *value_count = 0;
        return false;
    }

    *values = malloc2(*value_count*SIZEOF(**values));

    return true;
}

static bool
lrc_ctc_onnx_run_one_chunk(
    LrcCtcOnnxInference *onnx,
    LrcCtcModelInput *input,
    int64 chunk_index,
    int64 *shape,
    OrtTensor *output,
    LrcCtcInferenceResult *result
) {
    OrtTensor input_tensor;
    int64 sample_offset;

    ort_tensor_init_empty(&input_tensor);
    ort_tensor_init_empty(output);
    sample_offset = chunk_index*input->row_sample_count;
    if (!ort_tensor_create_f32(&onnx->context,
                               &input_tensor,
                               input->samples + sample_offset,
                               input->row_sample_count,
                               shape,
                               LRC_CTC_MODEL_INPUT_RANK)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_BACKEND_FAILED,
            "could not create CTC ONNX chunk input tensor",
            chunk_index
        );
        return false;
    }
    if (!ort_model_run_f32(&onnx->context,
                           &onnx->model,
                           &input_tensor,
                           output)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_BACKEND_FAILED,
            "could not run CTC ONNX chunk inference",
            chunk_index
        );
        ort_tensor_destroy(&onnx->context, &input_tensor);
        return false;
    }

    ort_tensor_destroy(&onnx->context, &input_tensor);

    return true;
}

static bool
lrc_ctc_onnx_chunked_copy_output(
    LrcCtcModelInput *input,
    OrtTensor *output,
    int64 chunk_index,
    int64 *chunk_emission_count,
    int64 *vocabulary_size,
    int64 *chunk_value_count,
    float **values,
    int64 *value_count,
    LrcCtcInferenceResult *result
) {
    int64 current_emission_count;
    int64 current_vocabulary_size;
    int64 current_value_count;
    int64 output_offset;

    if (!lrc_ctc_onnx_chunk_output_shape(output,
                                         chunk_index,
                                         &current_emission_count,
                                         &current_vocabulary_size,
                                         &current_value_count,
                                         result)) {
        return false;
    }

    if (chunk_index == 0) {
        *chunk_emission_count = current_emission_count;
        *vocabulary_size = current_vocabulary_size;
        *chunk_value_count = current_value_count;
        if (!lrc_ctc_onnx_chunked_prepare_values(input,
                                                 *chunk_value_count,
                                                 values,
                                                 value_count,
                                                 result)) {
            return false;
        }
    } else if ((current_emission_count != *chunk_emission_count)
               || (current_vocabulary_size != *vocabulary_size)
               || (current_value_count != *chunk_value_count)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT,
            "CTC chunk output shape changed between chunks",
            chunk_index
        );
        return false;
    }

    output_offset = chunk_index*(*chunk_value_count);
    memcpy64(*values + output_offset,
             output->data,
             *chunk_value_count*SIZEOF(**values));

    return true;
}

static bool
lrc_ctc_onnx_inference_run_chunked(
    LrcCtcOnnxInference *onnx,
    LrcCtcModelInput *input,
    LrcCtcEmissions *emissions,
    enum LrcCtcEmissionValuesKind values_kind,
    bool print_progress,
    LrcCtcInferenceResult *result
) {
    LrcProgress progress;
    OrtTensor output;
    float *values = NULL;
    int64 input_shape[2];
    int64 output_shape[3];
    int64 chunk_emission_count = 0;
    int64 vocabulary_size = 0;
    int64 chunk_value_count = 0;
    int64 value_count = 0;
    bool ok;

    input_shape[0] = 1;
    input_shape[1] = input->row_sample_count;
    ok = true;

    lrc_progress_init(&progress,
                      print_progress,
                      "run CTC model",
                      input->chunk_count);
    lrc_progress_begin(&progress);
    for (int64 i = 0; i < input->chunk_count; i += 1) {
        ort_tensor_init_empty(&output);
        if (!lrc_ctc_onnx_run_one_chunk(onnx,
                                        input,
                                        i,
                                        input_shape,
                                        &output,
                                        result)) {
            ok = false;
        } else if (!lrc_ctc_onnx_chunked_copy_output(input,
                                                     &output,
                                                     i,
                                                     &chunk_emission_count,
                                                     &vocabulary_size,
                                                     &chunk_value_count,
                                                     &values,
                                                     &value_count,
                                                     result)) {
            ok = false;
        }

        ort_tensor_destroy(&onnx->context, &output);
        if (!ok) {
            break;
        }
        lrc_progress_update(&progress, i + 1);
    }

    if (ok) {
        lrc_progress_finish(&progress);
        output_shape[0] = input->chunk_count;
        output_shape[1] = chunk_emission_count;
        output_shape[2] = vocabulary_size;
        ok = lrc_ctc_emissions_build_trimmed_from_model_output(
            emissions,
            input,
            values,
            value_count,
            output_shape,
            3,
            values_kind,
            print_progress,
            result
        );
    } else {
        lrc_progress_cancel(&progress);
    }

    lrc_ctc_onnx_chunked_free_values(&values, value_count);

    return ok;
}
#endif

static bool
lrc_ctc_onnx_inference_run(
    void *backend,
    LrcCtcModelInput *input,
    LrcCtcEmissions *emissions,
    enum LrcCtcEmissionValuesKind values_kind,
    bool print_progress,
    LrcCtcInferenceResult *result
) {
#if LRC_CTC_INFERENCE_ENABLE_ORT
    LrcCtcOnnxInference *onnx = backend;
    LrcCtcModelInputResult input_result;
    LrcCtcModelIoInfo input_info;
    OrtTensor input_tensor;
    OrtTensor output_tensor;
    LrcProgress progress;
    bool ok;

    if ((onnx == NULL) || !onnx->loaded) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_BACKEND_UNAVAILABLE,
            "CTC ONNX inference backend is not loaded",
            -1
        );
        return false;
    }
    if (!lrc_ctc_onnx_model_input_info(onnx, &input_info)
        || !lrc_ctc_onnx_validate_model_input(input,
                                              &input_info,
                                              &input_result)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_INVALID_INPUT,
            "CTC model input does not match ONNX model input",
            -1
        );
        return false;
    }

    if (input->chunked && (input->chunk_count > 1)) {
        return lrc_ctc_onnx_inference_run_chunked(onnx,
                                                  input,
                                                  emissions,
                                                  values_kind,
                                                  print_progress,
                                                  result);
    }

    ort_tensor_init_empty(&input_tensor);
    ort_tensor_init_empty(&output_tensor);
    if (!ort_tensor_create_f32(&onnx->context,
                               &input_tensor,
                               input->samples,
                               input->sample_count,
                               input->shape,
                               input->shape_len)) {
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_BACKEND_FAILED,
            "could not create CTC ONNX input tensor",
            -1
        );
        return false;
    }

    lrc_progress_init(&progress,
                      print_progress,
                      "run CTC model",
                      input->chunk_count);
    lrc_progress_begin(&progress);
    ok = ort_model_run_f32(&onnx->context,
                           &onnx->model,
                           &input_tensor,
                           &output_tensor);
    if (ok) {
        lrc_progress_finish(&progress);
        ok = lrc_ctc_emissions_build_trimmed_from_model_output(
            emissions,
            input,
            output_tensor.data,
            output_tensor.data_len,
            output_tensor.shape,
            output_tensor.shape_len,
            values_kind,
            print_progress,
            result
        );
    } else {
        lrc_progress_cancel(&progress);
        lrc_ctc_inference_result_set(
            result,
            LS_ERROR_CTC_INFERENCE_BACKEND_FAILED,
            "could not run CTC ONNX inference",
            -1
        );
    }

    ort_tensor_destroy(&onnx->context, &output_tensor);
    ort_tensor_destroy(&onnx->context, &input_tensor);

    return ok;
#else
    (void)backend;
    (void)input;
    (void)emissions;
    (void)values_kind;
    (void)print_progress;
    lrc_ctc_inference_result_set(
        result,
        LS_ERROR_CTC_INFERENCE_BACKEND_UNAVAILABLE,
        "CTC ONNX inference backend is not enabled in this build",
        -1
    );
    return false;
#endif
}

static void
lrc_ctc_onnx_inference_backend(
    LrcCtcOnnxInference *onnx,
    LrcCtcInferenceBackend *backend
) {
    if (backend == NULL) {
        return;
    }

    backend->backend = onnx;
    backend->run = lrc_ctc_onnx_inference_run;
    backend->values_kind = LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES;
    backend->print_progress = false;

    return;
}

#if TESTING_ctc_inference
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "ctc_model.c"

static int32
ctc_inference_test_fail(char *name) {
    error2("CTC inference test failed: %s\n", name);

    return 1;
}

static bool
ctc_inference_float_close(float a, float b, float max_error) {
    float diff;

    diff = fabsf(a - b);

    return diff <= max_error;
}

static void
ctc_inference_make_input(LrcCtcModelInput *input) {
    static float samples[] = {0.0f, 0.1f, -0.1f, 0.2f};

    memset64(input, 0, SIZEOF(*input));
    input->samples = samples;
    input->sample_count = LENGTH(samples);
    input->shape_len = LRC_CTC_MODEL_INPUT_RANK;
    input->shape[0] = 1;
    input->shape[1] = LENGTH(samples);

    return;
}


static void
ctc_inference_make_rank3_trim_input(
    LrcCtcModelInput *input,
    LrcCtcModelChunk *chunks,
    int64 original_emission_count
) {
    static float samples[] = {0.0f, 0.1f, -0.1f, 0.2f, 0.3f, -0.3f};

    memset64(input, 0, SIZEOF(*input));
    memset64(chunks, 0, 2*SIZEOF(*chunks));

    input->samples = samples;
    input->sample_count = LENGTH(samples);
    input->shape_len = LRC_CTC_MODEL_INPUT_RANK;
    input->shape[0] = 2;
    input->shape[1] = 3;

    input->chunked = true;
    input->chunk_count = 2;
    input->chunks = chunks;
    input->raw_chunk_emission_count = 3;
    input->window_frame_count = 1;
    input->context_frame_count = 1;
    input->kept_emission_count = 2;
    input->original_emission_count = original_emission_count;

    for (int64 i = 0; i < input->chunk_count; i += 1) {
        LrcCtcModelChunk *chunk = &chunks[i];

        chunk->raw_emission_start = i*input->raw_chunk_emission_count;
        chunk->raw_emission_count = input->raw_chunk_emission_count;
        chunk->trim_left_emissions = 1;
        chunk->trim_right_emissions = 1;
        chunk->kept_emission_start = chunk->raw_emission_start + 1;
        chunk->kept_emission_count = 1;
    }

    return;
}


static void
ctc_inference_make_custom_rank3_trim_input(
    LrcCtcModelInput *input,
    LrcCtcModelChunk *chunks,
    int64 chunk_count,
    int64 raw_chunk_emission_count,
    int64 trim_left,
    int64 trim_right,
    int64 original_emission_count
) {
    static float samples[] = {0.0f};
    int64 kept_count;

    memset64(input, 0, SIZEOF(*input));
    memset64(chunks, 0, chunk_count*SIZEOF(*chunks));
    kept_count = raw_chunk_emission_count - trim_left - trim_right;

    input->samples = samples;
    input->sample_count = 1;
    input->shape_len = LRC_CTC_MODEL_INPUT_RANK;
    input->shape[0] = chunk_count;
    input->shape[1] = raw_chunk_emission_count;

    input->chunked = true;
    input->chunk_count = chunk_count;
    input->chunks = chunks;
    input->raw_chunk_emission_count = raw_chunk_emission_count;
    input->window_frame_count = kept_count;
    input->context_frame_count = trim_left;
    input->kept_emission_count = kept_count*chunk_count;
    input->original_emission_count = original_emission_count;

    for (int64 i = 0; i < chunk_count; i += 1) {
        LrcCtcModelChunk *chunk = &chunks[i];

        chunk->raw_emission_start = i*raw_chunk_emission_count;
        chunk->raw_emission_count = raw_chunk_emission_count;
        chunk->trim_left_emissions = trim_left;
        chunk->trim_right_emissions = trim_right;
        chunk->kept_emission_start = chunk->raw_emission_start + trim_left;
        chunk->kept_emission_count = kept_count;
    }

    return;
}

typedef struct CtcInferenceStderrCapture {
    FILE *file;

    int32 saved_stderr;
    int32 fd;
} CtcInferenceStderrCapture;

static bool
ctc_inference_stderr_capture_begin(
    CtcInferenceStderrCapture *capture
) {
    if (capture == NULL) {
        return false;
    }

    capture->file = tmpfile();
    capture->saved_stderr = -1;
    capture->fd = -1;
    if (capture->file == NULL) {
        return false;
    }

    fflush(stderr);
    capture->fd = fileno(capture->file);
    capture->saved_stderr = dup(STDERR_FILENO);
    if ((capture->fd < 0) || (capture->saved_stderr < 0)) {
        if (capture->saved_stderr >= 0) {
            XCLOSE(&capture->saved_stderr);
        }
        fclose(capture->file);
        capture->file = NULL;
        capture->fd = -1;
        return false;
    }
    xdup2(capture->fd, STDERR_FILENO);

    return true;
}

static int64
ctc_inference_stderr_capture_end(
    CtcInferenceStderrCapture *capture,
    char *buffer,
    int32 buffer_len
) {
    int64 len;

    if ((capture == NULL) || (capture->file == NULL)) {
        return -1;
    }

    fflush(stderr);
    len = (int64)lseek(capture->fd, 0, SEEK_END);
    if (buffer && (buffer_len > 0)) {
        int64 read_limit;
        int64 read_len;

        buffer[0] = '\0';
        read_limit = buffer_len - 1;
        if (read_limit > 0) {
            lseek(capture->fd, 0, SEEK_SET);
            read_len = (int64)read(capture->fd,
                                   buffer,
                                   (size_t)read_limit);
            if (read_len < 0) {
                read_len = 0;
            }
            buffer[read_len] = '\0';
        }
    }

    xdup2(capture->saved_stderr, STDERR_FILENO);
    XCLOSE(&capture->saved_stderr);
    fclose(capture->file);
    capture->file = NULL;
    capture->fd = -1;

    return len;
}

static void
ctc_inference_test_empty_initializers(void) {
    LrcCtcInferenceResult result;
    LrcCtcEmissions emissions = {0};
    LrcCtcFakeInference fake = {0};
    LrcCtcInferenceBackend backend;
    LrcCtcOnnxInference onnx = {0};

    lrc_ctc_inference_result_init(&result);
    lrc_ctc_fake_inference_backend(&fake, &backend);

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.header.message, "ok"));
    ASSERT(result.output_index == -1);

    ASSERT(emissions.values == NULL);
    ASSERT(emissions.value_count == 0);
    ASSERT(emissions.frame_count == 0);
    ASSERT(emissions.vocabulary_size == 0);

    ASSERT(fake.values == NULL);
    ASSERT(fake.value_count == 0);
    ASSERT(fake.shape_len == 0);

    ASSERT(backend.backend == &fake);
    ASSERT(backend.run == lrc_ctc_fake_inference_run);

    ASSERT(!onnx.loaded);

    return;
}

static void
ctc_inference_test_fake_rank2(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    float values[] = {
        -2.0f, -0.1f, -3.0f,
        -1.0f, -4.0f, -0.2f,
    };

    ctc_inference_make_input(&input);
    if (!lrc_ctc_fake_inference_set(&fake, values, 2, 3)) {
        fatal(ctc_inference_test_fail("set fake rank-2 emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run fake rank-2 backend"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(emissions.value_count == 6);
    ASSERT(emissions.row_count == 1);
    ASSERT(emissions.row_frame_count == 2);
    ASSERT(emissions.frame_count == 2);
    ASSERT(emissions.vocabulary_size == 3);
    ASSERT(emissions.shape_len == 2);
    ASSERT(emissions.shape[0] == 2);
    ASSERT(emissions.shape[1] == 3);
    ASSERT(emissions.values[0] == values[0]);
    ASSERT(emissions.values[5] == values[5]);

    values[0] = 99.0f;
    ASSERT(emissions.values[0] == -2.0f);

    emissions.values[1] = NAN;
    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_fake_rank3(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[12];

    for (int32 i = 0; i < LENGTH(values); i += 1) {
        values[i] = (float)i;
    }
    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 2;

    ctc_inference_make_rank3_trim_input(&input, chunks, 2);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set fake rank-3 emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run fake rank-3 backend"));
    }

    ASSERT(emissions.value_count == 4);
    ASSERT(emissions.row_count == 1);
    ASSERT(emissions.row_frame_count == 2);
    ASSERT(emissions.frame_count == 2);
    ASSERT(emissions.vocabulary_size == 2);
    ASSERT(emissions.shape_len == 2);
    ASSERT(emissions.shape[0] == 2);
    ASSERT(emissions.shape[1] == 2);
    ASSERT(emissions.values[0] == 2.0f);
    ASSERT(emissions.values[1] == 3.0f);
    ASSERT(emissions.values[2] == 8.0f);
    ASSERT(emissions.values[3] == 9.0f);

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_rank3_extension_truncated(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[12];

    for (int32 i = 0; i < LENGTH(values); i += 1) {
        values[i] = (float)i;
    }
    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 2;

    ctc_inference_make_rank3_trim_input(&input, chunks, 1);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set padded rank-3 emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run padded rank-3 backend"));
    }

    ASSERT(emissions.value_count == 2);
    ASSERT(emissions.frame_count == 1);
    ASSERT(emissions.vocabulary_size == 2);
    ASSERT(emissions.shape_len == 2);
    ASSERT(emissions.shape[0] == 1);
    ASSERT(emissions.shape[1] == 2);
    ASSERT(emissions.values[0] == 2.0f);
    ASSERT(emissions.values[1] == 3.0f);

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_rank3_logits_converted_after_trim(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[] = {
        10000.0f, -10000.0f,
        0.0f, 0.0f,
        -10000.0f, 10000.0f,
        5000.0f, -5000.0f,
        2.0f, 0.0f,
        -5000.0f, 5000.0f,
    };
    double row0_norm;
    double row1_norm;

    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 2;
    row0_norm = log(2.0);
    row1_norm = log(exp(2.0) + 1.0);

    ctc_inference_make_rank3_trim_input(&input, chunks, 2);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set rank-3 logits"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);
    backend.values_kind = LRC_CTC_EMISSION_VALUES_LOGITS;

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run rank-3 logits backend"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(emissions.frame_count == 2);
    ASSERT(emissions.vocabulary_size == 2);
    ASSERT(ctc_inference_float_close(emissions.values[0],
                                     (float)-row0_norm,
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[1],
                                     (float)-row0_norm,
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[2],
                                     (float)(2.0 - row1_norm),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[3],
                                     (float)-row1_norm,
                                     0.00001f));

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_rank3_probability_trim_before_convert(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[] = {
        0.0f, -1.0f,
        0.25f, 0.75f,
        -2.0f, 0.0f,
        0.0f, -3.0f,
        0.90f, 0.10f,
        -4.0f, 0.0f,
    };

    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 2;

    ctc_inference_make_rank3_trim_input(&input, chunks, 2);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set rank-3 probabilities"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);
    backend.values_kind = LRC_CTC_EMISSION_VALUES_PROBABILITIES;

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run rank-3 probabilities backend"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(emissions.frame_count == 2);
    ASSERT(emissions.vocabulary_size == 2);
    ASSERT(ctc_inference_float_close(emissions.values[0],
                                     logf(0.25f),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[1],
                                     logf(0.75f),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[2],
                                     logf(0.90f),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[3],
                                     logf(0.10f),
                                     0.00001f));

    lrc_ctc_emissions_destroy(&emissions);

    return;
}


static void
ctc_inference_test_rank3_accepts_short_actual_model_length(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input = {0};
    LrcCtcModelChunk chunk = {0};
    int64 shape[3];
    float values[] = {
        10.0f,
        11.0f,
        12.0f,
    };

    shape[0] = 1;
    shape[1] = 3;
    shape[2] = 1;

    input.samples = values;
    input.sample_count = 4;
    input.shape_len = LRC_CTC_MODEL_INPUT_RANK;
    input.shape[0] = 1;
    input.shape[1] = 4;
    input.chunk_count = 1;
    input.chunks = &chunk;
    input.raw_chunk_emission_count = 4;
    input.original_emission_count = 4;
    input.kept_emission_count = 4;
    chunk.raw_emission_count = 4;
    chunk.kept_emission_count = 4;

    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set short actual rank-3 output"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run short actual rank-3 output"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(emissions.frame_count == 3);
    ASSERT(emissions.value_count == 3);
    ASSERT(emissions.values[0] == 10.0f);
    ASSERT(emissions.values[1] == 11.0f);
    ASSERT(emissions.values[2] == 12.0f);

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_rank3_accepts_wav2vec_actual_chunk_length(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[10];
    float expected[] = {
        2.0f,
        3.0f,
        4.0f,
        102.0f,
        103.0f,
        104.0f,
    };

    for (int64 chunk = 0; chunk < 2; chunk += 1) {
        for (int64 frame = 0; frame < 5; frame += 1) {
            values[chunk*5 + frame] = (float)(chunk*100 + frame);
        }
    }
    shape[0] = 2;
    shape[1] = 5;
    shape[2] = 1;

    ctc_inference_make_custom_rank3_trim_input(&input,
                                               chunks,
                                               2,
                                               6,
                                               2,
                                               1,
                                               LENGTH(expected));
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set actual wav2vec rank-3 output"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run actual wav2vec rank-3 output"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(emissions.frame_count == LENGTH(expected));
    for (int32 i = 0; i < LENGTH(expected); i += 1) {
        ASSERT(emissions.values[i] == expected[i]);
    }

    lrc_ctc_emissions_destroy(&emissions);

    return;
}


static void
ctc_inference_test_rank3_python_slicing_vectors(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[3];
    int64 shape[3];
    float values[18];
    float expected[] = {
        2.0f,
        3.0f,
        4.0f,
        102.0f,
        103.0f,
        104.0f,
        202.0f,
        203.0f,
    };

    for (int64 chunk = 0; chunk < 3; chunk += 1) {
        for (int64 frame = 0; frame < 6; frame += 1) {
            values[chunk*6 + frame] = (float)(chunk*100 + frame);
        }
    }
    shape[0] = 3;
    shape[1] = 6;
    shape[2] = 1;

    ctc_inference_make_custom_rank3_trim_input(&input,
                                               chunks,
                                               3,
                                               6,
                                               2,
                                               1,
                                               LENGTH(expected));
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set Python slicing vector"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run Python slicing vector"));
    }

    ASSERT(emissions.frame_count == LENGTH(expected));
    ASSERT(emissions.vocabulary_size == 1);
    for (int32 i = 0; i < LENGTH(expected); i += 1) {
        ASSERT(emissions.values[i] == expected[i]);
    }

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_rank3_repeated_boundary_frames(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[] = {
        88.0f,
        89.0f,
        0.0f,
        1.0f,
        2.0f,
        3.0f,
        0.0f,
        1.0f,
        2.0f,
        3.0f,
        90.0f,
        91.0f,
    };
    float expected[] = {
        0.0f,
        1.0f,
        2.0f,
        3.0f,
    };

    shape[0] = 2;
    shape[1] = 6;
    shape[2] = 1;

    ctc_inference_make_custom_rank3_trim_input(&input,
                                               chunks,
                                               2,
                                               6,
                                               2,
                                               2,
                                               LENGTH(expected));
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set repeated-boundary emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("run repeated-boundary emissions"));
    }

    ASSERT(emissions.frame_count == LENGTH(expected));
    for (int32 i = 0; i < LENGTH(expected); i += 1) {
        ASSERT(emissions.values[i] == expected[i]);
    }

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_progress_disabled_is_silent(void) {
    CtcInferenceStderrCapture capture;
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    int64 stderr_len;
    bool ok;
    float values[12];

    for (int32 i = 0; i < LENGTH(values); i += 1) {
        values[i] = (float)i;
    }
    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 2;

    ctc_inference_make_rank3_trim_input(&input, chunks, 2);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set silent progress emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);
    backend.print_progress = false;

    if (!ctc_inference_stderr_capture_begin(&capture)) {
        fatal(ctc_inference_test_fail("start stderr capture"));
    }
    ok = lrc_ctc_inference_run(&backend, &input, &emissions, &result);
    stderr_len = ctc_inference_stderr_capture_end(&capture, NULL, 0);
    if (!ok) {
        fatal(ctc_inference_test_fail("run silent progress backend"));
    }

    ASSERT(stderr_len == 0);
    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_progress_counts_chunks(void) {
    CtcInferenceStderrCapture capture;
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    char output[512];
    int64 shape[3];
    bool ok;
    float values[12];

    for (int32 i = 0; i < LENGTH(values); i += 1) {
        values[i] = (float)i;
    }
    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 2;

    ctc_inference_make_rank3_trim_input(&input, chunks, 2);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set chunk progress emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);
    backend.print_progress = true;

    if (!ctc_inference_stderr_capture_begin(&capture)) {
        fatal(ctc_inference_test_fail("start chunk progress capture"));
    }
    ok = lrc_ctc_inference_run(&backend, &input, &emissions, &result);
    ctc_inference_stderr_capture_end(&capture, output, LENGTH(output));
    if (!ok) {
        fatal(ctc_inference_test_fail("run chunk progress backend"));
    }

    ASSERT_CONTAINS(output, strlen32(output), "2/2");
    ASSERT_NOT_CONTAINS(output, strlen32(output), "4/4");
    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_onnx_chunk_output_shape(void) {
    LrcCtcInferenceResult result;
    OrtTensor output = {0};
    float values[12];
    int64 chunk_emission_count;
    int64 vocabulary_size;
    int64 chunk_value_count;

    lrc_ctc_inference_result_init(&result);
    output.data = values;
    output.data_len = LENGTH(values);
    output.shape_len = 3;
    output.shape[0] = 1;
    output.shape[1] = 3;
    output.shape[2] = 4;
    if (!lrc_ctc_onnx_chunk_output_shape(&output,
                                         0,
                                         &chunk_emission_count,
                                         &vocabulary_size,
                                         &chunk_value_count,
                                         &result)) {
        fatal(ctc_inference_test_fail("accept rank-3 chunk output"));
    }
    ASSERT(chunk_emission_count == 3);
    ASSERT(vocabulary_size == 4);
    ASSERT(chunk_value_count == LENGTH(values));

    output.shape_len = 2;
    output.shape[0] = 6;
    output.shape[1] = 2;
    if (!lrc_ctc_onnx_chunk_output_shape(&output,
                                         0,
                                         &chunk_emission_count,
                                         &vocabulary_size,
                                         &chunk_value_count,
                                         &result)) {
        fatal(ctc_inference_test_fail("accept rank-2 chunk output"));
    }
    ASSERT(chunk_emission_count == 6);
    ASSERT(vocabulary_size == 2);
    ASSERT(chunk_value_count == LENGTH(values));

    output.shape_len = 3;
    output.shape[0] = 2;
    output.shape[1] = 3;
    output.shape[2] = 2;
    if (lrc_ctc_onnx_chunk_output_shape(&output,
                                        0,
                                        &chunk_emission_count,
                                        &vocabulary_size,
                                        &chunk_value_count,
                                        &result)) {
        fatal(ctc_inference_test_fail("multi-row chunk output accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT);

    return;
}

static void
ctc_inference_test_rank3_rejects_mismatched_chunks(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    LrcCtcModelChunk chunks[2];
    int64 shape[3];
    float values[6];

    for (int32 i = 0; i < LENGTH(values); i += 1) {
        values[i] = (float)i;
    }
    shape[0] = 1;
    shape[1] = 3;
    shape[2] = 2;

    ctc_inference_make_rank3_trim_input(&input, chunks, 2);
    if (!lrc_ctc_fake_inference_set_shape(&fake,
                                          values,
                                          LENGTH(values),
                                          shape,
                                          3)) {
        fatal(ctc_inference_test_fail("set mismatched rank-3 emissions"));
    }
    lrc_ctc_fake_inference_backend(&fake, &backend);

    if (lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("mismatched rank-3 chunks accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_OUTPUT);

    return;
}

static void
ctc_inference_test_rejects_invalid_inputs(void) {
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcFakeInference fake = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    float values[] = {0.0f, 1.0f};

    lrc_ctc_fake_inference_backend(&fake, &backend);
    ctc_inference_make_input(&input);

    if (lrc_ctc_inference_run(NULL, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("missing backend accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT);

    if (lrc_ctc_inference_run(&backend, NULL, &emissions, &result)) {
        fatal(ctc_inference_test_fail("missing input accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT);

    input.samples = NULL;
    if (lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        fatal(ctc_inference_test_fail("unprepared input accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_INPUT);
    ctc_inference_make_input(&input);

    if (lrc_ctc_fake_inference_set(&fake, values, 1, 2)) {
        fake.values[1] = NAN;
        if (lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
            fatal(ctc_inference_test_fail("non-finite emissions accepted"));
        }
        ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_NON_FINITE_OUTPUT);
        ASSERT(result.output_index == 1);
    } else {
        fatal(ctc_inference_test_fail("set non-finite fake emissions"));
    }

    return;
}

static void
ctc_inference_test_rejects_bad_shapes(void) {
    LrcCtcFakeInference fake = {0};
    int64 shape[3];
    float values[] = {0.0f, 1.0f, 2.0f, 3.0f};


    shape[0] = 0;
    shape[1] = 2;
    if (lrc_ctc_fake_inference_set_shape(&fake, values, 0, shape, 2)) {
        fatal(ctc_inference_test_fail("zero shape accepted"));
    }

    shape[0] = 2;
    shape[1] = 2;
    if (lrc_ctc_fake_inference_set_shape(&fake, values, 3, shape, 2)) {
        fatal(ctc_inference_test_fail("mismatched value count accepted"));
    }

    shape[0] = 1;
    shape[1] = 2;
    shape[2] = 2;
    if (!lrc_ctc_fake_inference_set_shape(&fake, values, 4, shape, 3)) {
        fatal(ctc_inference_test_fail("valid rank-3 fake shape rejected"));
    }

    return;
}

static void
ctc_inference_test_log_probability_bypass(void) {
    LrcCtcInferenceResult result;
    LrcCtcEmissions emissions = {0};
    int64 shape[2];
    float values[] = {
        -0.10f, -2.30f,
        -1.20f, -0.40f,
    };

    shape[0] = 2;
    shape[1] = 2;

    if (!lrc_ctc_emissions_copy_shape(&emissions,
                                      values,
                                      LENGTH(values),
                                      shape,
                                      2,
                                      &result)) {
        fatal(ctc_inference_test_fail("copy log-probability emissions"));
    }
    if (!lrc_ctc_emissions_convert_to_log_probabilities(
            &emissions,
            LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES,
            &result)) {
        fatal(ctc_inference_test_fail("bypass log probabilities"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    for (int32 i = 0; i < LENGTH(values); i += 1) {
        ASSERT(emissions.values[i] == values[i]);
    }

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_logits_to_log_probabilities(void) {
    LrcCtcInferenceResult result;
    LrcCtcEmissions emissions = {0};
    int64 shape[2];
    double row1_norm;
    double row2_norm;
    double row1_sum;
    double row2_sum;
    float values[] = {
        0.0f, 0.0f,
        1000.0f, 999.0f,
    };

    shape[0] = 2;
    shape[1] = 2;
    row1_norm = log(2.0);
    row2_norm = log(1.0 + exp(-1.0));

    if (!lrc_ctc_emissions_copy_shape(&emissions,
                                      values,
                                      LENGTH(values),
                                      shape,
                                      2,
                                      &result)) {
        fatal(ctc_inference_test_fail("copy logits emissions"));
    }
    if (!lrc_ctc_emissions_convert_to_log_probabilities(
            &emissions,
            LRC_CTC_EMISSION_VALUES_LOGITS,
            &result)) {
        fatal(ctc_inference_test_fail("convert logits to log probabilities"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(ctc_inference_float_close(emissions.values[0],
                                     (float)-row1_norm,
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[1],
                                     (float)-row1_norm,
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[2],
                                     (float)-row2_norm,
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[3],
                                     (float)(-1.0 - row2_norm),
                                     0.00001f));

    row1_sum = exp((double)emissions.values[0])
               + exp((double)emissions.values[1]);
    row2_sum = exp((double)emissions.values[2])
               + exp((double)emissions.values[3]);
    ASSERT(fabs(row1_sum - 1.0) < 0.000001);
    ASSERT(fabs(row2_sum - 1.0) < 0.000001);

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_probabilities_to_log_probabilities(void) {
    LrcCtcInferenceResult result;
    LrcCtcEmissions emissions = {0};
    int64 shape[2];
    float values[] = {
        0.25f, 0.75f,
        0.90f, 0.10f,
    };

    shape[0] = 2;
    shape[1] = 2;

    if (!lrc_ctc_emissions_copy_shape(&emissions,
                                      values,
                                      LENGTH(values),
                                      shape,
                                      2,
                                      &result)) {
        fatal(ctc_inference_test_fail("copy probability emissions"));
    }
    if (!lrc_ctc_emissions_convert_to_log_probabilities(
            &emissions,
            LRC_CTC_EMISSION_VALUES_PROBABILITIES,
            &result)) {
        fatal(ctc_inference_test_fail("convert probabilities"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(ctc_inference_float_close(emissions.values[0],
                                     logf(values[0]),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[1],
                                     logf(values[1]),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[2],
                                     logf(values[2]),
                                     0.00001f));
    ASSERT(ctc_inference_float_close(emissions.values[3],
                                     logf(values[3]),
                                     0.00001f));

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_rejects_invalid_probability_conversion(void) {
    LrcCtcInferenceResult result;
    LrcCtcEmissions emissions = {0};
    int64 shape[2];
    float values[] = {1.0f, 0.0f};

    shape[0] = 1;
    shape[1] = 2;
    if (!lrc_ctc_emissions_copy_shape(&emissions,
                                      values,
                                      LENGTH(values),
                                      shape,
                                      2,
                                      &result)) {
        fatal(ctc_inference_test_fail("copy invalid probabilities"));
    }
    if (lrc_ctc_emissions_convert_to_log_probabilities(
            &emissions,
            LRC_CTC_EMISSION_VALUES_PROBABILITIES,
            &result)) {
        fatal(ctc_inference_test_fail("zero probability accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_PROBABILITY);
    ASSERT(result.output_index == 1);

    values[1] = 1.0f;
    if (!lrc_ctc_emissions_copy_shape(&emissions,
                                      values,
                                      LENGTH(values),
                                      shape,
                                      2,
                                      &result)) {
        fatal(ctc_inference_test_fail("copy valid probabilities"));
    }
    if (lrc_ctc_emissions_convert_to_log_probabilities(
            &emissions,
            (enum LrcCtcEmissionValuesKind)777,
            &result)) {
        fatal(ctc_inference_test_fail("invalid value kind accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_INVALID_ARGUMENT);

    lrc_ctc_emissions_destroy(&emissions);

    return;
}

static void
ctc_inference_test_optional_onnx_backend(void) {
#if LRC_CTC_INFERENCE_ENABLE_ORT
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult result;
    LrcCtcOnnxInference onnx = {0};
    LrcCtcEmissions emissions = {0};
    LrcCtcModelInput input;
    char *model_path;

    model_path = getenv("LRC_TEST_CTC_MODEL");
    if ((model_path == NULL) || (model_path[0] == '\0')) {
        return;
    }

    ctc_inference_make_input(&input);
    if (!lrc_ctc_onnx_inference_load(&onnx,
                                      model_path,
                                      NULL,
                                      &result)) {
        fatal(ctc_inference_test_fail("load optional ONNX CTC model"));
    }
    lrc_ctc_onnx_inference_backend(&onnx, &backend);
    if (!lrc_ctc_inference_run(&backend, &input, &emissions, &result)) {
        lrc_ctc_onnx_inference_destroy(&onnx);
        fatal(ctc_inference_test_fail("run optional ONNX CTC model"));
    }

    ASSERT(emissions.frame_count > 0);
    ASSERT(emissions.vocabulary_size > 0);

    lrc_ctc_emissions_destroy(&emissions);
    lrc_ctc_onnx_inference_destroy(&onnx);
#else
    LrcCtcInferenceResult result;
    LrcCtcOnnxInference onnx = {0};

    if (lrc_ctc_onnx_inference_load(&onnx,
                                     "missing.onnx",
                                     NULL,
                                     &result)) {
        fatal(ctc_inference_test_fail("disabled ONNX backend loaded"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_INFERENCE_BACKEND_UNAVAILABLE);
#endif

    return;
}

int32
main(void) {
    ctc_inference_test_empty_initializers();
    ctc_inference_test_fake_rank2();
    ctc_inference_test_fake_rank3();
    ctc_inference_test_rank3_extension_truncated();
    ctc_inference_test_rank3_logits_converted_after_trim();
    ctc_inference_test_rank3_probability_trim_before_convert();
    ctc_inference_test_rank3_accepts_short_actual_model_length();
    ctc_inference_test_rank3_accepts_wav2vec_actual_chunk_length();
    ctc_inference_test_rank3_python_slicing_vectors();
    ctc_inference_test_rank3_repeated_boundary_frames();
    ctc_inference_test_progress_disabled_is_silent();
    ctc_inference_test_progress_counts_chunks();
    ctc_inference_test_onnx_chunk_output_shape();
    ctc_inference_test_rank3_rejects_mismatched_chunks();
    ctc_inference_test_rejects_invalid_inputs();
    ctc_inference_test_rejects_bad_shapes();
    ctc_inference_test_log_probability_bypass();
    ctc_inference_test_logits_to_log_probabilities();
    ctc_inference_test_probabilities_to_log_probabilities();
    ctc_inference_test_rejects_invalid_probability_conversion();
    ctc_inference_test_optional_onnx_backend();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_ctc_inference */
