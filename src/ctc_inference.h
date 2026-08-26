#if !defined(CTC_INFERENCE_H)
#define CTC_INFERENCE_H

#include "cbase.h"
#include "errors.h"
#include "ctc_model.h"
#include "ort.h"

#define LRC_CTC_EMISSIONS_MAX_RANK 3

#if !defined(LRC_CTC_INFERENCE_ENABLE_ORT)
#define LRC_CTC_INFERENCE_ENABLE_ORT 0
#endif

#define LRC_CTC_EMISSION_VALUES_KIND_NAMES \
    "logits|probabilities|log-probabilities"

#define LRC_CTC_EMISSION_VALUES_KIND_VALUES(XX) \
    XX(LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES, "log-probabilities") \
    XX(LRC_CTC_EMISSION_VALUES_LOGITS, "logits") \
    XX(LRC_CTC_EMISSION_VALUES_PROBABILITIES, "probabilities")

#define ENUM_NAME LrcCtcEmissionValuesKind
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ LRC_CTC_EMISSION_VALUES_
#define LRC_CTC_EMISSION_VALUES_KIND_ENUM_FIELD(e, name) XX(e)
#define ENUM_FIELDS \
    LRC_CTC_EMISSION_VALUES_KIND_VALUES( \
        LRC_CTC_EMISSION_VALUES_KIND_ENUM_FIELD)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS
#undef LRC_CTC_EMISSION_VALUES_KIND_ENUM_FIELD

typedef struct LrcCtcInferenceResult {
    LrcResultHeader header;

    int32 output_index;
} LrcCtcInferenceResult;

typedef struct LrcCtcEmissions {
    float *values;

    int64 value_count;
    int64 row_count;
    int64 row_frame_count;
    int64 frame_count;
    int64 vocabulary_size;
    int64 shape[LRC_CTC_EMISSIONS_MAX_RANK];

    int32 shape_len;
} LrcCtcEmissions;

typedef bool (*LrcCtcInferenceRunFunction)(
    void *backend,
    LrcCtcModelInput *input,
    LrcCtcEmissions *emissions,
    enum LrcCtcEmissionValuesKind values_kind,
    bool print_progress,
    LrcCtcInferenceResult *result
);

typedef struct LrcCtcInferenceBackend {
    void *backend;
    LrcCtcInferenceRunFunction run;

    enum LrcCtcEmissionValuesKind values_kind;

    bool print_progress;
} LrcCtcInferenceBackend;

typedef struct LrcCtcFakeInference {
    float *values;

    int64 shape[LRC_CTC_EMISSIONS_MAX_RANK];
    int64 value_count;
    int32 shape_len;
} LrcCtcFakeInference;

typedef struct LrcCtcOnnxInference {
    OrtContext context;
    OrtModel model;

    bool loaded;
} LrcCtcOnnxInference;

static void lrc_ctc_inference_result_init(LrcCtcInferenceResult *result);
static void lrc_ctc_emissions_destroy(LrcCtcEmissions *emissions);
static bool lrc_ctc_emissions_copy_shape(
    LrcCtcEmissions *emissions,
    float *values,
    int64 value_count,
    int64 *shape,
    int32 shape_len,
    LrcCtcInferenceResult *result
);
static bool lrc_ctc_emissions_convert_to_log_probabilities(
    LrcCtcEmissions *emissions,
    enum LrcCtcEmissionValuesKind values_kind,
    LrcCtcInferenceResult *result
);
static bool lrc_ctc_inference_run(
    LrcCtcInferenceBackend *backend,
    LrcCtcModelInput *input,
    LrcCtcEmissions *emissions,
    LrcCtcInferenceResult *result
);

#if TESTING
static bool lrc_ctc_fake_inference_set_shape(
    LrcCtcFakeInference *fake,
    float *values,
    int64 value_count,
    int64 *shape,
    int32 shape_len
);
static bool lrc_ctc_fake_inference_set(
    LrcCtcFakeInference *fake,
    float *values,
    int64 frame_count,
    int64 vocabulary_size
);
static void lrc_ctc_fake_inference_backend(
    LrcCtcFakeInference *fake,
    LrcCtcInferenceBackend *backend
);
#endif

static void lrc_ctc_onnx_inference_destroy(LrcCtcOnnxInference *onnx);
static bool lrc_ctc_onnx_inference_load(
    LrcCtcOnnxInference *onnx,
    char *model_path,
    OrtSessionConfig *session_config,
    LrcCtcInferenceResult *result
);
static void lrc_ctc_onnx_inference_backend(
    LrcCtcOnnxInference *onnx,
    LrcCtcInferenceBackend *backend
);

#endif /* CTC_INFERENCE_H */
