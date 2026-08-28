#include "cbase.h"
#include "lyricsync.h"

#if !defined(TESTING_ort)
#define TESTING_ort 0
#endif

#include "progress.c"
#include "ort.h"

#if CC_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshift-sign-overflow"
#pragma clang diagnostic ignored "-Wdocumentation"
#pragma clang diagnostic ignored "-Wreserved-macro-identifier"
#pragma clang diagnostic ignored "-Wreserved-identifier"
#endif

#include <onnxruntime_c_api.h>

#if OS_LINUX
#include <dlfcn.h>
#define ORT_CUDA_PRELOAD_CUDNN 1
#else
#define ORT_CUDA_PRELOAD_CUDNN 0
#endif

#if CC_CLANG
#pragma clang diagnostic pop
#endif

typedef struct OrtExecutionProviderInfo {
    enum OrtExecutionProvider provider;
    char *name;
} OrtExecutionProviderInfo;

static OrtExecutionProviderInfo ort_execution_provider_infos[] = {
#define ORT_EXECUTION_PROVIDER_INFO_ENTRY(e, name) {e, name},
    ORT_EXECUTION_PROVIDER_VALUES(ORT_EXECUTION_PROVIDER_INFO_ENTRY)
#undef ORT_EXECUTION_PROVIDER_INFO_ENTRY
};

static bool
ort_execution_provider_parse(
    char *value,
    enum OrtExecutionProvider *provider
) {
    if ((value == NULL) || (provider == NULL)) {
        return false;
    }

    for (int32 i = 0; i < LENGTH(ort_execution_provider_infos); i += 1) {
        if (strequal(value, ort_execution_provider_infos[i].name)) {
            *provider = ort_execution_provider_infos[i].provider;
            return true;
        }
    }

    return false;
}

static void
ort_session_config_init(OrtSessionConfig *config) {
    if (config == NULL) {
        return;
    }

    config->execution_provider = ORT_EXECUTION_PROVIDER_AUTO;
    config->device_id = 0;
    config->print_info = false;

    return;
}

static bool
ort_check(OrtContext *context, OrtStatus *status, char *operation) {
    OrtApi *api;

    if (status == NULL) {
        return true;
    }

    api = (OrtApi *)context->api;
    lrc_progress_end_line();
    error2("%s: %s\n", operation, api->GetErrorMessage(status));
    api->ReleaseStatus(status);

    return false;
}


static bool
ort_cuda_preload_cudnn_library(
    OrtContext *context,
    char *name,
    bool required
) {
#if ORT_CUDA_PRELOAD_CUDNN
    static void *handle;
    char *message;

    if (handle) {
        return true;
    }

    dlerror();
    handle = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (handle) {
        if (context->session_config.print_info) {
            lrc_progress_end_line();
            error2("ONNX Runtime preloaded: %s\n", name);
        }
        return true;
    }

    message = dlerror();
    if (message == NULL) {
        message = "unknown dynamic loader error";
    }
    if (required || context->session_config.print_info) {
        lrc_progress_end_line();
        error2("warning: preloading ONNX CUDA dependency %s: %s; "
               "trying CUDA provider anyway\n",
               name,
               message);
    }

    return false;
#else
    (void)context;
    (void)name;
    (void)required;

    return true;
#endif
}

static bool
ort_provider_check(
    OrtContext *context,
    OrtStatus *status,
    char *operation,
    bool required
) {
    OrtApi *api;

    if (status == NULL) {
        return true;
    }

    api = (OrtApi *)context->api;
    lrc_progress_end_line();
    if (required) {
        error2("%s: %s\n", operation, api->GetErrorMessage(status));
    } else {
        error2("warning: %s: %s; using CPU ONNX provider\n",
               operation,
               api->GetErrorMessage(status));
    }
    api->ReleaseStatus(status);

    return false;
}

