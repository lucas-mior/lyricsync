#include "cbase.h"
#include "lyricsync.h"
#include "lyrics.h"

#if !defined(TESTING_lyrics)
#define TESTING_lyrics 0
#endif

static void
lrc_lyrics_destroy(LrcLyrics *lyrics) {
    if (lyrics == NULL) {
        return;
    }

    free2(lyrics->text, ((int64)lyrics->text_len + 1)*SIZEOF(*lyrics->text));
    ARRAY_FREE(lyrics->lines);

    memset64(lyrics, 0, SIZEOF(*lyrics));

    return;
}

static void
lrc_lyrics_load_result_init(LrcLyricsLoadResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_init(&result->path_header);

    result->byte_offset = -1;

    return;
}

static void
lrc_lyrics_load_result_set(
    LrcLyricsLoadResult *result,
    enum LsError error,
    char *message,
    char *path,
    int32 byte_offset
) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_set(&result->path_header, error, message, path);

    result->byte_offset = byte_offset;

    return;
}

static bool
lrc_lyrics_normalize_text(
    LrcLyrics *lyrics,
    char *file_text,
    int32 file_len,
    LrcLyricsLoadResult *result,
    char *path
) {
    int32 bad_offset;
    int32 start;
    StrBuilder normalized;

    if (!utf8_valid(file_text, file_len, &bad_offset)) {
        lrc_lyrics_load_result_set(
            result,
            LS_ERROR_LYRICS_LOAD_INVALID_UTF8,
            "lyrics file is not valid UTF-8",
            path,
            bad_offset
        );
        return false;
    }

    start = 0;
    lyrics->had_utf8_bom = utf8_has_bom(file_text, file_len);
    if (lyrics->had_utf8_bom) {
        start = 3;
    }

    sb_init(&normalized);
    sb_reserve(&normalized, file_len - start);
    for (int32 i = start; i < file_len; i += 1) {
        if (file_text[i] == '\r') {
            sb_append_byte(&normalized, '\n');
            if (((i + 1) < file_len) && (file_text[i + 1] == '\n')) {
                i += 1;
            }
        } else {
            sb_append(&normalized, file_text + i, 1);
        }
    }
    sb_append(&normalized, "", 0);

    lyrics->text = sb_steal_exact(&normalized, &lyrics->text_len);

    return true;
}

static bool
lrc_lyrics_line_has_text(char *text, int32 text_len) {
    for (int32 i = 0; i < text_len; i += 1) {
        if ((text[i] != ' ') && (text[i] != '\t') && (text[i] != '\n')) {
            return true;
        }
    }

    return false;
}

static bool
lrc_lyrics_reserve_lines(LrcLyrics *lyrics, int32 extra) {
    int64 needed;

    if (extra <= 0) {
        return true;
    }

    needed = (int64)lyrics->line_count + extra;
    if (needed > INT32_MAX) {
        return false;
    }

    return ARRAY_RESERVE(lyrics->lines, (int32)needed);
}

static bool
lrc_lyrics_append_line(LrcLyrics *lyrics, int32 start, int32 end) {
    LrcLyricsLine *line;

    if (!lrc_lyrics_reserve_lines(lyrics, 1)) {
        return false;
    }

    line = &lyrics->lines[lyrics->line_count];
    lyrics->line_count += 1;
    ARRAY_SET_COUNT(lyrics->lines, lyrics->line_count);

    line->text = lyrics->text + start;
    line->text_len = end - start;
    line->text_start = start;
    line->text_end = end;

    if (lrc_lyrics_line_has_text(line->text, line->text_len)) {
        lyrics->nonempty_line_count += 1;
    }

    return true;
}

static bool
lrc_lyrics_split_lines(LrcLyrics *lyrics) {
    int32 line_start = 0;

    for (int32 i = 0; i < lyrics->text_len; i += 1) {
        if (lyrics->text[i] == '\n') {
            if (!lrc_lyrics_append_line(lyrics, line_start, i)) {
                return false;
            }
            line_start = i + 1;
        }
    }

    if ((line_start < lyrics->text_len) || (lyrics->text_len == 0)) {
        if (!lrc_lyrics_append_line(lyrics, line_start, lyrics->text_len)) {
            return false;
        }
    }

    return true;
}

