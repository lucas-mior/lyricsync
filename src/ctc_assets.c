#include "cbase.h"
#include "lyricsync.h"
#include "ctc_assets.h"

#if !defined(TESTING_ctc_assets)
#define TESTING_ctc_assets 0
#endif

static void
lrc_ctc_assets_result_init(LrcCtcAssetsResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_init(&result->path_header);

    return;
}

static void
lrc_ctc_assets_result_set(
    LrcCtcAssetsResult *result,
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

static bool
lrc_ctc_assets_validate(
    LrcCtcAssets *assets,
    LrcCtcAssetsConfig *config,
    LrcCtcAssetsResult *result
) {
    if ((assets == NULL) || (config == NULL)) {
        lrc_ctc_assets_result_set(
            result,
            LS_ERROR_CTC_ASSETS_INVALID_ARGUMENT,
            "CTC asset validation received invalid arguments",
            NULL
        );
        return false;
    }

    memset64(assets, 0, SIZEOF(*assets));
    lrc_ctc_assets_result_init(result);

    if (path_missing(config->model_path)) {
        lrc_ctc_assets_result_set(
            result,
            LS_ERROR_CTC_ASSETS_MISSING_MODEL_PATH,
            "CTC model path is missing",
            config->model_path
        );
        return false;
    }
    if (path_missing(config->tokenizer_path)) {
        lrc_ctc_assets_result_set(
            result,
            LS_ERROR_CTC_ASSETS_MISSING_TOKENIZER_PATH,
            "CTC tokenizer path is missing",
            config->tokenizer_path
        );
        return false;
    }
    if (!util_file_exists(config->model_path)) {
        lrc_ctc_assets_result_set(
            result,
            LS_ERROR_CTC_ASSETS_MODEL_NOT_FOUND,
            "CTC model file was not found",
            config->model_path
        );
        return false;
    }
    if (!util_file_exists(config->tokenizer_path)) {
        lrc_ctc_assets_result_set(
            result,
            LS_ERROR_CTC_ASSETS_TOKENIZER_NOT_FOUND,
            "CTC tokenizer file was not found",
            config->tokenizer_path
        );
        return false;
    }

    assets->model_path = config->model_path;
    assets->tokenizer_path = config->tokenizer_path;
    assets->validated = true;

    return true;
}

#if TESTING_ctc_assets
#define CBASE_IMPLEMENT
#include "cbase.h"

static int32
ctc_assets_test_fail(char *name) {
    error2("CTC assets test failed: %s\n", name);

    return 1;
}

static bool
ctc_assets_write_file(char *path, char *text) {
    return write_entire_file(path, text, strlen32(text));
}


static void
ctc_assets_test_config_defaults(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets = {0};

    lrc_ctc_assets_result_init(&result);

    ASSERT(config.model_path == NULL);
    ASSERT(config.tokenizer_path == NULL);
    ASSERT(result.path_header.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.path_header.header.message, "ok"));
    ASSERT(result.path_header.path == NULL);
    ASSERT(assets.model_path == NULL);
    ASSERT(assets.tokenizer_path == NULL);
    ASSERT(!assets.validated);

    return;
}

static void
ctc_assets_test_valid_generated_files(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets;
    char temp_dir[PATH_MAX];
    char model_path[PATH_MAX];
    char tokenizer_path[PATH_MAX];

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_assets_valid");
    test_join_path(model_path, SIZEOF(model_path), temp_dir,
                         "model.onnx");
    test_join_path(tokenizer_path, SIZEOF(tokenizer_path), temp_dir,
                         "tokens.txt");

    if (!ctc_assets_write_file(model_path, "fake model\n")) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("write fake model"));
    }
    if (!ctc_assets_write_file(tokenizer_path, "<blank>\na\nb\n")) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("write fake tokenizer"));
    }

    config.model_path = model_path;
    config.tokenizer_path = tokenizer_path;

    if (!lrc_ctc_assets_validate(&assets, &config, &result)) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("validate generated files"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.path_header.header.message, "ok"));
    ASSERT(strequal(assets.model_path, model_path));
    ASSERT(strequal(assets.tokenizer_path, tokenizer_path));
    ASSERT(assets.validated);

    test_remove_tree(temp_dir);

    return;
}