static bool
ort_session_options_append_cuda(
    OrtContext *context,
    OrtSessionOptions *options,
    bool required
) {
    OrtApi *api;
    OrtCUDAProviderOptionsV2 *cuda_options;
    OrtStatus *status;
    char device_id[32];
    char *keys[1];
    char *values[1];
    int32 len;

    if (context->session_config.device_id < 0) {
        lrc_progress_end_line();
        error2("ONNX CUDA device must not be negative: %d\n",
               context->session_config.device_id);
        return false;
    }

    ort_cuda_preload_cudnn_library(
        context,
        "libcudnn_cnn.so.9",
        required
    );

    api = (OrtApi *)context->api;
    cuda_options = NULL;
    status = api->CreateCUDAProviderOptions(&cuda_options);
    if (!ort_provider_check(context,
                            status,
                            "creating ONNX CUDA provider options",
                            required)) {
        if (cuda_options) {
            api->ReleaseCUDAProviderOptions(cuda_options);
        }
        return false;
    }

    len = snprintf2(device_id,
                    SIZEOF(device_id),
                    "%d",
                    context->session_config.device_id);
    if ((len <= 0) || (len >= SIZEOF(device_id))) {
        api->ReleaseCUDAProviderOptions(cuda_options);
        lrc_progress_end_line();
        error2("ONNX CUDA device id is too long: %d\n",
               context->session_config.device_id);
        return false;
    }

    keys[0] = "device_id";
    values[0] = device_id;
    status = api->UpdateCUDAProviderOptions(
        cuda_options,
        (char const *const *)keys,
        (char const *const *)values,
        1
    );
    if (!ort_provider_check(context,
                            status,
                            "configuring ONNX CUDA provider",
                            required)) {
        api->ReleaseCUDAProviderOptions(cuda_options);
        return false;
    }

    status = api->SessionOptionsAppendExecutionProvider_CUDA_V2(
        options,
        cuda_options
    );
    api->ReleaseCUDAProviderOptions(cuda_options);
    if (!ort_provider_check(context,
                            status,
                            "enabling ONNX CUDA provider",
                            required)) {
        return false;
    }

    if (context->session_config.print_info) {
        lrc_progress_end_line();
        error2("ONNX Runtime provider: CUDA device %d\n",
               context->session_config.device_id);
    }

    return true;
}

static bool
ort_session_options_configure_provider(
    OrtContext *context,
    OrtSessionOptions *options
) {
    bool required;

    if ((context == NULL) || (options == NULL)) {
        return false;
    }

    switch (context->session_config.execution_provider) {
    case ORT_EXECUTION_PROVIDER_AUTO:
        required = false;
        if (!ort_session_options_append_cuda(context, options, required)) {
            return true;
        }
        return true;
    case ORT_EXECUTION_PROVIDER_CPU:
        if (context->session_config.print_info) {
            lrc_progress_end_line();
            error2("ONNX Runtime provider: CPU\n");
        }
        return true;
    case ORT_EXECUTION_PROVIDER_CUDA:
        required = true;
        return ort_session_options_append_cuda(context, options, required);
    case ORT_EXECUTION_PROVIDER_COUNT:
    default:
        lrc_progress_end_line();
        error2("unknown ONNX provider.\n");
        return false;
    }
}

static bool
ort_model_read_tensor_info(
    OrtContext *context,
    OrtSession *session,
    bool input,
    int64 *shape,
    int32 *shape_len
) {
    OrtApi *api = (OrtApi *)context->api;
    OrtTypeInfo *type_info = NULL;
    OrtStatus *status;
    ONNXTensorElementDataType element_type;
    OrtTensorTypeAndShapeInfo *tensor_info;
    int64_t dims[ORT_TENSOR_MAX_RANK];
    size_t dim_count;

    if (input) {
        status = api->SessionGetInputTypeInfo(session, 0, &type_info);
        if (!ort_check(context, status, "getting ONNX input type info")) {
            return false;
        }
    } else {
        status = api->SessionGetOutputTypeInfo(session, 0, &type_info);
        if (!ort_check(context, status, "getting ONNX output type info")) {
            return false;
        }
    }

    status = api->CastTypeInfoToTensorInfo(
        type_info,
        (OrtTensorTypeAndShapeInfo const **)&tensor_info
    );
    if (!ort_check(context, status, "casting ONNX type info to tensor info")) {
        api->ReleaseTypeInfo(type_info);
        return false;
    }
    if (tensor_info == NULL) {
        error2("ONNX model I/O is not a tensor\n");
        api->ReleaseTypeInfo(type_info);
        return false;
    }

    status = api->GetTensorElementType(tensor_info, &element_type);
    if (!ort_check(context, status, "getting ONNX tensor element type")) {
        api->ReleaseTypeInfo(type_info);
        return false;
    }
    if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        error2("ONNX model I/O tensor is not float32\n");
        api->ReleaseTypeInfo(type_info);
        return false;
    }

    status = api->GetDimensionsCount(tensor_info, &dim_count);
    if (!ort_check(context, status, "getting ONNX tensor rank")) {
        api->ReleaseTypeInfo(type_info);
        return false;
    }
    if (dim_count > ORT_TENSOR_MAX_RANK) {
        error2("ONNX tensor rank is too large: %lld\n",
                (int64)dim_count);
        api->ReleaseTypeInfo(type_info);
        return false;
    }

    status = api->GetDimensions(tensor_info, dims, dim_count);
    if (!ort_check(context, status, "getting ONNX tensor shape")) {
        api->ReleaseTypeInfo(type_info);
        return false;
    }

    *shape_len = (int32)dim_count;
    for (int32 i = 0; i < *shape_len; i += 1) {
        shape[i] = (int64)dims[i];
    }

    api->ReleaseTypeInfo(type_info);

    return true;
}