static bool
lrc_lyrics_load_file(
    LrcLyrics *lyrics,
    char *path,
    LrcLyricsLoadResult *result
) {
    char *file_text;
    int32 file_len;

    if (result) {
        lrc_lyrics_load_result_init(result);
    }
    if (lyrics == NULL) {
        lrc_lyrics_load_result_set(
            result,
            LS_ERROR_LYRICS_LOAD_INVALID_ARGUMENT,
            "lyrics object is missing",
            path,
            -1
        );
        return false;
    }
    if (path_missing(path)) {
        lrc_lyrics_load_result_set(
            result,
            LS_ERROR_LYRICS_LOAD_MISSING_PATH,
            "lyrics path is missing",
            path,
            -1
        );
        return false;
    }

    lrc_lyrics_destroy(lyrics);
    file_text = NULL;
    file_len = 0;
    if ((file_len = read_entire_file(path, &file_text)) < 0) {
        lrc_lyrics_load_result_set(
            result,
            LS_ERROR_LYRICS_LOAD_READ_FAILED,
            "could not read lyrics file",
            path,
            -1
        );
        return false;
    }
    if (!lrc_lyrics_normalize_text(lyrics, file_text, file_len, result, path)) {
        free2(file_text, ((int64)file_len + 1)*SIZEOF(*file_text));
        lrc_lyrics_destroy(lyrics);
        return false;
    }
    free2(file_text, ((int64)file_len + 1)*SIZEOF(*file_text));

    if (!lrc_lyrics_split_lines(lyrics)) {
        lrc_lyrics_load_result_set(
            result,
            LS_ERROR_LYRICS_LOAD_FILE_TOO_LARGE,
            "lyrics file has too many lines",
            path,
            -1
        );
        lrc_lyrics_destroy(lyrics);
        return false;
    }
    if (lyrics->nonempty_line_count <= 0) {
        lrc_lyrics_load_result_set(
            result,
            LS_ERROR_LYRICS_LOAD_EMPTY,
            "lyrics file is empty",
            path,
            -1
        );
        lrc_lyrics_destroy(lyrics);
        return false;
    }

    return true;
}



#if TESTING_lyrics
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "unicode_norm.c"
#include "ctc_text.c"

static int32
lyrics_test_fail(char *name) {
    error2("lyrics test failed: %s\n", name);

    return 1;
}

static bool
lyrics_test_write(char *path, char *text, int32 text_len) {
    return write_entire_file(path, text, text_len) >= 0;
}

static bool
lyrics_test_load_text(LrcLyrics *lyrics, char *text, int32 text_len) {
    LrcLyricsLoadResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    int32 len;
    bool ok;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lyrics_text");
    len = snprintf2(path, SIZEOF(path), "%s/lyrics.txt", temp_dir);
    if ((len <= 0) || (len >= SIZEOF(path))) {
        test_remove_tree(temp_dir);
        return false;
    }
    if (!lyrics_test_write(path, text, text_len)) {
        test_remove_tree(temp_dir);
        return false;
    }

    memset64(lyrics, 0, SIZEOF(*lyrics));
    ok = lrc_lyrics_load_file(lyrics, path, &result);
    test_remove_tree(temp_dir);

    return ok;
}

static void
lyrics_test_assert_line_range(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 expected_start,
    int32 expected_end
) {
    int32 start;
    int32 end;

    ASSERT(lrc_lyrics_normalized_line_range(normalized,
                                            line_index,
                                            &start,
                                            &end));
    ASSERT(start == expected_start);
    ASSERT(end == expected_end);

    return;
}

static void
lyrics_test_assert_no_line_range(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    enum LrcLyricsNormalizedLineKind expected_kind
) {
    int32 start;
    int32 end;

    ASSERT(!lrc_lyrics_normalized_line_range(normalized,
                                             line_index,
                                             &start,
                                             &end));
    ASSERT(start == -1);
    ASSERT(end == -1);
    ASSERT(lrc_lyrics_normalized_line_kind(normalized, line_index)
           == expected_kind);

    return;
}

