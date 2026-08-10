#include "cbase.h"
#include "lyricsync.h"
#include "vocals.h"
#include "ort.h"
#include "stft.h"
#include "progress.c"

#if !defined(TESTING_vocals)
#define TESTING_vocals 0
#endif

typedef struct VocalsExtractionConfig {
    char *model_path;
    char *ffmpeg_path;

    MdxConfig mdx_config;
    OrtSessionConfig ort_session_config;

    bool print_info;
} VocalsExtractionConfig;

static void
lrc_vocals_extract_result_init(LrcVocalsExtractResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_init(&result->path_header);

    return;
}

static void
vocals_extract_result_set(
    LrcVocalsExtractResult *result,
    enum LsError error,
    char *message,
    char *path
) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_set(&result->path_header, error, message, path);

    return;
}

static void
lrc_vocals_extract_request_init(LrcVocalsExtractRequest *request) {
    if (request == NULL) {
        return;
    }

    request->input_path = NULL;
    request->output_path = NULL;
    request->model_path = NULL;
    request->temp_dir = "/tmp";
    request->ffmpeg_path = "ffmpeg";
    request->container_format = LRC_AUDIO_FORMAT_DEFAULT;
    request->print_info = true;

    audio_io_format_init(&request->output_format);
    mdx_config_init(&request->mdx_config);
    request->ort_session_config.execution_provider =
        ORT_EXECUTION_PROVIDER_AUTO;
    request->ort_session_config.device_id = 0;
    request->ort_session_config.print_info = false;

    return;
}

static void
vocals_extraction_config_from_request(
    VocalsExtractionConfig *config,
    LrcVocalsExtractRequest *request
) {
    config->model_path = request->model_path;
    config->ffmpeg_path = request->ffmpeg_path;
    config->mdx_config = request->mdx_config;
    config->ort_session_config = request->ort_session_config;
    config->print_info = request->print_info;

    return;
}

static void
vocals_print_model_info(MdxModelInfo *info, MdxConfig *config) {
    error2(
        "MDX model: input=%s output=%s shape=[%d, %d, %d, %d]\n",
        info->input_name,
        info->output_name,
        info->batch_size,
        info->channel_count,
        info->dim_f,
        info->dim_t);
    error2(
        "MDX config: sample_rate=%d channels=%d dim_c=%d n_fft=%d "
        "hop=%d chunk_size=%d trim=%d gen_size=%d\n",
        config->sample_rate,
        config->channel_count,
        config->dim_c,
        config->n_fft,
        config->hop,
        config->chunk_size,
        config->trim,
        config->gen_size);

    return;
}

static bool
vocals_request_valid(
    LrcVocalsExtractRequest *request,
    LrcVocalsExtractResult *result
) {
    if (request == NULL) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT,
            "vocals extraction request is missing",
            NULL
        );
        return false;
    }
    if (path_missing(request->input_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MISSING_INPUT,
            "input audio path is missing",
            request->input_path
        );
        return false;
    }
    if (path_missing(request->output_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MISSING_OUTPUT,
            "output vocals path is missing",
            request->output_path
        );
        return false;
    }
    if (path_missing(request->model_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MISSING_MODEL,
            "vocals extraction model path is missing",
            request->model_path
        );
        return false;
    }
    if (path_missing(request->temp_dir)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MISSING_TEMP_DIR,
            "temporary directory path is missing",
            request->temp_dir
        );
        return false;
    }
    if (path_missing(request->ffmpeg_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MISSING_FFMPEG,
            "FFmpeg executable path is missing",
            request->ffmpeg_path
        );
        return false;
    }
    if (path_missing(request->container_format)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT,
            "output container format is missing",
            request->container_format
        );
        return false;
    }
    if (!lrc_audio_format_valid(request->container_format)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT,
            "output container format is invalid",
            request->container_format
        );
        return false;
    }
    if (!audio_io_format_valid(&request->output_format)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT,
            "output audio format is invalid",
            NULL
        );
        return false;
    }

    return true;
}