static void
ort_model_io_info_init_empty(OrtModelIoInfo *info) {
    info->name = NULL;

    info->shape_len = 0;
    info->count = 0;

    return;
}

static bool
ort_model_copy_io_info(
    OrtModel *model,
    OrtModelIoInfo *info,
    enum OrtModelIoKind kind
) {
    int64 *source_shape;
    int32 source_shape_len;

    if ((model == NULL) || (info == NULL)) {
        error2("ONNX model I/O info arguments are invalid\n");
        return false;
    }

    ort_model_io_info_init_empty(info);
    if (kind == ORT_MODEL_IO_INPUT) {
        info->name = model->input_name;
        info->count = model->input_count;
        source_shape = model->input_shape;
        source_shape_len = model->input_shape_len;
    } else {
        info->name = model->output_name;
        info->count = model->output_count;
        source_shape = model->output_shape;
        source_shape_len = model->output_shape_len;
    }

    if ((info->count <= 0) || (info->name == NULL)
        || (source_shape_len <= 0)) {
        error2("ONNX model I/O info is not loaded\n");
        return false;
    }
    if (source_shape_len > ORT_TENSOR_MAX_RANK) {
        error2("ONNX model I/O rank is too large: %d\n", source_shape_len);
        return false;
    }

    info->shape_len = source_shape_len;
    for (int32 i = 0; i < info->shape_len; i += 1) {
        info->shape[i] = source_shape[i];
    }

    return true;
}

static bool
ort_model_input_info(OrtModel *model, OrtModelIoInfo *info) {
    return ort_model_copy_io_info(model, info, ORT_MODEL_IO_INPUT);
}

static bool
ort_model_output_info(OrtModel *model, OrtModelIoInfo *info) {
    return ort_model_copy_io_info(model, info, ORT_MODEL_IO_OUTPUT);
}

static void
ort_context_init_empty(OrtContext *context) {
    context->api = NULL;
    context->environment = NULL;
    context->memory_info = NULL;
    context->allocator = NULL;
    ort_session_config_init(&context->session_config);

    return;
}

static void
ort_context_session_config_set(
    OrtContext *context,
    OrtSessionConfig *config
) {
    if (context == NULL) {
        return;
    }
    if (config == NULL) {
        ort_session_config_init(&context->session_config);
        return;
    }

    context->session_config = *config;

    return;
}

static bool
ort_context_init(OrtContext *context) {
    OrtApi *api;
    OrtStatus *status;

    ort_context_init_empty(context);

    context->api = (void *)OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (context->api == NULL) {
        error2("ONNX Runtime API is unavailable\n");
        return false;
    }

    api = (OrtApi *)context->api;
    status = api->CreateEnv(
        ORT_LOGGING_LEVEL_WARNING,
        "uvr-c",
        (OrtEnv **)&context->environment
    );
    if (!ort_check(context, status, "creating ONNX Runtime environment")) {
        ort_context_destroy(context);
        return false;
    }

    status = api->CreateCpuMemoryInfo(
        OrtArenaAllocator,
        OrtMemTypeDefault,
        (OrtMemoryInfo **)&context->memory_info
    );
    if (!ort_check(context, status, "creating ONNX Runtime memory info")) {
        ort_context_destroy(context);
        return false;
    }

    status = api->GetAllocatorWithDefaultOptions(
        (OrtAllocator **)&context->allocator
    );
    if (!ort_check(context, status, "getting ONNX Runtime default allocator")) {
        ort_context_destroy(context);
        return false;
    }

    return true;
}

