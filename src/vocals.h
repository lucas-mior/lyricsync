#if !defined(VOCALS_H)
#define VOCALS_H

#include "cbase.h"
#include "errors.h"
#include "audio.h"
#include "mdx.h"

/*
 * Phase-1 public API: original song + MDX model -> extracted vocals file.
 */

typedef struct LrcVocalsExtractRequest {
    char *input_path;
    char *output_path;
    char *model_path;
    char *temp_dir;
    char *ffmpeg_path;
    char *container_format;

    AudioIoFormat output_format;
    MdxConfig mdx_config;
    OrtSessionConfig ort_session_config;

    bool print_info;
} LrcVocalsExtractRequest;

typedef struct LrcVocalsExtractResult {
    LrcPathResultHeader path_header;
} LrcVocalsExtractResult;

static void lrc_vocals_extract_request_init(LrcVocalsExtractRequest *request);
static void lrc_vocals_extract_result_init(LrcVocalsExtractResult *result);
static bool lrc_extract_vocals(LrcVocalsExtractRequest *request,
                               LrcVocalsExtractResult *result);

#endif /* VOCALS_H */