static void
lyrics_test_crlf_and_trailing_newline(void) {
    LrcLyrics lyrics;
    char text[] = "First\r\nSecond\rThird\n";

    if (!lyrics_test_load_text(&lyrics, text, strlen32(text))) {
        fatal(lyrics_test_fail("load crlf text"));
    }
    ASSERT_EQUAL(lyrics.text, "First\nSecond\nThird\n");
    ASSERT(lyrics.line_count == 3);
    ASSERT(lyrics.nonempty_line_count == 3);
    ASSERT(STREQUAL(lyrics.lines[0].text, lyrics.lines[0].text_len,
                     "First"));
    ASSERT(STREQUAL(lyrics.lines[1].text, lyrics.lines[1].text_len,
                     "Second"));
    ASSERT(STREQUAL(lyrics.lines[2].text, lyrics.lines[2].text_len,
                     "Third"));

    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
lyrics_test_bom_unicode_and_blank_lines(void) {
    LrcLyrics lyrics;
    char text[] = "\xEF\xBB\xBFOlá\r\n\r\n世界";

    if (!lyrics_test_load_text(&lyrics, text, SIZEOF(text) - 1)) {
        fatal(lyrics_test_fail("load bom unicode text"));
    }
    ASSERT(lyrics.had_utf8_bom);
    ASSERT_EQUAL(lyrics.text, "Olá\n\n世界");
    ASSERT(lyrics.line_count == 3);
    ASSERT(lyrics.nonempty_line_count == 2);
    ASSERT(STREQUAL(lyrics.lines[0].text, lyrics.lines[0].text_len,
                     "Olá"));
    ASSERT(lyrics.lines[1].text_len == 0);
    ASSERT(STREQUAL(lyrics.lines[2].text, lyrics.lines[2].text_len,
                     "世界"));

    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
lyrics_test_reject_empty(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsLoadResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    char text[] = "\r\n\n\t \n";
    int32 len;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lyrics_empty");
    len = snprintf2(path, SIZEOF(path), "%s/lyrics.txt", temp_dir);
    if ((len <= 0) || (len >= SIZEOF(path))) {
        test_remove_tree(temp_dir);
        fatal(lyrics_test_fail("empty path"));
    }
    if (!lyrics_test_write(path, text, strlen32(text))) {
        test_remove_tree(temp_dir);
        fatal(lyrics_test_fail("write empty text"));
    }

    if (lrc_lyrics_load_file(&lyrics, path, &result)) {
        lrc_lyrics_destroy(&lyrics);
        test_remove_tree(temp_dir);
        fatal(lyrics_test_fail("empty text accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_LYRICS_LOAD_EMPTY);
    ASSERT(lyrics.text == NULL);
    ASSERT(lyrics.line_count == 0);

    test_remove_tree(temp_dir);

    return;
}

static void
lyrics_test_reject_invalid_utf8(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsLoadResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    char text[] = {'o', 'k', '\n', (char)0xC0, '\0'};
    int32 len;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lyrics_invalid_utf8");
    len = snprintf2(path, SIZEOF(path), "%s/lyrics.txt", temp_dir);
    if ((len <= 0) || (len >= SIZEOF(path))) {
        test_remove_tree(temp_dir);
        fatal(lyrics_test_fail("invalid utf8 path"));
    }
    if (!lyrics_test_write(path, text, 4)) {
        test_remove_tree(temp_dir);
        fatal(lyrics_test_fail("write invalid utf8 text"));
    }

    if (lrc_lyrics_load_file(&lyrics, path, &result)) {
        lrc_lyrics_destroy(&lyrics);
        test_remove_tree(temp_dir);
        fatal(lyrics_test_fail("invalid utf8 accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_LYRICS_LOAD_INVALID_UTF8);
    ASSERT(result.byte_offset == 3);

    test_remove_tree(temp_dir);

    return;
}


static void
lyrics_test_normalize_punctuation_sections_and_mapping(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    char text[] = "  Hello, WORLD!!\n[Chorus]\nBang-bang   MAXWELL's\n";
    int32 bang_offset;
    char *bang;

    if (!lyrics_test_load_text(&lyrics, text, strlen32(text))) {
        fatal(lyrics_test_fail("load normalization text"));
    }

    if (!lrc_lyrics_normalize(&lyrics, &normalized)) {
        lrc_lyrics_destroy(&lyrics);
        fatal(lyrics_test_fail("normalize punctuation text"));
    }

    ASSERT_EQUAL(normalized.text, "hello world bang bang maxwell's");
    ASSERT(normalized.byte_count == normalized.text_len);
    ASSERT(normalized.alignable_line_count == 2);
    ASSERT(normalized.line_count == lyrics.line_count);
    lyrics_test_assert_line_range(&normalized, 0, 0, 11);
    lyrics_test_assert_no_line_range(
        &normalized,
        1,
        LRC_LYRICS_NORMALIZED_LINE_KIND_SECTION_MARKER
    );
    lyrics_test_assert_line_range(&normalized, 2, 12, 31);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 0) == 0);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 10) == 0);

    bang = memmem64(normalized.text,
                    normalized.text_len,
                    STRLIT("bang bang"));
    ASSERT(bang);
    bang_offset = (int32)(bang - normalized.text);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, bang_offset) == 2);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized,
                                         normalized.text_len - 1) == 2);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
lyrics_test_normalize_unicode_and_blank_lines(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    char text[] = "Olá,   世界!\n\n(Bridge)\nAgain\n";
    char *again;
    int32 again_offset;

    if (!lyrics_test_load_text(&lyrics, text, strlen32(text))) {
        fatal(lyrics_test_fail("load unicode normalization text"));
    }

    if (!lrc_lyrics_normalize(&lyrics, &normalized)) {
        lrc_lyrics_destroy(&lyrics);
        fatal(lyrics_test_fail("normalize unicode text"));
    }

    ASSERT_EQUAL(normalized.text, "ola shi jie again");
    ASSERT(normalized.alignable_line_count == 2);
    ASSERT(normalized.line_count == lyrics.line_count);
    lyrics_test_assert_line_range(&normalized, 0, 0, 11);
    lyrics_test_assert_no_line_range(
        &normalized,
        1,
        LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK
    );
    lyrics_test_assert_no_line_range(
        &normalized,
        2,
        LRC_LYRICS_NORMALIZED_LINE_KIND_SECTION_MARKER
    );
    lyrics_test_assert_line_range(&normalized, 3, 12, 17);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 0) == 0);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 3) == 0);

    again = memmem64(normalized.text,
                     normalized.text_len,
                     STRLIT("again"));
    ASSERT(again);
    again_offset = (int32)(again - normalized.text);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, again_offset) == 3);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