static bool
vocals_prepare_runtime(
    VocalsExtractionConfig *config,
    char *input_path,
    MdxConfig *mdx_config,
    StftPlan *stft_plan,
    MdxModelInfo *mdx_info,
    OrtContext *ort_context,
    OrtModel *ort_model,
    LrcVocalsExtractResult *result
) {
    *mdx_config = config->mdx_config;

    if (!audio_check_ffmpeg(config->ffmpeg_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_FFMPEG_UNAVAILABLE,
            "could not run ffmpeg",
            config->ffmpeg_path
        );
        return false;
    }

    if (!audio_can_decode_file(input_path, config->ffmpeg_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INPUT_DECODE_FAILED,
            "could not decode input audio with ffmpeg",
            input_path
        );
        return false;
    }

    if (!util_file_exists(config->model_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MODEL_OPEN_FAILED,
            "could not read ONNX model",
            config->model_path
        );
        return false;
    }

    if (!stft_plan_init(stft_plan, mdx_config->n_fft, mdx_config->hop)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_STFT_INIT_FAILED,
            "could not initialize STFT plan",
            NULL
        );
        return false;
    }

    if (!ort_context_init(ort_context)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_ORT_INIT_FAILED,
            "could not initialize ONNX Runtime",
            NULL
        );
        return false;
    }
    config->ort_session_config.print_info = config->print_info;
    ort_context_session_config_set(ort_context, &config->ort_session_config);

    if (!ort_model_load(ort_context, ort_model, config->model_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_ORT_MODEL_LOAD_FAILED,
            "could not load ONNX model",
            config->model_path
        );
        return false;
    }

    if (!mdx_model_inspect(mdx_info, mdx_config, ort_model)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_UNSUPPORTED_MDX_MODEL,
            "ONNX model is not a supported MDX-Net model",
            config->model_path
        );
        return false;
    }

    if (!mdx_config_prepare(mdx_config)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MDX_CONFIG_FAILED,
            "could not prepare MDX configuration",
            NULL
        );
        return false;
    }

    return true;
}

static bool
vocals_read_input_audio(
    AudioBuffer *input_audio,
    char *input_path,
    MdxConfig *mdx_config,
    char *ffmpeg_path,
    LrcVocalsExtractResult *result
) {
    AudioIoFormat input_format;

    audio_io_format_init(&input_format);
    input_format.sample_rate = mdx_config->sample_rate;
    input_format.channel_count = mdx_config->channel_count;

    if (!audio_read_file_format(input_audio,
                                input_path,
                                &input_format,
                                ffmpeg_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INPUT_READ_FAILED,
            "could not decode input audio",
            input_path
        );
        return false;
    }

    lrc_progress_end_line();
    error2(
        "decoded audio: sample_rate=%d channels=%d frames=%lld\n",
        input_audio->sample_rate,
        input_audio->channel_count,
        input_audio->frame_count);

    return true;
}

static bool
vocals_extract_audio(
    AudioBuffer *output_audio,
    char *input_path,
    VocalsExtractionConfig *config,
    LrcVocalsExtractResult *result
) {
    AudioBuffer input_audio;
    MdxConfig mdx_config;
    MdxModelInfo mdx_info;
    OrtContext ort_context;
    OrtModel ort_model;
    StftPlan stft_plan;
    LrcProgress progress;
    bool ok;

    if ((output_audio == NULL) || (input_path == NULL) || (config == NULL)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT,
            "vocals extraction received invalid arguments",
            NULL
        );
        return false;
    }

    ok = false;
    audio_buffer_init(&input_audio);
    stft_plan_init_empty(&stft_plan);
    mdx_config_init(&mdx_config);
    mdx_model_info_init_empty(&mdx_info);
    ort_context_init_empty(&ort_context);
    ort_model_init_empty(&ort_model);

    lrc_progress_init(&progress,
                      config->print_info,
                      "prepare vocals runtime",
                      1);
    lrc_progress_begin(&progress);
    if (!vocals_prepare_runtime(config,
                                input_path,
                                &mdx_config,
                                &stft_plan,
                                &mdx_info,
                                &ort_context,
                                &ort_model,
                                result)) {
        lrc_progress_cancel(&progress);
        goto cleanup;
    }
    lrc_progress_finish(&progress);

    if (config->print_info) {
        vocals_print_model_info(&mdx_info, &mdx_config);
    }

    lrc_progress_init(&progress, config->print_info, "decode input audio", 1);
    lrc_progress_begin(&progress);
    if (!vocals_read_input_audio(&input_audio,
                                 input_path,
                                 &mdx_config,
                                 config->ffmpeg_path,
                                 result)) {
        lrc_progress_cancel(&progress);
        goto cleanup;
    }
    lrc_progress_finish(&progress);

    if (!mdx_process_song_with_progress(&mdx_config,
                                         &stft_plan,
                                         &ort_context,
                                         &ort_model,
                                         &input_audio,
                                         output_audio,
                                         config->print_info)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_MDX_PROCESS_FAILED,
            "could not process audio through MDX model",
            NULL
        );
        goto cleanup;
    }

    ok = true;

