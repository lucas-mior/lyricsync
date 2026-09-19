#if !defined(PIPELINE_H)
#define PIPELINE_H

#include "cbase.h"
#include "errors.h"
#include "audio.h"
#include "mdx.h"
#include "vocals.h"
#include "ctc_assets.h"
#include "lyrics.h"
#include "ctc_tokenizer.h"
#include "ctc_audio.h"
#include "ctc_model.h"
#include "ctc_inference.h"
#include "ctc_align.h"
#include "lrc.h"
#include "progress.h"

#if !defined(LRC_PIPELINE_ENABLE_GENERATE)
#define LRC_PIPELINE_ENABLE_GENERATE 0
#endif

typedef struct LrcPipelineGenerateResult {
    LrcPathResultHeader path_header;

    int64 frame_index;
    int32 token_index;
    int32 line_index;
} LrcPipelineGenerateResult;

typedef struct LrcPipelineConfig {
    char *song_path;
    char *lyrics_text_path;
    char *existing_vocals_path;
    char *vocals_path;
    char *output_lrc_path;

    char *vocals_model_path;
    char *ctc_model_path;
    char *tokenizer_path;
    char *ctc_debug_dump_path;

    char *temp_dir;
    char *ffmpeg_path;
    char *vocals_container_format;

    AudioIoFormat vocals_output_format;
    MdxConfig mdx_config;
    LrcCtcModelConfig ctc_model_config;
    LrcLyricsPreprocessOptions lyrics_preprocess_options;
    OrtSessionConfig ort_session_config;

    enum LrcCtcEmissionValuesKind ctc_emission_values_kind;

    bool keep_temp_files;
    bool print_info;
} LrcPipelineConfig;

typedef struct LrcPipeline {
    LrcPipelineConfig config;

    enum LsError error;
    char *message;
    char *path;

    char owned_temp_dir[PATH_MAX];
    char owned_vocals_path[PATH_MAX];
    char *vocals_stage_path;

    LrcCtcAssets ctc_assets;

    bool prepared;
    bool owns_temp_dir;
    bool owns_vocals_path;
} LrcPipeline;

static void lrc_pipeline_config_init(LrcPipelineConfig *config);
static bool lrc_pipeline_debug_dump_enabled(LrcPipeline *pipeline);
static void lrc_pipeline_init(LrcPipeline *pipeline, LrcPipelineConfig *config);
static bool lrc_pipeline_prepare(LrcPipeline *pipeline);
static void lrc_pipeline_cleanup(LrcPipeline *pipeline);
static bool lrc_pipeline_vocals_request(LrcPipeline *pipeline,
                                        LrcVocalsExtractRequest *request);
static bool lrc_pipeline_extract_vocals(LrcPipeline *pipeline,
                                        LrcVocalsExtractResult *result);
static void lrc_pipeline_ctc_assets_config(LrcPipeline *pipeline,
                                           LrcCtcAssetsConfig *config);
static bool lrc_pipeline_validate_ctc_assets(LrcPipeline *pipeline,
                                             LrcCtcAssetsResult *result);
#if LRC_PIPELINE_ENABLE_GENERATE
static void lrc_pipeline_generate_result_init(
    LrcPipelineGenerateResult *result);
static bool lrc_pipeline_generate_lrc(LrcPipeline *pipeline,
                                      LrcPipelineGenerateResult *result);
#endif

#endif /* PIPELINE_H */