static void
ort_context_destroy(OrtContext *context) {
    OrtApi *api = (OrtApi *)context->api;

    if (api) {
        if (context->memory_info) {
            api->ReleaseMemoryInfo((OrtMemoryInfo *)context->memory_info);
        }
        if (context->environment) {
            api->ReleaseEnv((OrtEnv *)context->environment);
        }
    }

    ort_context_init_empty(context);

    return;
}

static void
ort_model_init_empty(OrtModel *model) {
    model->session = NULL;

    model->input_name = NULL;
    model->output_name = NULL;

    model->input_shape_len = 0;
    model->output_shape_len = 0;
    model->input_count = 0;
    model->output_count = 0;

    return;
}

static bool
ort_model_load(OrtContext *context, OrtModel *model, char *model_path) {
    OrtApi *api;
    OrtSessionOptions *options;
    OrtStatus *status;

    ort_model_init_empty(model);
    api = (OrtApi *)context->api;
    if ((api == NULL) || (context->environment == NULL)) {
        error2("ONNX Runtime context is not initialized\n");
        return false;
    }

    options = NULL;
    status = api->CreateSessionOptions(&options);
    if (!ort_check(context, status, "creating ONNX Runtime session options")) {
        return false;
    }

    status = api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
    if (!ort_check(context, status, "configuring ONNX Runtime optimizations")) {
        api->ReleaseSessionOptions(options);
        return false;
    }

    if (!ort_session_options_configure_provider(context, options)) {
        api->ReleaseSessionOptions(options);
        return false;
    }

    status = api->CreateSession(
        (OrtEnv *)context->environment,
        model_path,
        options,
        (OrtSession **)&model->session
    );
    api->ReleaseSessionOptions(options);
    if (!ort_check(context, status, "loading ONNX model")) {
        ort_model_destroy(context, model);
        return false;
    }

    if (!ort_model_get_io_info(context, model)) {
        ort_model_destroy(context, model);
        return false;
    }

    return true;
}

static bool
ort_model_get_io_info(OrtContext *context, OrtModel *model) {
    OrtApi *api = (OrtApi *)context->api;
    OrtStatus *status;
    size_t count;

    if ((api == NULL) || (model->session == NULL)) {
        error2("ONNX Runtime model is not loaded\n");
        return false;
    }

    if (model->input_name) {
        status = api->AllocatorFree((OrtAllocator *)context->allocator,
                                    model->input_name);
        if (!ort_check(context, status, "freeing ONNX input name")) {
            return false;
        }
        model->input_name = NULL;
    }
    if (model->output_name) {
        status = api->AllocatorFree((OrtAllocator *)context->allocator,
                                    model->output_name);
        if (!ort_check(context, status, "freeing ONNX output name")) {
            return false;
        }
        model->output_name = NULL;
    }

    status = api->SessionGetInputCount((OrtSession *)model->session, &count);
    if (!ort_check(context, status, "getting ONNX model input count")) {
        return false;
    }
    if ((count < 1) || (count > INT32_MAX)) {
        error2("unsupported ONNX input count: %lld\n", (int64)count);
        return false;
    }
    model->input_count = (int32)count;

    status = api->SessionGetOutputCount((OrtSession *)model->session, &count);
    if (!ort_check(context, status, "getting ONNX model output count")) {
        return false;
    }
    if ((count < 1) || (count > INT32_MAX)) {
        error2("unsupported ONNX output count: %lld\n", (int64)count);
        return false;
    }
    model->output_count = (int32)count;

    status = api->SessionGetInputName(
        (OrtSession *)model->session,
        0,
        (OrtAllocator *)context->allocator,
        &model->input_name
    );
    if (!ort_check(context, status, "getting ONNX input name")) {
        return false;
    }

    status = api->SessionGetOutputName(
        (OrtSession *)model->session,
        0,
        (OrtAllocator *)context->allocator,
        &model->output_name
    );
    if (!ort_check(context, status, "getting ONNX output name")) {
        return false;
    }

    if (!ort_model_read_tensor_info(
            context,
            (OrtSession *)model->session,
            true,
            model->input_shape,
            &model->input_shape_len)) {
        return false;
    }
    if (!ort_model_read_tensor_info(
            context,
            (OrtSession *)model->session,
            false,
            model->output_shape,
            &model->output_shape_len)) {
        return false;
    }

    return true;
}