lyrics_test_normalized_ranges_blank_punctuation_repeated(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    char text[] = "Repeat\n\n!!! ???\nRepeat\n";

    if (!lyrics_test_load_text(&lyrics, text, strlen32(text))) {
        fatal(lyrics_test_fail("load repeated range text"));
    }

    if (!lrc_lyrics_normalize(&lyrics, &normalized)) {
        lrc_lyrics_destroy(&lyrics);
        fatal(lyrics_test_fail("normalize repeated range text"));
    }

    ASSERT_EQUAL(normalized.text, "repeat repeat");
    ASSERT(normalized.line_count == 4);
    ASSERT(normalized.alignable_line_count == 2);
    lyrics_test_assert_line_range(&normalized, 0, 0, 6);
    lyrics_test_assert_no_line_range(
        &normalized,
        1,
        LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK
    );
    lyrics_test_assert_no_line_range(
        &normalized,
        2,
        LRC_LYRICS_NORMALIZED_LINE_KIND_PUNCTUATION_ONLY
    );
    lyrics_test_assert_line_range(&normalized, 3, 7, 13);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 6) == 3);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 7) == 3);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
lyrics_test_preprocess_option_defaults_preserve_normalization(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized default_normalized = {0};
    LrcLyricsNormalized option_normalized = {0};
    LrcLyricsPreprocessOptions options;
    char text[] = "  Hello, WORLD!!\n[Chorus]\nBang-bang   MAXWELL's\n";

    if (!lyrics_test_load_text(&lyrics, text, strlen32(text))) {
        fatal(lyrics_test_fail("load options normalization text"));
    }

    lrc_lyrics_preprocess_options_init(&options);
    ASSERT(options.split_size == LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD);
    ASSERT(options.star_frequency
           == LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_EDGES);
    ASSERT(options.romanization == LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU);
    ASSERT_EQUAL(options.language, "eng");

    if (!lrc_lyrics_normalize(&lyrics, &default_normalized)) {
        lrc_lyrics_destroy(&lyrics);
        fatal(lyrics_test_fail("normalize through default wrapper"));
    }
    if (!lrc_lyrics_normalize_with_options(&lyrics,
                                           &option_normalized,
                                           &options)) {
        lrc_lyrics_normalized_destroy(&default_normalized);
        lrc_lyrics_destroy(&lyrics);
        fatal(lyrics_test_fail("normalize through explicit options"));
    }

    ASSERT_EQUAL(default_normalized.text, option_normalized.text);
    ASSERT(default_normalized.byte_count == option_normalized.byte_count);
    ASSERT(default_normalized.line_count == option_normalized.line_count);
    ASSERT(default_normalized.alignable_line_count
           == option_normalized.alignable_line_count);

    lrc_lyrics_normalized_destroy(&option_normalized);
    lrc_lyrics_normalized_destroy(&default_normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
lyrics_test_optional_maxwell_txt(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsLoadResult result;
    char *path;

    path = getenv("LRC_TEST_MAXWELL_TXT");
    if (path == NULL) {
        path = "next-phase/maxwell.txt";
    }
    if (!util_file_exists(path)) {
        return;
    }

    if (!lrc_lyrics_load_file(&lyrics, path, &result)) {
        fatal(lyrics_test_fail("load maxwell lyrics"));
    }

    ASSERT(lyrics.line_count == 6);
    ASSERT(lyrics.nonempty_line_count == 5);
    ASSERT(STREQUAL(lyrics.lines[0].text, lyrics.lines[0].text_len,
                     "Can I take you out to the pictures, Joan?"));
    ASSERT(lyrics.lines[3].text_len == 0);
    ASSERT(STREQUAL(lyrics.lines[4].text, lyrics.lines[4].text_len,
                     "Bang, bang, Maxwell's silver hammer"));
    ASSERT(STREQUAL(lyrics.lines[5].text, lyrics.lines[5].text_len,
                     "Came down upon her head"));

    {
        LrcLyricsNormalized normalized = {0};
        char *bang;
        int32 bang_offset;

        if (!lrc_lyrics_normalize(&lyrics, &normalized)) {
            lrc_lyrics_destroy(&lyrics);
            fatal(lyrics_test_fail("normalize maxwell lyrics"));
        }

        ASSERT_EQUAL(
            normalized.text,
            "can i take you out to the pictures joan "
            "but as shes getting ready to go "
            "a knock comes on the door "
            "bang bang maxwells silver hammer "
            "came down upon her head");
        ASSERT(normalized.alignable_line_count == 5);
        ASSERT(normalized.line_count == lyrics.line_count);
        lyrics_test_assert_line_range(&normalized, 0, 0, 39);
        lyrics_test_assert_line_range(&normalized, 1, 40, 71);
        lyrics_test_assert_line_range(&normalized, 2, 72, 97);
        lyrics_test_assert_no_line_range(
            &normalized,
            3,
            LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK
        );
        lyrics_test_assert_line_range(&normalized, 4, 98, 130);
        lyrics_test_assert_line_range(&normalized, 5, 131, 154);
        ASSERT(lrc_lyrics_normalized_line_at(&normalized, 0) == 0);

        bang = memmem64(normalized.text,
                        normalized.text_len,
                        STRLIT("bang bang"));
        ASSERT(bang);
        bang_offset = (int32)(bang - normalized.text);
        ASSERT(lrc_lyrics_normalized_line_at(&normalized, bang_offset) == 4);

        lrc_lyrics_normalized_destroy(&normalized);
    }

    lrc_lyrics_destroy(&lyrics);

    return;
}

int32
main(void) {
    lyrics_test_crlf_and_trailing_newline();
    lyrics_test_bom_unicode_and_blank_lines();
    lyrics_test_reject_empty();
    lyrics_test_reject_invalid_utf8();
    lyrics_test_normalize_punctuation_sections_and_mapping();
    lyrics_test_normalize_unicode_and_blank_lines();
    lyrics_test_normalized_ranges_blank_punctuation_repeated();
    lyrics_test_preprocess_option_defaults_preserve_normalization();
    lyrics_test_optional_maxwell_txt();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_lyrics */