cleanup:
    audio_buffer_destroy(&input_audio);
    ort_model_destroy(&ort_context, &ort_model);
    ort_context_destroy(&ort_context);
    stft_plan_destroy(&stft_plan);

    return ok;
}

static bool
lrc_extract_vocals(
    LrcVocalsExtractRequest *request,
    LrcVocalsExtractResult *result
) {
    AudioBuffer output_audio;
    VocalsExtractionConfig config;
    LrcProgress progress;
    bool ok;

    if (result) {
        lrc_vocals_extract_result_init(result);
    }
    if (!vocals_request_valid(request, result)) {
        return false;
    }

    ok = false;
    audio_buffer_init(&output_audio);
    vocals_extraction_config_from_request(&config, request);

    if (!vocals_extract_audio(&output_audio,
                              request->input_path,
                              &config,
                              result)) {
        goto cleanup;
    }

    lrc_progress_init(&progress, request->print_info, "write vocals file", 1);
    lrc_progress_begin(&progress);
    if (!audio_write_file_format(&output_audio,
                                 request->output_path,
                                 request->container_format,
                                 &request->output_format,
                                 request->ffmpeg_path)) {
        vocals_extract_result_set(
            result,
            LS_ERROR_VOCALS_EXTRACT_OUTPUT_WRITE_FAILED,
            "could not write output audio",
            request->output_path
        );
        lrc_progress_cancel(&progress);
        goto cleanup;
    }
    lrc_progress_finish(&progress);

    error2("wrote extracted vocals: %s\n", request->output_path);
    ok = true;

cleanup:
    audio_buffer_destroy(&output_audio);

    return ok;
}

#if TESTING_vocals
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "fftw.c"
#include "stft.c"
#include "audio.c"
#include "ort.c"
#include "mdx.c"

static int32
vocals_test_fail(char *name) {
    error2("vocals test failed: %s\n", name);

    return 1;
}

static void
vocals_test_request_defaults(void) {
    LrcVocalsExtractRequest request;
    LrcVocalsExtractResult result;

    lrc_vocals_extract_request_init(&request);
    lrc_vocals_extract_result_init(&result);

    ASSERT(request.input_path == NULL);
    ASSERT(request.output_path == NULL);
    ASSERT(request.model_path == NULL);
    ASSERT(strequal(request.temp_dir, "/tmp"));
    ASSERT(strequal(request.ffmpeg_path, "ffmpeg"));
    ASSERT(strequal(request.container_format, "wav"));
    ASSERT(request.print_info);
    ASSERT(request.ort_session_config.execution_provider
           == ORT_EXECUTION_PROVIDER_AUTO);
    ASSERT(request.ort_session_config.device_id == 0);
    ASSERT(!request.ort_session_config.print_info);
    ASSERT(request.output_format.sample_rate == 44100);
    ASSERT(request.output_format.channel_count == 2);
    ASSERT(request.mdx_config.sample_rate == 44100);
    ASSERT(request.mdx_config.channel_count == 2);
    ASSERT(result.path_header.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.path_header.header.message, "ok"));
    ASSERT(result.path_header.path == NULL);

    return;
}