static void
ort_model_destroy(OrtContext *context, OrtModel *model) {
    OrtApi *api = (OrtApi *)context->api;
    OrtStatus *status;

    if (api) {
        if (model->input_name) {
            status = api->AllocatorFree((OrtAllocator *)context->allocator,
                                        model->input_name);
            ort_check(context, status, "freeing ONNX input name");
        }
        if (model->output_name) {
            status = api->AllocatorFree((OrtAllocator *)context->allocator,
                                        model->output_name);
            ort_check(context, status, "freeing ONNX output name");
        }
        if (model->session) {
            api->ReleaseSession((OrtSession *)model->session);
        }
    }

    ort_model_init_empty(model);

    return;
}

static void
ort_tensor_init_empty(OrtTensor *tensor) {
    tensor->value = NULL;

    tensor->data = NULL;
    tensor->data_len = 0;

    tensor->shape_len = 0;

    return;
}

static bool
ort_tensor_shape_element_count(
    int64 *shape,
    int32 shape_len,
    int64 *element_count
) {
    int64 count;

    if (element_count == NULL) {
        return false;
    }
    *element_count = 0;
    if ((shape == NULL) || (shape_len <= 0)
        || (shape_len > ORT_TENSOR_MAX_RANK)) {
        return false;
    }

    count = 1;
    for (int32 i = 0; i < shape_len; i += 1) {
        if (shape[i] <= 0) {
            return false;
        }
        if (count > INT64_MAX/shape[i]) {
            return false;
        }
        count *= shape[i];
    }

    *element_count = count;
    return true;
}

static bool
ort_tensor_create_f32(
    OrtContext *context,
    OrtTensor *tensor,
    float *data,
    int64 data_len,
    int64 *shape,
    int32 shape_len
) {
    OrtApi *api;
    OrtStatus *status;
    int64 shape_count;
    int64_t ort_shape[ORT_TENSOR_MAX_RANK];

    ort_tensor_init_empty(tensor);
    api = (OrtApi *)context->api;
    if ((api == NULL) || (context->memory_info == NULL)) {
        error2("ONNX Runtime context is not initialized\n");
        return false;
    }
    if ((data == NULL) || (data_len <= 0)) {
        error2("ONNX tensor data is empty\n");
        return false;
    }
    if (!ort_tensor_shape_element_count(shape, shape_len, &shape_count)) {
        error2("ONNX tensor shape is invalid\n");
        return false;
    }
    if (shape_count != data_len) {
        error2("ONNX tensor shape does not match data length\n");
        return false;
    }

    for (int32 i = 0; i < shape_len; i += 1) {
        ort_shape[i] = (int64_t)shape[i];
        tensor->shape[i] = shape[i];
    }

    status = api->CreateTensorWithDataAsOrtValue(
        (OrtMemoryInfo *)context->memory_info,
        data,
        (size_t)(data_len*SIZEOF(*data)),
        ort_shape,
        (size_t)shape_len,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        (OrtValue **)&tensor->value
    );
    if (!ort_check(context, status, "creating ONNX float32 tensor")) {
        ort_tensor_init_empty(tensor);
        return false;
    }

    tensor->data = data;
    tensor->data_len = data_len;
    tensor->shape_len = shape_len;

    return true;
}

