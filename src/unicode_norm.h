#if !defined(UNICODE_NORM_H)
#define UNICODE_NORM_H

#include "cbase.h"

#if !defined(LRC_UNICODE_ENABLE_ICU)
#define LRC_UNICODE_ENABLE_ICU 0
#endif

typedef struct CtcUnicodeNormResult {
    String text;

    bool used_icu;
} CtcUnicodeNormResult;

static void ctc_unicode_norm_result_init(CtcUnicodeNormResult *result);
static void ctc_unicode_norm_result_destroy(CtcUnicodeNormResult *result);
static bool ctc_unicode_norm_nfkc_lower(char *text, int32 text_len,
                                        CtcUnicodeNormResult *result);
static bool ctc_unicode_norm_transliterate_latin(char *text, int32 text_len,
                                                 CtcUnicodeNormResult *result);

#endif /* UNICODE_NORM_H */