static void
vocals_test_request_validation(void) {
    LrcVocalsExtractRequest request;
    LrcVocalsExtractResult result;

    lrc_vocals_extract_request_init(&request);
    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("missing input accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_MISSING_INPUT) {
        fatal(vocals_test_fail("missing input error"));
    }

    request.input_path = "input.wav";
    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("missing output accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_MISSING_OUTPUT) {
        fatal(vocals_test_fail("missing output error"));
    }

    request.output_path = "output.wav";
    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("missing model accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_MISSING_MODEL) {
        fatal(vocals_test_fail("missing model error"));
    }

    request.model_path = "model.onnx";
    request.temp_dir = NULL;
    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("missing temp dir accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_MISSING_TEMP_DIR) {
        fatal(vocals_test_fail("missing temp dir error"));
    }

    request.temp_dir = "/tmp";
    request.ffmpeg_path = NULL;
    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("missing ffmpeg accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_MISSING_FFMPEG) {
        fatal(vocals_test_fail("missing ffmpeg error"));
    }

    request.ffmpeg_path = "ffmpeg";
    request.output_format.channel_count = 3;
    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("invalid output format accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT) {
        fatal(vocals_test_fail("invalid output format error"));
    }

    return;
}

static void
vocals_test_missing_external_resources(void) {
    LrcVocalsExtractRequest request;
    LrcVocalsExtractResult result;

    lrc_vocals_extract_request_init(&request);
    request.input_path = "missing.wav";
    request.output_path = "out.wav";
    request.model_path = "missing.onnx";
    request.ffmpeg_path = "/definitely/missing/ffmpeg";
    request.print_info = false;

    if (lrc_extract_vocals(&request, &result)) {
        fatal(vocals_test_fail("missing ffmpeg extraction accepted"));
    }
    if (result.path_header.header.error != LS_ERROR_VOCALS_EXTRACT_FFMPEG_UNAVAILABLE) {
        fatal(vocals_test_fail("missing ffmpeg extraction error"));
    }

    return;
}

static void
vocals_test_optional_real_extraction(void) {
    LrcVocalsExtractRequest request;
    LrcVocalsExtractResult result;
    AudioBuffer decoded_output;
    AudioTestSineOptions sine_options;
    char input_path[PATH_MAX];
    char output_path[PATH_MAX];
    char temp_dir[PATH_MAX];
    char *model_path;

    model_path = getenv("UVR_TEST_MDX_MODEL");
    if (model_path == NULL) {
        return;
    }
    if (!test_command_exists("ffmpeg")) {
        return;
    }

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "vocals");
    test_join_path(input_path, SIZEOF(input_path), temp_dir, "input.wav");
    test_join_path(output_path, SIZEOF(output_path), temp_dir, "output.wav");

    audio_test_sine_options_init(&sine_options);
    sine_options.duration_seconds = 0.25;
    sine_options.format.sample_rate = 44100;
    sine_options.format.channel_count = 2;
    if (!audio_test_generate_sine_wav(input_path, &sine_options, "ffmpeg")) {
        test_remove_tree(temp_dir);
        fatal(vocals_test_fail("generated input audio"));
    }

    lrc_vocals_extract_request_init(&request);
    request.input_path = input_path;
    request.output_path = output_path;
    request.model_path = model_path;
    request.temp_dir = temp_dir;
    request.print_info = false;
    request.mdx_config.chunk_seconds = 1;
    request.mdx_config.margin_seconds = 0;

    if (!lrc_extract_vocals(&request, &result)) {
        error2("optional extraction failed: %s\n", result.path_header.header.message);
        test_remove_tree(temp_dir);
        fatal(vocals_test_fail("optional real extraction"));
    }

    audio_buffer_init(&decoded_output);
    if (!audio_read_file(&decoded_output, output_path, "ffmpeg")) {
        audio_buffer_destroy(&decoded_output);
        test_remove_tree(temp_dir);
        fatal(vocals_test_fail("optional output is decodable"));
    }
    if (decoded_output.frame_count <= 0) {
        audio_buffer_destroy(&decoded_output);
        test_remove_tree(temp_dir);
        fatal(vocals_test_fail("optional output frame count"));
    }

    audio_buffer_destroy(&decoded_output);
    test_remove_tree(temp_dir);

    return;
}

int32
main(void) {
    vocals_test_request_defaults();
    vocals_test_request_validation();
    vocals_test_missing_external_resources();
    vocals_test_optional_real_extraction();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_vocals */
