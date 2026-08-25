#include "cbase.h"
#include "lyricsync.h"
#include "unicode_norm.h"

#if LRC_UNICODE_ENABLE_ICU
#include <unicode/ucasemap.h>
#include <unicode/unorm2.h>
#include <unicode/utrans.h>
#include <unicode/ustring.h>
#endif

#if !defined(TESTING_unicode_norm)
#define TESTING_unicode_norm 0
#endif

static void
ctc_unicode_norm_result_init(CtcUnicodeNormResult *result) {
    if (result == NULL) {
        return;
    }

    sb_init(&result->text);
    result->used_icu = false;

    return;
}

static void
ctc_unicode_norm_result_destroy(CtcUnicodeNormResult *result) {
    if (result == NULL) {
        return;
    }

    sb_free(&result->text);
    result->used_icu = false;

    return;
}

static bool
ctc_unicode_norm_reserve(
    CtcUnicodeNormResult *result,
    int32 needed
) {
    int32 extra;

    if (result == NULL) {
        return false;
    }
    if (needed < 0) {
        return false;
    }
    if (needed >= INT32_MAX) {
        return false;
    }
    if (needed <= result->text.len) {
        return true;
    }

    extra = needed - result->text.len;
    sb_reserve(&result->text, extra);

    return true;
}

static bool
ctc_unicode_norm_copy_fallback(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    if (text_len < 0) {
        return false;
    }
    if (sb_set(&result->text, text, text_len) < 0) {
        return false;
    }

    result->used_icu = false;

    return true;
}

#if LRC_UNICODE_ENABLE_ICU
static bool
ctc_unicode_norm_check_icu_status(UErrorCode status) {
    if (U_SUCCESS(status)) {
        return true;
    }
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        return true;
    }

    return false;
}

static bool
ctc_unicode_norm_preflight_utf16_from_utf8(
    char *text,
    int32 text_len,
    int32 *utf16_len
) {
    UErrorCode status = U_ZERO_ERROR;

    u_strFromUTF8(NULL,
                  0,
                  (int32_t *)utf16_len,
                  text,
                  (int32_t)text_len,
                  &status);

    return ctc_unicode_norm_check_icu_status(status);
}

static bool
ctc_unicode_norm_utf16_from_utf8(
    char *text,
    int32 text_len,
    UChar *utf16,
    int32 utf16_cap,
    int32 *utf16_len
) {
    UErrorCode status = U_ZERO_ERROR;

    u_strFromUTF8(utf16,
                  (int32_t)utf16_cap,
                  (int32_t *)utf16_len,
                  text,
                  (int32_t)text_len,
                  &status);

    return U_SUCCESS(status);
}

static bool
ctc_unicode_norm_preflight_nfkc(
    UChar *utf16,
    int32 utf16_len,
    UNormalizer2 *normalizer,
    int32 *nfkc_len
) {
    UErrorCode status = U_ZERO_ERROR;

    *nfkc_len = (int32)unorm2_normalize(normalizer,
                                        utf16,
                                        (int32_t)utf16_len,
                                        NULL,
                                        0,
                                        &status);

    return ctc_unicode_norm_check_icu_status(status);
}

static bool
ctc_unicode_norm_nfkc(
    UChar *utf16,
    int32 utf16_len,
    UNormalizer2 *normalizer,
    UChar *nfkc,
    int32 nfkc_cap,
    int32 *nfkc_len
) {
    UErrorCode status = U_ZERO_ERROR;

    *nfkc_len = (int32)unorm2_normalize(normalizer,
                                        utf16,
                                        (int32_t)utf16_len,
                                        nfkc,
                                        (int32_t)nfkc_cap,
                                        &status);

    return U_SUCCESS(status);
}

static bool
ctc_unicode_norm_preflight_lower(
    UChar *nfkc,
    int32 nfkc_len,
    int32 *lower_len
) {
    UErrorCode status = U_ZERO_ERROR;

    *lower_len = (int32)u_strToLower(NULL,
                                     0,
                                     nfkc,
                                     (int32_t)nfkc_len,
                                     "",
                                     &status);

    return ctc_unicode_norm_check_icu_status(status);
}

