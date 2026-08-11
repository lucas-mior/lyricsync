#if !defined(TESTING_app)
#define TESTING_app 0
#endif

#define CBASE_API_DECL static
#define CBASE_API_DEF static
#define CBASE_IMPLEMENT
#include "cbase.h"

#define LRC_PIPELINE_ENABLE_GENERATE 1
#include "lyricsync.h"
#include "default_models.h"

#include "fftw.c"
#include "stft.c"
#include "audio.c"
#include "ort.c"
#include "mdx.c"
#include "vocals.c"
#include "lyrics.c"
#include "unicode_norm.c"
#include "ctc_text.c"
#include "ctc_assets.c"
#include "ctc_tokenizer.c"
#include "ctc_audio.c"
#include "ctc_model.c"
#include "ctc_inference.c"
#include "ctc_align.c"
#include "lrc.c"
#include "pipeline.c"

typedef struct MainOptions {
    LrcPipelineConfig config;

    char output_lrc_path[PATH_MAX];
    char lyrics_text_path[PATH_MAX];

    bool output_lrc_defaulted;
    bool onnx_provider_set;
    bool onnx_device_set;
    bool print_usage_on_error;
} MainOptions;

#define MAIN_FIELD(field) ((int64)offsetof(MainOptions, field))
#define MAIN_NO_FIELD 0

#define MAIN_TASK_VALUE_OPTIONS(X) \
    X(INPUT_SONG, "--input-song", "PATH", \
      "original song to process", NULL, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.song_path)) \
    X(INPUT_VOCALS, "--input-vocals", "PATH", \
      "already extracted vocals to use", NULL, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.existing_vocals_path)) \
    X(OUTPUT_VOCALS, "--output-vocals", "PATH", \
      "save extracted vocals at PATH", NULL, \
      MAIN_VALUE_STRING_INFER_VOCALS_FORMAT, \
      MAIN_FIELD(config.vocals_path)) \
    X(INPUT_LYRICS, "--input-lyrics", "PATH", \
      "plain-text lyrics to align", "derived from input prefix", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.lyrics_text_path)) \
    X(OUTPUT_LRC, "--output-lrc", "PATH", \
      "synced lyrics output path", NULL, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.output_lrc_path))

#define MAIN_MODEL_VALUE_OPTIONS(X) \
    X(MODEL_VOCAL, "--model-vocal", "PATH", \
      "MDX-Net ONNX model", LRC_DEFAULT_VOCALS_MODEL_PATH, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.vocals_model_path)) \
    X(MODEL_CTC, "--model-ctc", "PATH", \
      "CTC ONNX model", LRC_DEFAULT_CTC_MODEL_PATH, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.ctc_model_path)) \
    X(TOKENIZER, "--tokenizer", "PATH", \
      "CTC tokenizer tokens file", LRC_DEFAULT_CTC_TOKENIZER_PATH, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.tokenizer_path)) \
    X(ONNX_PROVIDER, "--onnx-provider", "KIND", \
      "ONNX provider (" ORT_EXECUTION_PROVIDER_NAMES ")", "auto", \
      MAIN_VALUE_ONNX_PROVIDER, \
      MAIN_FIELD(config.ort_session_config.execution_provider)) \
    X(ONNX_DEVICE, "--onnx-device", "N", \
      "CUDA device id", "0", MAIN_VALUE_ONNX_DEVICE, \
      MAIN_FIELD(config.ort_session_config.device_id))

#define MAIN_AUDIO_VALUE_OPTIONS(X) \
    X(FFMPEG, "--ffmpeg", "PATH", \
      "ffmpeg executable", "ffmpeg", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.ffmpeg_path)) \
    X(TEMP_DIR, "--temp-dir", "PATH", \
      "temporary directory", "/tmp", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.temp_dir)) \
    X(VOCALS_FORMAT, "--vocals-format", "KIND", \
      "extracted vocals container (" LRC_AUDIO_FORMAT_NAMES ")", \
      "inferred", MAIN_VALUE_VOCALS_FORMAT, \
      MAIN_FIELD(config.vocals_container_format)) \
    X(CHUNK_SECONDS, "--chunk-seconds", "N", \
      "MDX chunk size in seconds", "30", \
      MAIN_VALUE_POSITIVE_INT32, \
      MAIN_FIELD(config.mdx_config.chunk_seconds)) \
    X(MARGIN_SECONDS, "--margin-seconds", "N", \
      "MDX chunk margin in seconds", "3", \
      MAIN_VALUE_INT32, MAIN_FIELD(config.mdx_config.margin_seconds)) \
    X(COMPENSATE, "--compensate", "X", \
      "output gain", "1.035", \
      MAIN_VALUE_FLOAT, MAIN_FIELD(config.mdx_config.compensate)) \
    X(N_FFT, "--n-fft", "N", \
      "STFT size", "6144", \
      MAIN_VALUE_POSITIVE_INT32, MAIN_FIELD(config.mdx_config.n_fft)) \
    X(HOP, "--hop", "N", \
      "STFT hop", "1024", \
      MAIN_VALUE_POSITIVE_INT32, MAIN_FIELD(config.mdx_config.hop)) \
    X(DIM_F, "--dim-f", "N", \
      "override model frequency bins", NULL, \
      MAIN_VALUE_POSITIVE_INT32, MAIN_FIELD(config.mdx_config.dim_f)) \
    X(DIM_T, "--dim-t", "N", \
      "override model time frames", NULL, \
      MAIN_VALUE_POSITIVE_INT32, MAIN_FIELD(config.mdx_config.dim_t)) \
    X(MODEL_OUTPUT, "--model-output", "KIND", \
      "model output stem (" MDX_MODEL_OUTPUT_NAMES ")", "vocals", \
      MAIN_VALUE_MODEL_OUTPUT, \
      MAIN_FIELD(config.mdx_config.model_output)) \
    X(CLIP_MODE, "--clip-mode", "KIND", \
      "final clipping policy (" MDX_CLIP_MODE_NAMES ")", "clamp", \
      MAIN_VALUE_CLIP_MODE, MAIN_FIELD(config.mdx_config.clip_mode))

