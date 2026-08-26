#if !defined(CTC_TEXT_H)
#define CTC_TEXT_H

#include "cbase.h"

typedef struct LrcLyrics LrcLyrics;

#define ENUM_NAME LrcLyricsPreprocessSplitSize
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ LRC_LYRICS_PREPROCESS_SPLIT_SIZE_
#define ENUM_FIELDS \
    XX(LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CURRENT)  \
    XX(LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD)     \
    XX(LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CHAR)     \
    XX(LRC_LYRICS_PREPROCESS_SPLIT_SIZE_SENTENCE)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS

#define ENUM_NAME LrcLyricsPreprocessStarFrequency
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_
#define ENUM_FIELDS \
    XX(LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_NONE)    \
    XX(LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_EDGES)   \
    XX(LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_SEGMENT)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS

#define ENUM_NAME LrcLyricsPreprocessRomanization
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ LRC_LYRICS_PREPROCESS_ROMANIZATION_
#define ENUM_FIELDS \
    XX(LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF) \
    XX(LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS

typedef struct LrcLyricsPreprocessOptions {
    enum LrcLyricsPreprocessSplitSize split_size;
    enum LrcLyricsPreprocessStarFrequency star_frequency;
    enum LrcLyricsPreprocessRomanization romanization;

    char language[4];
    int32 language_len;
} LrcLyricsPreprocessOptions;

typedef struct LrcLyricsNormalizedByte {
    int32 line_index;
    int32 source_start;
    int32 source_end;
} LrcLyricsNormalizedByte;

typedef struct LrcLyricsTargetByte {
    int32 line_index;
    int32 normalized_start;
    int32 normalized_end;
} LrcLyricsTargetByte;

enum LrcLyricsNormalizedLineKind {
    LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK,
    LRC_LYRICS_NORMALIZED_LINE_KIND_SECTION_MARKER,
    LRC_LYRICS_NORMALIZED_LINE_KIND_PUNCTUATION_ONLY,
    LRC_LYRICS_NORMALIZED_LINE_KIND_ALIGNABLE,
};

typedef struct LrcLyricsNormalizedLine {
    enum LrcLyricsNormalizedLineKind kind;

    int32 normalized_start;
    int32 normalized_end;
} LrcLyricsNormalizedLine;

typedef struct CtcTextSegment {
    int32 line_index;
    int32 source_start;
    int32 source_end;

    int32 normalized_start;
    int32 normalized_end;
    int32 target_start;
    int32 target_end;
} CtcTextSegment;

typedef struct LrcLyricsNormalized {
    char *text;
    char *target_text;
    LrcLyricsNormalizedByte *bytes;
    LrcLyricsTargetByte *target_bytes;
    LrcLyricsNormalizedLine *lines;
    CtcTextSegment *segments;

    int32 text_len;
    int32 target_text_len;
    int32 byte_count;
    int32 target_byte_count;
    int32 line_count;
    int32 segment_count;
    int32 alignable_line_count;
} LrcLyricsNormalized;

static void
lrc_lyrics_preprocess_options_init(
    LrcLyricsPreprocessOptions *options
) {
    if (options == NULL) {
        return;
    }

    memset64(options, 0, SIZEOF(*options));

    options->split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    options->star_frequency = LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_EDGES;
    options->romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU;

    memcpy64(options->language, STRLIT("eng"));
    options->language[3] = '\0';
    options->language_len = 3;

    return;
}
static void lrc_lyrics_normalized_destroy(LrcLyricsNormalized *normalized);
static bool lrc_lyrics_normalize_with_options(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsPreprocessOptions *options
);
#if TESTING
static bool lrc_lyrics_normalize(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized
);
static int32 lrc_lyrics_normalized_line_at(
    LrcLyricsNormalized *normalized,
    int32 byte_offset
);
#endif
static enum LrcLyricsNormalizedLineKind lrc_lyrics_normalized_line_kind(
    LrcLyricsNormalized *normalized,
    int32 line_index
);
static bool lrc_lyrics_normalized_line_range(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 *start,
    int32 *end
);
#endif /* CTC_TEXT_H */