static bool
ctc_unicode_norm_lower(
    UChar *nfkc,
    int32 nfkc_len,
    UChar *lower,
    int32 lower_cap,
    int32 *lower_len
) {
    UErrorCode status = U_ZERO_ERROR;

    *lower_len = (int32)u_strToLower(lower,
                                     (int32_t)lower_cap,
                                     nfkc,
                                     (int32_t)nfkc_len,
                                     "",
                                     &status);

    return U_SUCCESS(status);
}

static bool
ctc_unicode_norm_preflight_utf8_from_utf16(
    UChar *utf16,
    int32 utf16_len,
    int32 *utf8_len
) {
    UErrorCode status = U_ZERO_ERROR;

    u_strToUTF8(NULL,
                0,
                (int32_t *)utf8_len,
                utf16,
                (int32_t)utf16_len,
                &status);

    return ctc_unicode_norm_check_icu_status(status);
}

static bool
ctc_unicode_norm_utf8_from_utf16(
    UChar *utf16,
    int32 utf16_len,
    CtcUnicodeNormResult *result
) {
    UErrorCode status;
    int32 utf8_len;

    if (!ctc_unicode_norm_preflight_utf8_from_utf16(utf16,
                                                    utf16_len,
                                                    &utf8_len)) {
        return false;
    }
    sb_clear(&result->text);
    if (!ctc_unicode_norm_reserve(result, utf8_len)) {
        return false;
    }

    status = U_ZERO_ERROR;
    u_strToUTF8(result->text.data,
                (int32_t)result->text.cap,
                (int32_t *)&result->text.len,
                utf16,
                (int32_t)utf16_len,
                &status);
    if (!U_SUCCESS(status)) {
        return false;
    }

    result->text.data[result->text.len] = '\0';
    result->used_icu = true;

    return true;
}

static bool
ctc_unicode_norm_nfkc_lower_icu(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    UNormalizer2 *normalizer;
    UChar *utf16;
    UChar *nfkc;
    UChar *lower;
    UErrorCode status;
    int32 utf16_len;
    int32 nfkc_len;
    int32 lower_len;
    int32 utf16_cap;
    int32 nfkc_cap;
    int32 lower_cap;
    bool ok;

    if (text_len < 0) {
        return false;
    }

    status = U_ZERO_ERROR;
    normalizer = (UNormalizer2 *)unorm2_getNFKCInstance(&status);
    if (!U_SUCCESS(status) || (normalizer == NULL)) {
        return false;
    }

    if (!ctc_unicode_norm_preflight_utf16_from_utf8(text,
                                                    text_len,
                                                    &utf16_len)) {
        return false;
    }

    utf16_cap = utf16_len + 1;
    nfkc_cap = 0;
    lower_cap = 0;
    utf16 = malloc2((int64)utf16_cap*SIZEOF(*utf16));
    nfkc = NULL;
    lower = NULL;
    ok = false;

    if (!ctc_unicode_norm_utf16_from_utf8(text,
                                          text_len,
                                          utf16,
                                          utf16_cap,
                                          &utf16_len)) {
        goto done;
    }
    if (!ctc_unicode_norm_preflight_nfkc(utf16,
                                         utf16_len,
                                         normalizer,
                                         &nfkc_len)) {
        goto done;
    }

    nfkc_cap = nfkc_len + 1;
    nfkc = malloc2((int64)nfkc_cap*SIZEOF(*nfkc));
    if (!ctc_unicode_norm_nfkc(utf16,
                               utf16_len,
                               normalizer,
                               nfkc,
                               nfkc_cap,
                               &nfkc_len)) {
        goto done;
    }
    if (!ctc_unicode_norm_preflight_lower(nfkc,
                                          nfkc_len,
                                          &lower_len)) {
        goto done;
    }

    lower_cap = lower_len + 1;
    lower = malloc2((int64)lower_cap*SIZEOF(*lower));
    if (!ctc_unicode_norm_lower(nfkc,
                                nfkc_len,
                                lower,
                                lower_cap,
                                &lower_len)) {
        goto done;
    }

    ok = ctc_unicode_norm_utf8_from_utf16(lower, lower_len, result);

done:
    free2(lower, (int64)lower_cap*SIZEOF(*lower));
    free2(nfkc, (int64)nfkc_cap*SIZEOF(*nfkc));
    free2(utf16, (int64)utf16_cap*SIZEOF(*utf16));

    return ok;
}