#define MAIN_LYRICS_VALUE_OPTIONS(X) \
    X(CTC_DEBUG_DUMP, "--ctc-debug-dump", "PATH", \
      "write CTC parity debug dump", NULL, \
      MAIN_VALUE_STRING, MAIN_FIELD(config.ctc_debug_dump_path)) \
    X(SPLIT_SIZE, "--split-size", "KIND", \
      "lyrics split size (current|word|char|sentence)", "word", \
      MAIN_VALUE_SPLIT_SIZE, MAIN_NO_FIELD) \
    X(STAR_FREQUENCY, "--star-frequency", "KIND", \
      "star-token placement (none|edges|segment)", "edges", \
      MAIN_VALUE_STAR_FREQUENCY, MAIN_NO_FIELD) \
    X(ROMANIZATION, "--romanization", "KIND", \
      "romanization backend (off|icu)", "icu", \
      MAIN_VALUE_ROMANIZATION, MAIN_NO_FIELD) \
    X(LANGUAGE, "--language", "CODE", \
      "3-letter language code", "eng", \
      MAIN_VALUE_LANGUAGE, MAIN_NO_FIELD) \
    X(EMISSIONS, "--emissions", "KIND", \
      "model emission values (" LRC_CTC_EMISSION_VALUES_KIND_NAMES ")", \
      "logits", MAIN_VALUE_EMISSIONS, \
      MAIN_FIELD(config.ctc_emission_values_kind))

#define MAIN_VALUE_OPTIONS(X) \
    MAIN_TASK_VALUE_OPTIONS(X) \
    MAIN_MODEL_VALUE_OPTIONS(X) \
    MAIN_AUDIO_VALUE_OPTIONS(X) \
    MAIN_LYRICS_VALUE_OPTIONS(X)

#define MAIN_VALUE_OPTION_ALIASES(X) \
    X(OUTPUT_VOCALS, "--vocals-output", \
      MAIN_VALUE_STRING_INFER_VOCALS_FORMAT, \
      MAIN_FIELD(config.vocals_path)) \
    X(INPUT_LYRICS, "--lyrics", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.lyrics_text_path)) \
    X(INPUT_LYRICS, "-l", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.lyrics_text_path)) \
    X(OUTPUT_LRC, "--output", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.output_lrc_path)) \
    X(OUTPUT_LRC, "-o", \
      MAIN_VALUE_STRING, MAIN_FIELD(config.output_lrc_path)) \
    X(VOCALS_FORMAT, "--format", \
      MAIN_VALUE_VOCALS_FORMAT, \
      MAIN_FIELD(config.vocals_container_format))

#define MAIN_GENERAL_FLAG_OPTIONS(X) \
    X(HELP, "--help", "show this help", \
      MAIN_FLAG_HELP, MAIN_NO_FIELD)

#define MAIN_AUDIO_FLAG_OPTIONS(X) \
    X(DENOISE, "--denoise", "run denoising inference mode", \
      MAIN_FLAG_SET_TRUE, MAIN_FIELD(config.mdx_config.denoise))

#define MAIN_LYRICS_FLAG_OPTIONS(X) \
    X(KEEP_TEMP_FILES, "--keep-temp-files", \
      "keep generated temporary files", \
      MAIN_FLAG_SET_TRUE, MAIN_FIELD(config.keep_temp_files)) \
    X(ROMANIZE, "--romanize", "select ICU romanization", \
      MAIN_FLAG_ROMANIZE, MAIN_NO_FIELD)

#define MAIN_FLAG_OPTIONS(X) \
    MAIN_GENERAL_FLAG_OPTIONS(X) \
    MAIN_AUDIO_FLAG_OPTIONS(X) \
    MAIN_LYRICS_FLAG_OPTIONS(X)

#define MAIN_FLAG_OPTION_ALIASES(X) \
    X(HELP, "-h", MAIN_FLAG_HELP, MAIN_NO_FIELD)

enum MainValueOptionKind {
#define MAIN_VALUE_OPTION_ENUM(id, name, metavar, description, default_text, \
                               action, offset) \
    MAIN_VALUE_OPTION_##id,
    MAIN_VALUE_OPTIONS(MAIN_VALUE_OPTION_ENUM)
#undef MAIN_VALUE_OPTION_ENUM
    MAIN_VALUE_OPTION_LAST,
};

