#if !defined(ORT_H)
#define ORT_H

#include "cbase.h"

#define ORT_TENSOR_MAX_RANK 8

enum OrtModelIoKind {
    ORT_MODEL_IO_INPUT,
    ORT_MODEL_IO_OUTPUT,
};

#define ORT_EXECUTION_PROVIDER_NAMES "auto|cpu|cuda"

#define ORT_EXECUTION_PROVIDER_VALUES(XX) \
    XX(ORT_EXECUTION_PROVIDER_AUTO, "auto") \
    XX(ORT_EXECUTION_PROVIDER_CPU, "cpu") \
    XX(ORT_EXECUTION_PROVIDER_CUDA, "cuda")

#define ENUM_NAME OrtExecutionProvider
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ ORT_EXECUTION_PROVIDER_
#define ORT_EXECUTION_PROVIDER_ENUM_FIELD(e, name) XX(e)
#define ENUM_FIELDS \
    ORT_EXECUTION_PROVIDER_VALUES(ORT_EXECUTION_PROVIDER_ENUM_FIELD)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS
#undef ORT_EXECUTION_PROVIDER_ENUM_FIELD

typedef struct OrtSessionConfig {
    enum OrtExecutionProvider execution_provider;
    int32 device_id;

    bool print_info;
} OrtSessionConfig;

typedef struct OrtContext {
    void *api;
    void *environment;
    void *memory_info;
    void *allocator;

    OrtSessionConfig session_config;
} OrtContext;

typedef struct OrtModelIoInfo {
    char *name;

    int64 shape[ORT_TENSOR_MAX_RANK];
    int32 shape_len;
    int32 count;
} OrtModelIoInfo;

typedef struct OrtModel {
    void *session;

    char *input_name;
    char *output_name;

    int64 input_shape[ORT_TENSOR_MAX_RANK];
    int64 output_shape[ORT_TENSOR_MAX_RANK];

    int32 input_shape_len;
    int32 output_shape_len;
    int32 input_count;
    int32 output_count;
} OrtModel;

typedef struct OrtTensor {
    void *value;

    float *data;
    int64 data_len;

    int64 shape[ORT_TENSOR_MAX_RANK];
    int32 shape_len;
} OrtTensor;

static bool ort_execution_provider_parse(
    char *value,
    enum OrtExecutionProvider *provider
);
static void ort_session_config_init(OrtSessionConfig *config);
static void ort_context_init_empty(OrtContext *context);
static void ort_context_session_config_set(
    OrtContext *context,
    OrtSessionConfig *config
);
static bool ort_context_init(OrtContext *context);
static void ort_context_destroy(OrtContext *context);

static void ort_model_io_info_init_empty(OrtModelIoInfo *info);
static bool ort_model_input_info(OrtModel *model, OrtModelIoInfo *info);
static bool ort_model_output_info(OrtModel *model, OrtModelIoInfo *info);

static void ort_model_init_empty(OrtModel *model);
static bool ort_model_load(
    OrtContext *context,
    OrtModel *model,
    char *model_path
);
static bool ort_model_get_io_info(OrtContext *context, OrtModel *model);
static void ort_model_destroy(OrtContext *context, OrtModel *model);

static void ort_tensor_init_empty(OrtTensor *tensor);
static bool ort_tensor_shape_element_count(
    int64 *shape,
    int32 shape_len,
    int64 *element_count
);
static bool ort_tensor_create_f32(
    OrtContext *context,
    OrtTensor *tensor,
    float *data,
    int64 data_len,
    int64 *shape,
    int32 shape_len
);
static bool ort_model_run_f32(
    OrtContext *context,
    OrtModel *model,
    OrtTensor *input,
    OrtTensor *output
);
static void ort_tensor_destroy(OrtContext *context, OrtTensor *tensor);

#endif /* ORT_H */