static bool
ort_model_run_f32(
    OrtContext *context,
    OrtModel *model,
    OrtTensor *input,
    OrtTensor *output
) {
    OrtApi *api;
    OrtStatus *status;
    OrtValue *input_value;
    OrtValue *output_value;
    OrtTensorTypeAndShapeInfo *tensor_info;
    ONNXTensorElementDataType element_type;
    int64_t dims[ORT_TENSOR_MAX_RANK];
    size_t dim_count;
    size_t element_count;
    int32 is_tensor;
    char *input_names[1];
    char *output_names[1];

    ort_tensor_init_empty(output);
    api = (OrtApi *)context->api;
    if ((api == NULL) || (model->session == NULL)
        || (input->value == NULL)) {
        error2("ONNX Runtime run arguments are invalid\n");
        return false;
    }

    input_value = (OrtValue *)input->value;
    output_value = NULL;
    input_names[0] = model->input_name;
    output_names[0] = model->output_name;
    status = api->Run(
        (OrtSession *)model->session,
        NULL,
        (char const *const *)input_names,
        (OrtValue const *const *)&input_value,
        1,
        (char const *const *)output_names,
        1,
        &output_value
    );
    if (!ort_check(context, status, "running ONNX model")) {
        return false;
    }

    status = api->IsTensor(output_value, &is_tensor);
    if (!ort_check(context, status, "checking ONNX output tensor")) {
        api->ReleaseValue(output_value);
        return false;
    }
    if (is_tensor == 0) {
        error2("ONNX model output is not a tensor\n");
        api->ReleaseValue(output_value);
        return false;
    }

    status = api->GetTensorMutableData(output_value, (void **)&output->data);
    if (!ort_check(context, status, "getting ONNX output tensor data")) {
        api->ReleaseValue(output_value);
        return false;
    }

    status = api->GetTensorTypeAndShape(output_value, &tensor_info);
    if (!ort_check(context, status, "getting ONNX output tensor shape")) {
        api->ReleaseValue(output_value);
        return false;
    }

    status = api->GetTensorElementType(tensor_info, &element_type);
    if (!ort_check(context, status, "getting ONNX output element type")) {
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }
    if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        error2("ONNX model output tensor is not float32\n");
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }

    status = api->GetDimensionsCount(tensor_info, &dim_count);
    if (!ort_check(context, status, "getting ONNX output rank")) {
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }
    if (dim_count > ORT_TENSOR_MAX_RANK) {
        error2("ONNX output tensor rank is too large: %lld\n",
                (int64)dim_count);
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }

    status = api->GetDimensions(tensor_info, dims, dim_count);
    if (!ort_check(context, status, "getting ONNX output dimensions")) {
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }

    status = api->GetTensorShapeElementCount(tensor_info, &element_count);
    if (!ort_check(context, status, "getting ONNX output element count")) {
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }

    if (element_count > (size_t)INT64_MAX) {
        error2("ONNX output tensor is too large: %llu\n",
                (uint64)element_count);
        api->ReleaseTensorTypeAndShapeInfo(tensor_info);
        api->ReleaseValue(output_value);
        return false;
    }

    output->value = output_value;
    output->data_len = (int64)element_count;
    output->shape_len = (int32)dim_count;
    for (int32 i = 0; i < output->shape_len; i += 1) {
        output->shape[i] = (int64)dims[i];
    }

    api->ReleaseTensorTypeAndShapeInfo(tensor_info);

    return true;
}

static void
ort_tensor_destroy(OrtContext *context, OrtTensor *tensor) {
    OrtApi *api = (OrtApi *)context->api;

    if (api && tensor->value) {
        api->ReleaseValue((OrtValue *)tensor->value);
    }

    ort_tensor_init_empty(tensor);

    return;
}

#if TESTING_ort
#define CBASE_IMPLEMENT
#include "cbase.h"

static char *
ort_execution_provider_str(enum OrtExecutionProvider provider) {
    for (int32 i = 0; i < LENGTH(ort_execution_provider_infos); i += 1) {
        if (ort_execution_provider_infos[i].provider == provider) {
            return ort_execution_provider_infos[i].name;
        }
    }

    return "unknown";
}

static int32
ort_test_fail(char *name) {
    error2("ort test failed: %s\n", name);

    return 1;
}

static void
ort_test_empty_initializers(void) {
    OrtContext context;
    OrtModel model;
    OrtModelIoInfo info;
    OrtTensor tensor;

    ort_context_init_empty(&context);
    ort_model_init_empty(&model);
    ort_model_io_info_init_empty(&info);
    ort_tensor_init_empty(&tensor);

    ASSERT(context.api == NULL);
    ASSERT(context.environment == NULL);
    ASSERT(context.memory_info == NULL);
    ASSERT(context.allocator == NULL);
    ASSERT(context.session_config.execution_provider
           == ORT_EXECUTION_PROVIDER_AUTO);
    ASSERT(context.session_config.device_id == 0);
    ASSERT(!context.session_config.print_info);

    ASSERT(model.session == NULL);
    ASSERT(model.input_name == NULL);
    ASSERT(model.output_name == NULL);
    ASSERT(model.input_shape_len == 0);
    ASSERT(model.output_shape_len == 0);
    ASSERT(model.input_count == 0);
    ASSERT(model.output_count == 0);

    ASSERT(info.name == NULL);
    ASSERT(info.shape_len == 0);
    ASSERT(info.count == 0);

    ASSERT(tensor.value == NULL);
    ASSERT(tensor.data == NULL);
    ASSERT(tensor.data_len == 0);
    ASSERT(tensor.shape_len == 0);

    return;
}