enum MainFlagOptionKind {
#define MAIN_FLAG_OPTION_ENUM(id, name, description, action, offset) \
    MAIN_FLAG_OPTION_##id,
    MAIN_FLAG_OPTIONS(MAIN_FLAG_OPTION_ENUM)
#undef MAIN_FLAG_OPTION_ENUM
    MAIN_FLAG_OPTION_LAST,
};

enum MainValueOptionAction {
    MAIN_VALUE_STRING,
    MAIN_VALUE_STRING_INFER_VOCALS_FORMAT,
    MAIN_VALUE_INT32,
    MAIN_VALUE_POSITIVE_INT32,
    MAIN_VALUE_FLOAT,
    MAIN_VALUE_ONNX_PROVIDER,
    MAIN_VALUE_ONNX_DEVICE,
    MAIN_VALUE_VOCALS_FORMAT,
    MAIN_VALUE_MODEL_OUTPUT,
    MAIN_VALUE_CLIP_MODE,
    MAIN_VALUE_SPLIT_SIZE,
    MAIN_VALUE_STAR_FREQUENCY,
    MAIN_VALUE_ROMANIZATION,
    MAIN_VALUE_LANGUAGE,
    MAIN_VALUE_EMISSIONS,
};

enum MainFlagOptionAction {
    MAIN_FLAG_HELP,
    MAIN_FLAG_SET_TRUE,
    MAIN_FLAG_ROMANIZE,
};

typedef struct MainValueOption {
    char *name;
    enum MainValueOptionKind kind;
    enum MainValueOptionAction action;
    int64 offset;
} MainValueOption;

typedef struct MainFlagOption {
    char *name;
    enum MainFlagOptionKind kind;
    enum MainFlagOptionAction action;
    int64 offset;
} MainFlagOption;

typedef struct MainEnumValue {
    char *name;
    int32 value;
} MainEnumValue;

static void
main_print_value_option_usage(
    char *name,
    char *metavar,
    char *description,
    char *default_text
) {
    char option[64];
    int32 len;

    len = snprintf2(option, SIZEOF(option), "%s %s", name, metavar);
    if ((len <= 0) || (len >= SIZEOF(option))) {
        option[0] = '\0';
    }

    error2("    %-28s %s", option, description);
    if (default_text != NULL) {
        error2(" [%s]", default_text);
    }
    error2("\n");

    return;
}

static void
main_print_flag_option_usage(char *name, char *description) {
    error2("    %-28s %s\n", name, description);

    return;
}

static noreturn void
main_print_usage(FILE *stream) {
    error2(
        "usage: %s (--input-song SONG | --input-vocals VOCALS) [options]\n",
        program
    );
    error2("\n");
    error2("general options:\n");
#define MAIN_PRINT_FLAG_USAGE(id, name, description, action, offset) \
    main_print_flag_option_usage(name, description);
    MAIN_GENERAL_FLAG_OPTIONS(MAIN_PRINT_FLAG_USAGE)
#undef MAIN_PRINT_FLAG_USAGE

    error2("\n");
    error2("task selection:\n");
#define MAIN_PRINT_VALUE_USAGE(id, name, metavar, description, default_text, \
                               action, offset) \
    main_print_value_option_usage(name, metavar, description, default_text);
    MAIN_TASK_VALUE_OPTIONS(MAIN_PRINT_VALUE_USAGE)
#undef MAIN_PRINT_VALUE_USAGE

    error2("\n");
    error2("model options:\n");
#define MAIN_PRINT_VALUE_USAGE(id, name, metavar, description, default_text, \
                               action, offset) \
    main_print_value_option_usage(name, metavar, description, default_text);
    MAIN_MODEL_VALUE_OPTIONS(MAIN_PRINT_VALUE_USAGE)
#undef MAIN_PRINT_VALUE_USAGE

    error2("\n");
    error2("audio options:\n");
#define MAIN_PRINT_VALUE_USAGE(id, name, metavar, description, default_text, \
                               action, offset) \
    main_print_value_option_usage(name, metavar, description, default_text);
    MAIN_AUDIO_VALUE_OPTIONS(MAIN_PRINT_VALUE_USAGE)
#undef MAIN_PRINT_VALUE_USAGE
#define MAIN_PRINT_FLAG_USAGE(id, name, description, action, offset) \
    main_print_flag_option_usage(name, description);
    MAIN_AUDIO_FLAG_OPTIONS(MAIN_PRINT_FLAG_USAGE)
#undef MAIN_PRINT_FLAG_USAGE

    error2("\n");
    error2("lyrics options:\n");
#define MAIN_PRINT_VALUE_USAGE(id, name, metavar, description, default_text, \
                               action, offset) \
    main_print_value_option_usage(name, metavar, description, default_text);
    MAIN_LYRICS_VALUE_OPTIONS(MAIN_PRINT_VALUE_USAGE)
#undef MAIN_PRINT_VALUE_USAGE
#define MAIN_PRINT_FLAG_USAGE(id, name, description, action, offset) \
    main_print_flag_option_usage(name, description);
    MAIN_LYRICS_FLAG_OPTIONS(MAIN_PRINT_FLAG_USAGE)
#undef MAIN_PRINT_FLAG_USAGE

    if (stream == stdout) {
        exit(EXIT_SUCCESS);
    }

    exit(EXIT_FAILURE);
}

