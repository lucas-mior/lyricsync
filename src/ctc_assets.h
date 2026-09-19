#if !defined(CTC_ASSETS_H)
#define CTC_ASSETS_H

#include "cbase.h"
#include "errors.h"

typedef struct LrcCtcAssetsConfig {
    char *model_path;
    char *tokenizer_path;
} LrcCtcAssetsConfig;

typedef struct LrcCtcAssetsResult {
    LrcPathResultHeader path_header;
} LrcCtcAssetsResult;

typedef struct LrcCtcAssets {
    char *model_path;
    char *tokenizer_path;

    bool validated;
} LrcCtcAssets;

static void lrc_ctc_assets_result_init(LrcCtcAssetsResult *result);
static bool lrc_ctc_assets_validate(LrcCtcAssets *assets,
                                    LrcCtcAssetsConfig *config,
                                    LrcCtcAssetsResult *result);

#endif /* CTC_ASSETS_H */