static void
ort_test_model_io_info_copy(void) {
    OrtModel model;
    OrtModelIoInfo info;

    ort_model_init_empty(&model);
    model.input_name = "input";
    model.output_name = "output";
    model.input_count = 1;
    model.output_count = 1;
    model.input_shape_len = 2;
    model.output_shape_len = 2;
    model.input_shape[0] = 1;
    model.input_shape[1] = 3;
    model.output_shape[0] = 1;
    model.output_shape[1] = 3;

    if (!ort_model_input_info(&model, &info)) {
        fatal(ort_test_fail("copy input info"));
    }
    ASSERT(strequal(info.name, "input"));
    ASSERT(info.count == 1);
    ASSERT(info.shape_len == 2);
    ASSERT(info.shape[0] == 1);
    ASSERT(info.shape[1] == 3);

    if (!ort_model_output_info(&model, &info)) {
        fatal(ort_test_fail("copy output info"));
    }
    ASSERT(strequal(info.name, "output"));
    ASSERT(info.count == 1);
    ASSERT(info.shape_len == 2);
    ASSERT(info.shape[0] == 1);
    ASSERT(info.shape[1] == 3);

    return;
}


static void
ort_test_execution_provider_parse(void) {
    enum OrtExecutionProvider provider;

    if (!ort_execution_provider_parse("auto", &provider)) {
        fatal(ort_test_fail("parse auto provider"));
    }
    ASSERT(provider == ORT_EXECUTION_PROVIDER_AUTO);
    ASSERT(strequal(ort_execution_provider_str(provider), "auto"));

    if (!ort_execution_provider_parse("cpu", &provider)) {
        fatal(ort_test_fail("parse cpu provider"));
    }
    ASSERT(provider == ORT_EXECUTION_PROVIDER_CPU);
    ASSERT(strequal(ort_execution_provider_str(provider), "cpu"));

    if (!ort_execution_provider_parse("cuda", &provider)) {
        fatal(ort_test_fail("parse cuda provider"));
    }
    ASSERT(provider == ORT_EXECUTION_PROVIDER_CUDA);
    ASSERT(strequal(ort_execution_provider_str(provider), "cuda"));

    if (ort_execution_provider_parse("gpu", &provider)) {
        fatal(ort_test_fail("parse invalid provider"));
    }

    return;
}

static void
ort_test_tensor_shape_element_count(void) {
    int64 shape[3];
    int64 count;

    shape[0] = 2;
    shape[1] = 3;
    shape[2] = 4;
    if (!ort_tensor_shape_element_count(shape, 3, &count)) {
        fatal(ort_test_fail("valid tensor shape count"));
    }
    ASSERT(count == 24);

    shape[1] = 0;
    if (ort_tensor_shape_element_count(shape, 3, &count)) {
        fatal(ort_test_fail("zero tensor dimension accepted"));
    }
    ASSERT(count == 0);

    shape[0] = INT64_MAX;
    shape[1] = 2;
    if (ort_tensor_shape_element_count(shape, 2, &count)) {
        fatal(ort_test_fail("overflowing tensor shape accepted"));
    }
    ASSERT(count == 0);

    return;
}