static bool
main_long_option_value(char *arg, char *name, char **value) {
    int32 i;

    for (i = 0; name[i] != '\0'; i += 1) {
        if (arg[i] != name[i]) {
            return false;
        }
    }
    if (arg[i] != '=') {
        return false;
    }

    *value = arg + i + 1;

    return true;
}

static bool
main_parse_int32(char *value, char *name, int32 *out) {
    char *end;
    int64 result;

    errno = 0;
    end = NULL;
    result = strtoll(value, &end, 10);
    if ((errno != 0) || (end == value) || (*end != '\0')) {
        error2("%s must be an integer: %s\n", name, value);
        return false;
    }
    if ((result < 0) || (result > INT32_MAX)) {
        error2("%s is outside the supported range: %s\n", name, value);
        return false;
    }

    *out = (int32)result;

    return true;
}

static bool
main_parse_positive_int32(char *value, char *name, int32 *out) {
    if (!main_parse_int32(value, name, out)) {
        return false;
    }
    if (*out <= 0) {
        error2("%s must be greater than zero: %s\n", name, value);
        return false;
    }

    return true;
}

static bool
main_parse_float(char *value, char *name, float *out) {
    char *end;
    float result;

    errno = 0;
    end = NULL;
    result = strtof(value, &end);
    if ((errno != 0) || (end == value) || (*end != '\0')) {
        error2("%s must be a number: %s\n", name, value);
        return false;
    }
    if (result < 0.0f) {
        error2("%s must not be negative: %s\n", name, value);
        return false;
    }

    *out = result;

    return true;
}

#define MAIN_ENUM_VALUE(e, name) {name, (int32)e},
static MainEnumValue main_model_output_values[] = {
    MDX_MODEL_OUTPUT_VALUES(MAIN_ENUM_VALUE)
};
static MainEnumValue main_clip_mode_values[] = {
    MDX_CLIP_MODE_VALUES(MAIN_ENUM_VALUE)
};
static MainEnumValue main_emission_values_kind_values[] = {
    LRC_CTC_EMISSION_VALUES_KIND_VALUES(MAIN_ENUM_VALUE)
};
#undef MAIN_ENUM_VALUE

static bool
main_parse_enum_value(
    char *option,
    char *value,
    MainEnumValue *values,
    int32 value_count,
    int32 *out
) {
    if ((value == NULL) || (out == NULL)) {
        return false;
    }

    for (int32 i = 0; i < value_count; i += 1) {
        if (strequal(value, values[i].name)) {
            *out = values[i].value;
            return true;
        }
    }

    error2("%s must be ", option);
    for (int32 i = 0; i < value_count; i += 1) {
        if (i > 0) {
            error2(", ");
        }
        error2("%s", values[i].name);
    }
    error2("\n");

    return false;
}

static bool
main_parse_vocals_format(LrcPipelineConfig *config, char *option, char *value) {
    LrcAudioFormatInfo *format_info;

    format_info = lrc_audio_format_info_from_name(value);
    if (format_info) {
        config->vocals_container_format = format_info->name;
        return true;
    }

    error2("%s must be %s\n", option, LRC_AUDIO_FORMAT_NAMES);

    return false;
}

static void
main_infer_vocals_format(LrcPipelineConfig *config, char *path) {
    LrcAudioFormatInfo *format_info;
    char *extension;
    char *last_slash;
    int32 path_len;

    if (path_missing(path)) {
        return;
    }

    path_len = strlen32(path);
    extension = NULL;
    last_slash = NULL;
    for (int32 i = 0; i < path_len; i += 1) {
        if (path[i] == '/') {
            last_slash = path + i;
            extension = NULL;
            continue;
        }
        if (path[i] == '.') {
            extension = path + i + 1;
        }
    }

    if ((extension == NULL) || (extension[0] == '\0')) {
        return;
    }
    if (last_slash && (extension <= last_slash + 1)) {
        return;
    }
    format_info = lrc_audio_format_info_from_extension(extension);
    if (format_info) {
        config->vocals_container_format = format_info->name;
    }

    return;
}