static void
ctc_assets_test_missing_model_path(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets;

    config.tokenizer_path = "tokens.txt";

    if (lrc_ctc_assets_validate(&assets, &config, &result)) {
        fatal(ctc_assets_test_fail("missing model path accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_ASSETS_MISSING_MODEL_PATH);
    ASSERT(strequal(result.path_header.header.message, "CTC model path is missing"));
    ASSERT(result.path_header.path == NULL);
    ASSERT(!assets.validated);

    return;
}

static void
ctc_assets_test_missing_tokenizer_path(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets;

    config.model_path = "model.onnx";

    if (lrc_ctc_assets_validate(&assets, &config, &result)) {
        fatal(ctc_assets_test_fail("missing tokenizer path accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_ASSETS_MISSING_TOKENIZER_PATH);
    ASSERT(strequal(result.path_header.header.message, "CTC tokenizer path is missing"));
    ASSERT(result.path_header.path == NULL);
    ASSERT(!assets.validated);

    return;
}

static void
ctc_assets_test_missing_model_file(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets;
    char temp_dir[PATH_MAX];
    char model_path[PATH_MAX];
    char tokenizer_path[PATH_MAX];

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_assets_no_model");
    test_join_path(model_path, SIZEOF(model_path), temp_dir,
                         "missing.onnx");
    test_join_path(tokenizer_path, SIZEOF(tokenizer_path), temp_dir,
                         "tokens.txt");

    if (!ctc_assets_write_file(tokenizer_path, "<blank>\na\n")) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("write tokenizer for missing model test"));
    }

    config.model_path = model_path;
    config.tokenizer_path = tokenizer_path;

    if (lrc_ctc_assets_validate(&assets, &config, &result)) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("missing model file accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_ASSETS_MODEL_NOT_FOUND);
    ASSERT(strequal(result.path_header.path, model_path));
    ASSERT(!assets.validated);

    test_remove_tree(temp_dir);

    return;
}

static void
ctc_assets_test_missing_tokenizer_file(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets;
    char temp_dir[PATH_MAX];
    char model_path[PATH_MAX];
    char tokenizer_path[PATH_MAX];

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_assets_no_tokens");
    test_join_path(model_path, SIZEOF(model_path), temp_dir,
                         "model.onnx");
    test_join_path(tokenizer_path, SIZEOF(tokenizer_path), temp_dir,
                         "missing.txt");

    if (!ctc_assets_write_file(model_path, "fake model\n")) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("write model for missing tokenizer test"));
    }

    config.model_path = model_path;
    config.tokenizer_path = tokenizer_path;

    if (lrc_ctc_assets_validate(&assets, &config, &result)) {
        test_remove_tree(temp_dir);
        fatal(ctc_assets_test_fail("missing tokenizer file accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_ASSETS_TOKENIZER_NOT_FOUND);
    ASSERT(strequal(result.path_header.path, tokenizer_path));
    ASSERT(!assets.validated);

    test_remove_tree(temp_dir);

    return;
}

static void
ctc_assets_test_invalid_arguments(void) {
    LrcCtcAssetsConfig config = {0};
    LrcCtcAssetsResult result;
    LrcCtcAssets assets;


    if (lrc_ctc_assets_validate(NULL, &config, &result)) {
        fatal(ctc_assets_test_fail("null assets accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_ASSETS_INVALID_ARGUMENT);

    if (lrc_ctc_assets_validate(&assets, NULL, &result)) {
        fatal(ctc_assets_test_fail("null config accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_ASSETS_INVALID_ARGUMENT);

    return;
}

int32
main(void) {
    ctc_assets_test_config_defaults();
    ctc_assets_test_valid_generated_files();
    ctc_assets_test_missing_model_path();
    ctc_assets_test_missing_tokenizer_path();
    ctc_assets_test_missing_model_file();
    ctc_assets_test_missing_tokenizer_file();
    ctc_assets_test_invalid_arguments();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_ctc_assets */