static bool
ctc_unicode_norm_open_latin_transliterator(
    UTransliterator **transliterator
) {
    UParseError parse_error = {0};
    UErrorCode status;
    UChar id[64];
    int32 id_len;
    int32 id_cap;

    *transliterator = NULL;
    id_cap = (int32)(SIZEOF(id)/SIZEOF(id[0]));
    if (!ctc_unicode_norm_utf16_from_utf8(STRLIT("Any-Latin; Latin-ASCII"),
                                          id,
                                          id_cap,
                                          &id_len)) {
        return false;
    }

    status = U_ZERO_ERROR;
    *transliterator = utrans_openU(id,
                                   (int32_t)id_len,
                                   UTRANS_FORWARD,
                                   NULL,
                                   0,
                                   &parse_error,
                                   &status);
    if (!U_SUCCESS(status) || (*transliterator == NULL)) {
        return false;
    }

    return true;
}

static bool
ctc_unicode_norm_copy_utf16(
    UChar *destination,
    int32 destination_cap,
    UChar *source,
    int32 source_len
) {
    if (source_len < 0) {
        return false;
    }
    if (source_len >= destination_cap) {
        return false;
    }

    if (source_len > 0) {
        memcpy64(destination, source, (int64)source_len*SIZEOF(*source));
    }
    destination[source_len] = 0;

    return true;
}

static bool
ctc_unicode_norm_transliterate_latin_try_cap(
    UTransliterator *transliterator,
    UChar *source,
    int32 source_len,
    int32 work_cap,
    CtcUnicodeNormResult *result,
    bool *needs_more
) {
    UErrorCode status;
    UChar *work;
    int32 work_len;
    int32 limit;
    bool ok;

    *needs_more = false;
    if (work_cap <= source_len) {
        return false;
    }

    work = malloc2((int64)work_cap*SIZEOF(*work));
    ok = false;
    if (!ctc_unicode_norm_copy_utf16(work, work_cap, source, source_len)) {
        goto done;
    }

    work_len = source_len;
    limit = source_len;
    status = U_ZERO_ERROR;
    utrans_transUChars(transliterator,
                       work,
                       (int32_t *)&work_len,
                       (int32_t)work_cap,
                       0,
                       (int32_t *)&limit,
                       &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        *needs_more = true;
        goto done;
    }
    if (!U_SUCCESS(status)) {
        goto done;
    }

    ok = ctc_unicode_norm_utf8_from_utf16(work, work_len, result);

done:
    free2(work, (int64)work_cap*SIZEOF(*work));

    return ok;
}

static bool
ctc_unicode_norm_transliterate_latin_icu(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    UTransliterator *transliterator;
    UChar *source;
    int32 source_len;
    int32 source_cap;
    bool ok;

    if (text_len < 0) {
        return false;
    }

    if (!ctc_unicode_norm_open_latin_transliterator(&transliterator)) {
        return false;
    }
    if (!ctc_unicode_norm_preflight_utf16_from_utf8(text,
                                                    text_len,
                                                    &source_len)) {
        utrans_close(transliterator);
        return false;
    }

    source_cap = source_len + 1;
    source = malloc2((int64)source_cap*SIZEOF(*source));
    ok = false;
    if (!ctc_unicode_norm_utf16_from_utf8(text,
                                          text_len,
                                          source,
                                          source_cap,
                                          &source_len)) {
        goto done;
    }

    for (int32 multiplier = 8; multiplier <= 128; multiplier *= 2) {
        int32 work_cap = source_len*multiplier + 64;
        bool needs_more;

        if (ctc_unicode_norm_transliterate_latin_try_cap(transliterator,
                                                         source,
                                                         source_len,
                                                         work_cap,
                                                         result,
                                                         &needs_more)) {
            ok = true;
            break;
        }
        if (!needs_more) {
            break;
        }
    }

done:
    free2(source, (int64)source_cap*SIZEOF(*source));
    utrans_close(transliterator);

    return ok;
}
#endif