static MainValueOption main_value_options[] = {
#define MAIN_VALUE_OPTION_ENTRY(id, name, metavar, description, default_text, \
                                action, offset) \
    {name, MAIN_VALUE_OPTION_##id, action, offset},
    MAIN_VALUE_OPTIONS(MAIN_VALUE_OPTION_ENTRY)
#undef MAIN_VALUE_OPTION_ENTRY
#define MAIN_VALUE_OPTION_ALIAS_ENTRY(id, name, action, offset) \
    {name, MAIN_VALUE_OPTION_##id, action, offset},
    MAIN_VALUE_OPTION_ALIASES(MAIN_VALUE_OPTION_ALIAS_ENTRY)
#undef MAIN_VALUE_OPTION_ALIAS_ENTRY
};

static MainFlagOption main_flag_options[] = {
#define MAIN_FLAG_OPTION_ENTRY(id, name, description, action, offset) \
    {name, MAIN_FLAG_OPTION_##id, action, offset},
    MAIN_FLAG_OPTIONS(MAIN_FLAG_OPTION_ENTRY)
#undef MAIN_FLAG_OPTION_ENTRY
#define MAIN_FLAG_OPTION_ALIAS_ENTRY(id, name, action, offset) \
    {name, MAIN_FLAG_OPTION_##id, action, offset},
    MAIN_FLAG_OPTION_ALIASES(MAIN_FLAG_OPTION_ALIAS_ENTRY)
#undef MAIN_FLAG_OPTION_ALIAS_ENTRY
};

static MainValueOption *
main_find_value_option(char *option) {
    for (int32 i = 0; i < LENGTH(main_value_options); i += 1) {
        if (strequal(option, main_value_options[i].name)) {
            return main_value_options + i;
        }
    }

    return NULL;
}

static MainFlagOption *
main_find_flag_option(char *option) {
    for (int32 i = 0; i < LENGTH(main_flag_options); i += 1) {
        if (strequal(option, main_flag_options[i].name)) {
            return main_flag_options + i;
        }
    }

    return NULL;
}

static char *
main_value_option_name(enum MainValueOptionKind kind) {
    for (int32 i = 0; i < LENGTH(main_value_options); i += 1) {
        if (main_value_options[i].kind == kind) {
            return main_value_options[i].name;
        }
    }

    return "unknown option";
}

static bool
main_apply_value_option(
    MainOptions *options,
    MainValueOption *option,
    char *value
) {
    enum OrtExecutionProvider provider;
    void *field = (char *)options + option->offset;
    int32 parsed;
    float parsed_float;

    switch (option->action) {
    case MAIN_VALUE_STRING:
        *(char **)field = value;
        return true;
    case MAIN_VALUE_STRING_INFER_VOCALS_FORMAT:
        *(char **)field = value;
        main_infer_vocals_format(&options->config, value);
        return true;
    case MAIN_VALUE_INT32:
        if (!main_parse_int32(value, option->name, &parsed)) {
            return false;
        }
        *(int32 *)field = parsed;
        return true;
    case MAIN_VALUE_POSITIVE_INT32:
        if (!main_parse_positive_int32(value, option->name, &parsed)) {
            return false;
        }
        *(int32 *)field = parsed;
        return true;
    case MAIN_VALUE_FLOAT:
        if (!main_parse_float(value, option->name, &parsed_float)) {
            return false;
        }
        *(float *)field = parsed_float;
        return true;
    case MAIN_VALUE_ONNX_PROVIDER:
        if (!ort_execution_provider_parse(value, &provider)) {
            error2("%s must be %s\n", option->name,
                   ORT_EXECUTION_PROVIDER_NAMES);
            return false;
        }
        options->onnx_provider_set = true;
        *(enum OrtExecutionProvider *)field = provider;
        return true;
    case MAIN_VALUE_ONNX_DEVICE:
        if (!main_parse_int32(value, option->name, &parsed)) {
            return false;
        }
        options->onnx_device_set = true;
        *(int32 *)field = parsed;
        return true;
    case MAIN_VALUE_VOCALS_FORMAT:
        return main_parse_vocals_format(&options->config,
                                        option->name,
                                        value);
    case MAIN_VALUE_MODEL_OUTPUT:
        if (!main_parse_enum_value(option->name,
                                   value,
                                   main_model_output_values,
                                   LENGTH(main_model_output_values),
                                   &parsed)) {
            return false;
        }
        *(enum MdxModelOutput *)field = (enum MdxModelOutput)parsed;
        return true;
    case MAIN_VALUE_CLIP_MODE:
        if (!main_parse_enum_value(option->name,
                                   value,
                                   main_clip_mode_values,
                                   LENGTH(main_clip_mode_values),
                                   &parsed)) {
            return false;
        }
        *(enum MdxClipMode *)field = (enum MdxClipMode)parsed;
        return true;
    case MAIN_VALUE_SPLIT_SIZE:
        return lrc_pipeline_parse_preprocess_split_size(&options->config,
                                                        value);
    case MAIN_VALUE_STAR_FREQUENCY:
        return lrc_pipeline_parse_preprocess_star_frequency(&options->config,
                                                            value);
    case MAIN_VALUE_ROMANIZATION:
        return lrc_pipeline_parse_preprocess_romanization(&options->config,
                                                          value);
    case MAIN_VALUE_LANGUAGE:
        return lrc_pipeline_parse_preprocess_language(&options->config, value);
    case MAIN_VALUE_EMISSIONS:
        if (!main_parse_enum_value(option->name,
                                   value,
                                   main_emission_values_kind_values,
                                   LENGTH(main_emission_values_kind_values),
                                   &parsed)) {
            return false;
        }
        *(enum LrcCtcEmissionValuesKind *)field =
            (enum LrcCtcEmissionValuesKind)parsed;
        return true;
    default:
        error2("internal error: unsupported value option action\n");
        return false;
    }
}

static bool
main_parse_value_option(MainOptions *options, char *option, char *value) {
    MainValueOption *value_option;

    value_option = main_find_value_option(option);
    if (value_option == NULL) {
        error2("unknown option: %s\n", option);
        return false;
    }

    return main_apply_value_option(options, value_option, value);
}

static bool
main_value_option_allows_equals(char *option) {
    if (option[0] != '-') {
        return false;
    }
    if (option[1] != '-') {
        return false;
    }

    return true;
}

static int32
main_parse_long_value(MainOptions *options, char *arg) {
    MainValueOption *value_option;
    char *value;

    for (int32 i = 0; i < LENGTH(main_value_options); i += 1) {
        value_option = main_value_options + i;
        if (!main_value_option_allows_equals(value_option->name)) {
            continue;
        }

        value = NULL;
        if (!main_long_option_value(arg, value_option->name, &value)) {
            continue;
        }
        if (!main_apply_value_option(options, value_option, value)) {
            return -1;
        }

        return 1;
    }

    return 0;
}

static bool
main_option_needs_value(char *option) {
    if (main_find_value_option(option)) {
        return true;
    }

    return false;
}

static bool
main_apply_flag_option(MainOptions *options, MainFlagOption *option) {
    void *field = (char *)options + option->offset;

    switch (option->action) {
    case MAIN_FLAG_HELP:
        main_print_usage(stdout);
    case MAIN_FLAG_SET_TRUE:
        *(bool *)field = true;
        return true;
    case MAIN_FLAG_ROMANIZE:
        lrc_pipeline_enable_preprocess_romanization(&options->config);
        return true;
    default:
        error2("internal error: unsupported flag option action\n");
        return false;
    }
}

static int32
main_parse_flag_option(MainOptions *options, char *option) {
    MainFlagOption *flag_option;

    flag_option = main_find_flag_option(option);
    if (flag_option == NULL) {
        return 0;
    }
    if (!main_apply_flag_option(options, flag_option)) {
        return -1;
    }

    return 1;
}

static void
main_apply_onnx_provider_env(LrcPipelineConfig *config) {
    enum OrtExecutionProvider provider;
    char *env;

    env = getenv("LRC_ONNX_PROVIDER");
    if ((env == NULL) || (env[0] == '\0')) {
        return;
    }
    if (!ort_execution_provider_parse(env, &provider)) {
        error2("warning: LRC_ONNX_PROVIDER must be %s\n",
               ORT_EXECUTION_PROVIDER_NAMES);
        return;
    }
    if (config->ort_session_config.execution_provider
        == ORT_EXECUTION_PROVIDER_AUTO) {
        config->ort_session_config.execution_provider = provider;
    }

    return;
}

static void
main_apply_onnx_device_env(LrcPipelineConfig *config) {
    char *env;
    int32 device_id;

    env = getenv("LRC_ONNX_DEVICE");
    if ((env == NULL) || (env[0] == '\0')) {
        return;
    }
    if (!main_parse_int32(env, "LRC_ONNX_DEVICE", &device_id)) {
        error2("warning: ignoring invalid LRC_ONNX_DEVICE\n");
        return;
    }
    if (config->ort_session_config.device_id == 0) {
        config->ort_session_config.device_id = device_id;
    }

    return;
}

static char *
main_first_existing_model_path(char **paths, int32 path_count) {
    if ((paths == NULL) || (path_count <= 0)) {
        return NULL;
    }

    for (int32 i = 0; i < path_count; i += 1) {
        if (!path_missing(paths[i]) && util_file_exists(paths[i])) {
            return paths[i];
        }
    }

    return paths[0];
}

static char *
main_default_vocals_model_path(void) {
    char *paths[] = {
        LRC_DEFAULT_VOCALS_MODEL_PATH,
        LRC_SYSTEM_VOCALS_MODEL_PATH,
        LRC_LOCAL_SYSTEM_VOCALS_MODEL_PATH,
    };

    return main_first_existing_model_path(paths, LENGTH(paths));
}

static char *
main_default_ctc_model_path(void) {
    char *paths[] = {
        LRC_DEFAULT_CTC_MODEL_PATH,
        LRC_SYSTEM_CTC_MODEL_PATH,
        LRC_LOCAL_SYSTEM_CTC_MODEL_PATH,
    };

    return main_first_existing_model_path(paths, LENGTH(paths));
}

static char *
main_default_ctc_tokenizer_path(void) {
    char *paths[] = {
        LRC_DEFAULT_CTC_TOKENIZER_PATH,
        LRC_SYSTEM_CTC_TOKENIZER_PATH,
        LRC_LOCAL_SYSTEM_CTC_TOKENIZER_PATH,
    };

    return main_first_existing_model_path(paths, LENGTH(paths));
}

static void
main_apply_model_defaults(LrcPipelineConfig *config) {
    char *env;

    env = getenv("LRC_VOCALS_MODEL");
    if ((config->vocals_model_path == NULL)
        && env
        && (env[0] != '\0')) {
        config->vocals_model_path = env;
    }
    if (config->vocals_model_path == NULL) {
        config->vocals_model_path = main_default_vocals_model_path();
    }

    env = getenv("LRC_CTC_MODEL");
    if ((config->ctc_model_path == NULL)
        && env
        && (env[0] != '\0')) {
        config->ctc_model_path = env;
    }
    if (config->ctc_model_path == NULL) {
        config->ctc_model_path = main_default_ctc_model_path();
    }

    env = getenv("LRC_CTC_TOKENIZER");
    if ((config->tokenizer_path == NULL)
        && env
        && (env[0] != '\0')) {
        config->tokenizer_path = env;
    }
    if (config->tokenizer_path == NULL) {
        config->tokenizer_path = main_default_ctc_tokenizer_path();
    }

    main_apply_onnx_provider_env(config);
    main_apply_onnx_device_env(config);

    return;
}

static bool
main_input_prefix_path(
    MainOptions *options,
    char *output_path,
    int64 output_size,
    char *extension,
    char *description
) {
    LrcPipelineConfig *config = &options->config;
    char *input_path = config->song_path;
    int32 input_len;
    int32 slash_index;
    int32 dot_index;
    int32 prefix_len;
    int32 len;

    if (path_missing(input_path)) {
        input_path = config->existing_vocals_path;
    }
    if (path_missing(input_path)) {
        error2("could not derive %s path without an input path\n",
               description);
        return false;
    }

    input_len = strlen32(input_path);
    slash_index = -1;
    dot_index = -1;
    for (int32 i = 0; i < input_len; i += 1) {
        if (input_path[i] == '/') {
            slash_index = i;
            dot_index = -1;
            continue;
        }
        if (input_path[i] == '.') {
            dot_index = i;
        }
    }

    prefix_len = input_len;
    if (dot_index > slash_index + 1) {
        prefix_len = dot_index;
    }
    len = snprintf2(output_path,
                    output_size,
                    "%.*s.%s",
                    prefix_len,
                    input_path,
                    extension);
    if ((len <= 0) || (len >= output_size)) {
        error2("default %s path is too long: %s\n",
               description,
               input_path);
        return false;
    }

    return true;
}

static bool
main_default_lyrics_text_path(MainOptions *options) {
    LrcPipelineConfig *config = &options->config;

    if (!path_missing(config->lyrics_text_path)) {
        return true;
    }
    if (!main_input_prefix_path(options,
                                options->lyrics_text_path,
                                SIZEOF(options->lyrics_text_path),
                                "txt",
                                "lyrics text")) {
        return false;
    }
    if (!util_file_exists(options->lyrics_text_path)) {
        error2("missing --input-lyrics and default lyrics file does not "
               "exist: %s\n",
               options->lyrics_text_path);
        return false;
    }

    config->lyrics_text_path = options->lyrics_text_path;

    return true;
}

static bool
main_default_lrc_path(MainOptions *options) {
    LrcPipelineConfig *config = &options->config;

    if (!main_input_prefix_path(options,
                                options->output_lrc_path,
                                SIZEOF(options->output_lrc_path),
                                "lrc",
                                "LRC output")) {
        return false;
    }

    config->output_lrc_path = options->output_lrc_path;
    options->output_lrc_defaulted = true;

    return true;
}

static void
main_mark_usage_error(MainOptions *options) {
    options->print_usage_on_error = true;

    return;
}

static bool
main_validate_options(MainOptions *options) {
    LrcPipelineConfig *config = &options->config;
    bool has_song = !path_missing(config->song_path);
    bool has_vocals = !path_missing(config->existing_vocals_path);
    bool has_lyrics;

    if (has_song && has_vocals) {
        error2("%s and %s cannot both be passed\n",
               main_value_option_name(MAIN_VALUE_OPTION_INPUT_SONG),
               main_value_option_name(MAIN_VALUE_OPTION_INPUT_VOCALS));
        main_mark_usage_error(options);
        return false;
    }
    if (!has_song && !has_vocals) {
        error2("missing required option: %s or %s\n",
               main_value_option_name(MAIN_VALUE_OPTION_INPUT_SONG),
               main_value_option_name(MAIN_VALUE_OPTION_INPUT_VOCALS));
        main_mark_usage_error(options);
        return false;
    }
    if (!main_default_lyrics_text_path(options)) {
        return false;
    }

    has_lyrics = !path_missing(config->lyrics_text_path);
    if (has_lyrics && path_missing(config->output_lrc_path)) {
        if (!main_default_lrc_path(options)) {
            return false;
        }
    }
    if (options->output_lrc_defaulted
        && util_file_exists(config->output_lrc_path)) {
        error2("default LRC output already exists: %s\n",
               config->output_lrc_path);
        return false;
    }
    if (has_lyrics && path_missing(config->output_lrc_path)) {
        error2("missing LRC output path: %s\n",
               main_value_option_name(MAIN_VALUE_OPTION_OUTPUT_LRC));
        main_mark_usage_error(options);
        return false;
    }
    if (has_song && path_missing(config->vocals_model_path)) {
        error2("missing required option: %s\n",
               main_value_option_name(MAIN_VALUE_OPTION_MODEL_VOCAL));
        main_mark_usage_error(options);
        return false;
    }
    if (has_lyrics && path_missing(config->ctc_model_path)) {
        error2("missing required option: %s\n",
               main_value_option_name(MAIN_VALUE_OPTION_MODEL_CTC));
        main_mark_usage_error(options);
        return false;
    }
    if (has_lyrics && path_missing(config->tokenizer_path)) {
        error2("missing required option: %s\n",
               main_value_option_name(MAIN_VALUE_OPTION_TOKENIZER));
        main_mark_usage_error(options);
        return false;
    }
    if (config->mdx_config.margin_seconds < 0) {
        error2("%s must not be negative\n",
               main_value_option_name(MAIN_VALUE_OPTION_MARGIN_SECONDS));
        main_mark_usage_error(options);
        return false;
    }
    if (config->ort_session_config.device_id < 0) {
        error2("%s must not be negative\n",
               main_value_option_name(MAIN_VALUE_OPTION_ONNX_DEVICE));
        main_mark_usage_error(options);
        return false;
    }

    return true;
}

static bool
main_parse_args(MainOptions *options, int32 argc, char **argv) {
    enum OrtExecutionProvider provider;
    int32 device_id;
    int32 parsed;

    if ((options == NULL) || (argc < 0)) {
        return false;
    }
    if ((argc > 0) && (argv == NULL)) {
        error2("missing command line argument vector\n");
        main_mark_usage_error(options);
        return false;
    }

    for (int32 i = 1; i < argc; i += 1) {
        if (argv[i] == NULL) {
            error2("missing command line argument %d\n", i);
            main_mark_usage_error(options);
            return false;
        }
        parsed = main_parse_flag_option(options, argv[i]);
        if (parsed > 0) {
            continue;
        }
        if (parsed < 0) {
            main_mark_usage_error(options);
            return false;
        }

        parsed = main_parse_long_value(options, argv[i]);
        if (parsed > 0) {
            continue;
        }
        if (parsed < 0) {
            main_mark_usage_error(options);
            return false;
        }
        if (!main_option_needs_value(argv[i])) {
            error2("unknown option: %s\n", argv[i]);
            main_mark_usage_error(options);
            return false;
        }
        if (i + 1 >= argc) {
            error2("%s requires a value\n", argv[i]);
            main_mark_usage_error(options);
            return false;
        }
        if (!main_parse_value_option(options, argv[i], argv[i + 1])) {
            main_mark_usage_error(options);
            return false;
        }
        i += 1;
    }

    provider = options->config.ort_session_config.execution_provider;
    device_id = options->config.ort_session_config.device_id;
    main_apply_model_defaults(&options->config);
    if (options->onnx_provider_set) {
        options->config.ort_session_config.execution_provider = provider;
    }
    if (options->onnx_device_set) {
        options->config.ort_session_config.device_id = device_id;
    }

    return main_validate_options(options);
}

static int32
main_generate_lrc(LrcPipelineConfig *config) {
    LrcPipeline pipeline;
    LrcPipelineGenerateResult result;
    int32 exit_status;

    lrc_pipeline_init(&pipeline, config);
    exit_status = EXIT_FAILURE;
    if (lrc_pipeline_generate_lrc(&pipeline, &result)) {
        exit_status = EXIT_SUCCESS;
    } else {
        error2("LRC generation failed: %s", result.path_header.header.message);
        if (result.path_header.path) {
            error2(": %s", result.path_header.path);
        }
        error2("\n");
    }

    lrc_pipeline_cleanup(&pipeline);

    return exit_status;
}

LYRICS_API void
lyrics_config_init(LrcPipelineConfig *config) {
    if (config == NULL) {
        return;
    }

    lrc_pipeline_config_init(config);
    main_apply_model_defaults(config);

    return;
}

LYRICS_API bool
lyrics_extract_vocals(
    LrcPipelineConfig *config,
    LrcVocalsExtractResult *result
) {
    LrcPipeline pipeline;
    bool ok;

    if (config == NULL) {
        lrc_vocals_extract_result_init(result);
        if (result) {
            result->path_header.header.error = LS_ERROR_VOCALS_EXTRACT_INVALID_ARGUMENT;
            result->path_header.header.message = "missing pipeline config";
        }
        return false;
    }

    main_apply_model_defaults(config);
    lrc_pipeline_init(&pipeline, config);
    ok = lrc_pipeline_extract_vocals(&pipeline, result);
    lrc_pipeline_cleanup(&pipeline);

    return ok;
}

LYRICS_API bool
lyrics_generate_lrc(
    LrcPipelineConfig *config,
    LrcPipelineGenerateResult *result
) {
    LrcPipeline pipeline;
    bool ok;

    if (config == NULL) {
        lrc_pipeline_generate_result_init(result);
        if (result) {
            result->path_header.header.error = LS_ERROR_PIPELINE_GENERATE_INVALID_ARGUMENT;
            result->path_header.header.message = "missing pipeline config";
        }
        return false;
    }

    main_apply_model_defaults(config);
    lrc_pipeline_init(&pipeline, config);
    ok = lrc_pipeline_generate_lrc(&pipeline, result);
    lrc_pipeline_cleanup(&pipeline);

    return ok;
}

LYRICS_API int32
lyrics_main(int32 argc, char **argv) {
    MainOptions options = {0};
    bool has_lyrics;

    if ((argc > 0) && argv) {
        program = argv[0];
    }

    lrc_pipeline_config_init(&options.config);
    if (!main_parse_args(&options, argc, argv)) {
        if (options.print_usage_on_error) {
            main_print_usage(stderr);
        }
        return EXIT_FAILURE;
    }

    has_lyrics = !path_missing(options.config.lyrics_text_path);
    if (!has_lyrics) {
        error2("internal error: lyrics path missing after validation\n");
        return EXIT_FAILURE;
    }

    if (!path_missing(options.config.existing_vocals_path)
        && !path_missing(options.config.vocals_path)) {
        error2("warning: --output-vocals ignored when --input-vocals is "
               "used\n");
    }

    return main_generate_lrc(&options.config);
}

#if !defined(LYRICS_BUILD_SHARED) || !LYRICS_BUILD_SHARED
int32
main(int32 argc, char **argv) {
    exit(lyrics_main(argc, argv));
}
#endif