static bool
ort_test_write_identity_model(char *path, char *temp_dir) {
    Command command;
    char script_path[PATH_MAX];
    char *script;
    int32 len;
    int32 exit_status;
    bool ok;

    if (!test_command_exists("python3")) {
        return false;
    }

    len = snprintf2(script_path,
                    SIZEOF(script_path),
                    "%s/write_identity_model.py",
                    temp_dir);
    if ((len <= 0) || (len >= SIZEOF(script_path))) {
        return false;
    }

    script =
        "import sys\n"
        "try:\n"
        "    import onnx\n"
        "    from onnx import TensorProto, helper\n"
        "except Exception:\n"
        "    sys.exit(77)\n"
        "x = helper.make_tensor_value_info(\"input\", "
            "TensorProto.FLOAT, [1, 3])\n"
        "y = helper.make_tensor_value_info(\"output\", "
            "TensorProto.FLOAT, [1, 3])\n"
        "node = helper.make_node(\"Identity\", [\"input\"], "
            "[\"output\"])\n"
        "graph = helper.make_graph([node], \"identity\", [x], [y])\n"
        "model = helper.make_model(\n"
        "    graph,\n"
        "    opset_imports=[helper.make_operatorsetid(\"\", 13)],\n"
        ")\n"
        "model.ir_version = 7\n"
        "onnx.save(model, sys.argv[1])\n";

    if (write_entire_file(script_path, script, strlen32(script)) < 0) {
        return false;
    }

    command = (Command){0};
    COMMAND_PUSH(&command, "python3", script_path, path);
    ok = command_run_sync(&command, &exit_status) == 0;
    command_free(&command);
    if (!ok || (exit_status != 0)) {
        return false;
    }

    return true;
}

static void
ort_test_optional_identity_model(void) {
    OrtContext context;
    OrtModel model;
    OrtModelIoInfo info;
    OrtTensor input;
    OrtTensor output;
    float data[3];
    int64 shape[2];
    char temp_dir[PATH_MAX];
    char model_path[PATH_MAX];
    int32 len;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ort_identity");
    len = snprintf2(model_path,
                    SIZEOF(model_path),
                    "%s/identity.onnx",
                    temp_dir);
    if ((len <= 0) || (len >= SIZEOF(model_path))) {
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("identity model path"));
    }

    if (!ort_test_write_identity_model(model_path, temp_dir)) {
        test_remove_tree(temp_dir);
        return;
    }

    ort_context_init_empty(&context);
    ort_model_init_empty(&model);
    ort_tensor_init_empty(&input);
    ort_tensor_init_empty(&output);

    if (!ort_context_init(&context)) {
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("context init"));
    }
    if (!ort_model_load(&context, &model, model_path)) {
        ort_context_destroy(&context);
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("identity model load"));
    }
    if (!ort_model_input_info(&model, &info)) {
        ort_model_destroy(&context, &model);
        ort_context_destroy(&context);
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("identity input info"));
    }
    ASSERT(strequal(info.name, "input"));
    ASSERT(info.shape_len == 2);
    ASSERT(info.shape[0] == 1);
    ASSERT(info.shape[1] == 3);

    if (!ort_model_output_info(&model, &info)) {
        ort_model_destroy(&context, &model);
        ort_context_destroy(&context);
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("identity output info"));
    }
    ASSERT(strequal(info.name, "output"));
    ASSERT(info.shape_len == 2);
    ASSERT(info.shape[0] == 1);
    ASSERT(info.shape[1] == 3);

    data[0] = 1.0f;
    data[1] = -2.0f;
    data[2] = 3.5f;
    shape[0] = 1;
    shape[1] = 3;
    if (!ort_tensor_create_f32(&context, &input, data, 3, shape, 2)) {
        ort_model_destroy(&context, &model);
        ort_context_destroy(&context);
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("identity input tensor"));
    }
    if (!ort_model_run_f32(&context, &model, &input, &output)) {
        ort_tensor_destroy(&context, &input);
        ort_model_destroy(&context, &model);
        ort_context_destroy(&context);
        test_remove_tree(temp_dir);
        fatal(ort_test_fail("identity run"));
    }

    ASSERT(output.data_len == 3);
    ASSERT(output.shape_len == 2);
    ASSERT(output.shape[0] == 1);
    ASSERT(output.shape[1] == 3);
    ASSERT(output.data[0] == data[0]);
    ASSERT(output.data[1] == data[1]);
    ASSERT(output.data[2] == data[2]);

    ort_tensor_destroy(&context, &output);
    ort_tensor_destroy(&context, &input);
    ort_model_destroy(&context, &model);
    ort_context_destroy(&context);
    test_remove_tree(temp_dir);

    return;
}

int32
main(void) {
    ort_test_empty_initializers();
    ort_test_model_io_info_copy();
    ort_test_execution_provider_parse();
    ort_test_tensor_shape_element_count();
    ort_test_optional_identity_model();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_ort */