static bool
ctc_unicode_norm_nfkc_lower(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    if ((text == NULL) || (result == NULL)) {
        return false;
    }

    ctc_unicode_norm_result_destroy(result);
#if LRC_UNICODE_ENABLE_ICU
    if (ctc_unicode_norm_nfkc_lower_icu(text, text_len, result)) {
        return true;
    }
#endif

    return ctc_unicode_norm_copy_fallback(text, text_len, result);
}

static bool
ctc_unicode_norm_transliterate_latin(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    if ((text == NULL) || (result == NULL)) {
        return false;
    }

    ctc_unicode_norm_result_destroy(result);
#if LRC_UNICODE_ENABLE_ICU
    return ctc_unicode_norm_transliterate_latin_icu(text, text_len, result);
#else
    (void)text_len;
    return false;
#endif
}

#if TESTING_unicode_norm
#define CBASE_IMPLEMENT
#include "cbase.h"

static bool
ctc_unicode_norm_icu_available(void) {
#if LRC_UNICODE_ENABLE_ICU
    return true;
#else
    return false;
#endif
}

static int32
unicode_norm_test_fail(char *name) {
    error2("unicode norm test failed: %s\n", name);

    return 1;
}

static int32
unicode_norm_test_fallback_or_icu_copy(void) {
    CtcUnicodeNormResult result;

    ctc_unicode_norm_result_init(&result);
    if (!ctc_unicode_norm_nfkc_lower(STRLIT("Hello"), &result)) {
        return unicode_norm_test_fail("normalize ascii");
    }

#if LRC_UNICODE_ENABLE_ICU
    ASSERT(ctc_unicode_norm_icu_available());
    ASSERT(result.used_icu);
    ASSERT_EQUAL(result.text.data, "hello");
#else
    ASSERT(!ctc_unicode_norm_icu_available());
    ASSERT(!result.used_icu);
    ASSERT_EQUAL(result.text.data, "Hello");
#endif

    ctc_unicode_norm_result_destroy(&result);

    return 0;
}

#if LRC_UNICODE_ENABLE_ICU
static int32
unicode_norm_test_icu_nfkc_lower(void) {
    CtcUnicodeNormResult result;

    ctc_unicode_norm_result_init(&result);
    if (!ctc_unicode_norm_nfkc_lower(STRLIT("ＡÉ"), &result)) {
        return unicode_norm_test_fail("normalize nfkc lower");
    }

    ASSERT(result.used_icu);
    ASSERT_EQUAL(result.text.data, "aé");

    ctc_unicode_norm_result_destroy(&result);

    return 0;
}

static int32
unicode_norm_test_icu_transliterate_latin(void) {
    CtcUnicodeNormResult result;

    ctc_unicode_norm_result_init(&result);
    if (!ctc_unicode_norm_transliterate_latin(STRLIT("猫 Привет"), &result)) {
        return unicode_norm_test_fail("transliterate latin");
    }

    ASSERT(result.used_icu);
    ASSERT_EQUAL(result.text.data, "mao Privet");

    ctc_unicode_norm_result_destroy(&result);

    return 0;
}
#endif

int32
main(void) {
    int32 status = 0;


    status += unicode_norm_test_fallback_or_icu_copy();
#if LRC_UNICODE_ENABLE_ICU
    status += unicode_norm_test_icu_nfkc_lower();
    status += unicode_norm_test_icu_transliterate_latin();
#endif

    exit(status);
}
#endif /* TESTING_unicode_norm */
