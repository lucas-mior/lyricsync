#include "cbase.h"
#include "lyricsync.h"
#include "lrc.h"

#if !defined(TESTING_lrc)
#define TESTING_lrc 0
#endif

static void
lrc_format_result_init(LrcFormatResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_init(&result->header);

    result->seconds = 0.0f;
    result->timestamp_hundredths = 0;

    return;
}

static void
lrc_format_result_set(
    LrcFormatResult *result,
    enum LsError error,
    char *message,
    float seconds,
    int32 timestamp_hundredths
) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_set(&result->header, error, message);

    result->seconds = seconds;
    result->timestamp_hundredths = timestamp_hundredths;

    return;
}

static void
lrc_write_result_init(LrcWriteResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_init(&result->path_header);
    result->format_error = LS_ERROR_NONE;

    result->line_index = -1;

    return;
}

static void
lrc_write_result_set(
    LrcWriteResult *result,
    enum LsError error,
    char *message,
    char *path,
    int32 line_index
) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_set(&result->path_header, error, message, path);

    result->line_index = line_index;

    return;
}

static int32
lrc_decimal_digit_count(int32 value) {
    int32 digits;

    ASSERT(value >= 0);

    digits = 1;
    while (value >= 10) {
        value /= 10;
        digits += 1;
    }

    return digits;
}

static int32
lrc_timestamp_formatted_len(int32 timestamp_hundredths) {
    int32 minutes;
    int32 minute_digits;

    ASSERT(timestamp_hundredths >= 0);

    minutes = timestamp_hundredths/6000;
    minute_digits = lrc_decimal_digit_count(minutes);
    if (minute_digits < 2) {
        minute_digits = 2;
    }

    return minute_digits + 8;
}

static bool
lrc_timestamp_hundredths_from_seconds(
    float seconds,
    int32 *timestamp_hundredths,
    LrcFormatResult *result
) {
    double rounded;

    if (result) {
        lrc_format_result_init(result);
    }
    if (timestamp_hundredths == NULL) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_ARGUMENT,
                              "timestamp destination is missing",
                              seconds,
                              -1);
        return false;
    }
    if (!isfinite(seconds) || (seconds < 0.0f)) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_TIMESTAMP,
                              "timestamp seconds are invalid",
                              seconds,
                              -1);
        return false;
    }

    rounded = (double)seconds*100.0 + 0.5;
    if (rounded > (double)INT32_MAX) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_TOO_LARGE,
                              "timestamp seconds are too large",
                              seconds,
                              -1);
        return false;
    }

    *timestamp_hundredths = (int32)rounded;

    return true;
}

static bool
lrc_format_timestamp_hundredths(
    int32 timestamp_hundredths,
    char *buffer,
    int32 buffer_len,
    int32 *formatted_len,
    LrcFormatResult *result
) {
    int32 minutes;
    int32 seconds;
    int32 hundredths;
    int32 len;

    if (result) {
        lrc_format_result_init(result);
    }
    if ((buffer == NULL) || (buffer_len <= 0)) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_ARGUMENT,
                              "timestamp buffer is missing",
                              0.0f,
                              timestamp_hundredths);
        return false;
    }
    if (timestamp_hundredths < 0) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_TIMESTAMP,
                              "timestamp hundredths are invalid",
                              0.0f,
                              timestamp_hundredths);
        return false;
    }

    len = lrc_timestamp_formatted_len(timestamp_hundredths);
    if (buffer_len <= len) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_TOO_LARGE,
                              "timestamp buffer is too small",
                              0.0f,
                              timestamp_hundredths);
        return false;
    }

    minutes = timestamp_hundredths/6000;
    seconds = (timestamp_hundredths/100)%60;
    hundredths = timestamp_hundredths%100;
    len = snprintf2(buffer,
                    buffer_len,
                    "[%02d:%02d.%02d]",
                    minutes,
                    seconds,
                    hundredths);

    if (formatted_len) {
        *formatted_len = len;
    }

    return true;
}

static bool
lrc_format_timestamped_line_hundredths(
    StrBuilder *builder,
    int32 timestamp_hundredths,
    char *text,
    int32 text_len,
    LrcFormatResult *result
) {
    char timestamp[32];
    int32 timestamp_len;

    if (result) {
        lrc_format_result_init(result);
    }
    if (builder == NULL) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_ARGUMENT,
                              "line destination is missing",
                              0.0f,
                              timestamp_hundredths);
        return false;
    }
    if ((text == NULL) && (text_len > 0)) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_ARGUMENT,
                              "line text is missing",
                              0.0f,
                              timestamp_hundredths);
        return false;
    }
    if (text_len < 0) {
        lrc_format_result_set(result,
                              LS_ERROR_FORMAT_INVALID_ARGUMENT,
                              "line text length is negative",
                              0.0f,
                              timestamp_hundredths);
        return false;
    }
    if (!lrc_format_timestamp_hundredths(timestamp_hundredths,
                                         timestamp,
                                         SIZEOF(timestamp),
                                         &timestamp_len,
                                         result)) {
        return false;
    }

    sb_append(builder, timestamp, timestamp_len);
    sb_append(builder, text, text_len);

    return true;
}

#if TESTING
static void
lrc_parse_result_init(LrcParseResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_init(&result->header);

    result->line_index = -1;
    result->byte_offset = -1;

    return;
}

static void
lrc_parse_result_set(
    LrcParseResult *result,
    enum LsError error,
    char *message,
    int32 line_index,
    int32 byte_offset
) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_set(&result->header, error, message);

    result->line_index = line_index;
    result->byte_offset = byte_offset;

    return;
}

static void
lrc_parsed_file_destroy(LrcParsedFile *parsed) {
    if (parsed == NULL) {
        return;
    }

    ARRAY_FREE(parsed->text);
    ARRAY_FREE(parsed->lines);

    memset64(parsed, 0, SIZEOF(*parsed));

    return;
}

static bool
lrc_is_digit(char c) {
    if ((c >= '0') && (c <= '9')) {
        return true;
    }

    return false;
}

static bool
lrc_is_blank_line(char *text, int32 text_len) {
    for (int32 i = 0; i < text_len; i += 1) {
        if ((text[i] != ' ') && (text[i] != '\t')) {
            return false;
        }
    }

    return true;
}

static bool
lrc_parse_two_digits(char *text, int32 index, int32 text_len, int32 *out) {
    if ((index + 1) >= text_len) {
        return false;
    }
    if (!lrc_is_digit(text[index]) || !lrc_is_digit(text[index + 1])) {
        return false;
    }

    *out = (text[index] - '0')*10 + (text[index + 1] - '0');

    return true;
}

static bool
lrc_parse_timestamp(
    char *text,
    int32 text_len,
    int32 *timestamp_hundredths,
    int32 *end_offset
) {
    int32 minutes;
    int32 seconds;
    int32 hundredths;
    int32 i;

    if ((text_len < 10) || (text[0] != '[')) {
        return false;
    }

    minutes = 0;
    i = 1;
    if ((i >= text_len) || !lrc_is_digit(text[i])) {
        return false;
    }
    while ((i < text_len) && lrc_is_digit(text[i])) {
        int32 digit = text[i] - '0';

        if (minutes > (INT32_MAX - digit)/10) {
            return false;
        }
        minutes = minutes*10 + digit;
        i += 1;
    }
    if ((i >= text_len) || (text[i] != ':')) {
        return false;
    }
    i += 1;

    if (!lrc_parse_two_digits(text, i, text_len, &seconds)) {
        return false;
    }
    if (seconds >= 60) {
        return false;
    }
    i += 2;

    if ((i >= text_len) || (text[i] != '.')) {
        return false;
    }
    i += 1;

    if (!lrc_parse_two_digits(text, i, text_len, &hundredths)) {
        return false;
    }
    i += 2;

    if ((i >= text_len) || (text[i] != ']')) {
        return false;
    }
    i += 1;

    if (minutes > (INT32_MAX/100 - seconds)/60) {
        return false;
    }

    *timestamp_hundredths = (minutes*60 + seconds)*100 + hundredths;
    *end_offset = i;

    return true;
}

static bool
lrc_parsed_file_reserve_lines(LrcParsedFile *parsed, int32 extra) {
    int64 needed;

    if (extra <= 0) {
        return true;
    }

    needed = (int64)parsed->line_count + extra;
    if (needed > INT32_MAX) {
        return false;
    }

    return ARRAY_RESERVE(parsed->lines, (int32)needed);
}

static bool
lrc_parsed_file_append_line(
    LrcParsedFile *parsed,
    enum LrcParsedLineKind kind,
    int32 source_line_index,
    int32 timestamp_hundredths,
    int32 text_start,
    int32 text_len
) {
    LrcParsedLine *line;

    if (!lrc_parsed_file_reserve_lines(parsed, 1)) {
        return false;
    }

    line = &parsed->lines[parsed->line_count];
    parsed->line_count += 1;
    ARRAY_SET_COUNT(parsed->lines, parsed->line_count);

    line->text = parsed->text + text_start;
    line->text_len = text_len;
    line->source_line_index = source_line_index;
    line->timestamp_hundredths = timestamp_hundredths;
    line->timestamp_seconds = (float)timestamp_hundredths/100.0f;
    line->kind = kind;

    if (kind == LRC_PARSED_LINE_KIND_TIMESTAMPED) {
        parsed->timestamped_line_count += 1;
    } else {
        parsed->blank_line_count += 1;
    }

    return true;
}

static bool
lrc_parse_line(
    LrcParsedFile *parsed,
    int32 source_line_index,
    int32 line_start,
    int32 line_end,
    LrcParseResult *result
) {
    char *line_text = parsed->text + line_start;
    int32 line_len = line_end - line_start;
    int32 timestamp_hundredths;
    int32 timestamp_end;

    if ((line_len > 0) && (line_text[line_len - 1] == '\r')) {
        line_len -= 1;
        parsed->text[line_start + line_len] = '\0';
    }

    if (line_len <= 0) {
        return lrc_parsed_file_append_line(parsed,
                                           LRC_PARSED_LINE_KIND_BLANK,
                                           source_line_index,
                                           -1,
                                           line_start,
                                           0);
    }
    if (lrc_is_blank_line(line_text, line_len)) {
        return lrc_parsed_file_append_line(parsed,
                                           LRC_PARSED_LINE_KIND_BLANK,
                                           source_line_index,
                                           -1,
                                           line_start,
                                           line_len);
    }
    if (line_text[0] != '[') {
        lrc_parse_result_set(result,
                             LS_ERROR_PARSE_UNTIMED_TEXT,
                             "LRC line is missing timestamp",
                             source_line_index,
                             line_start);
        return false;
    }
    if (!lrc_parse_timestamp(line_text,
                             line_len,
                             &timestamp_hundredths,
                             &timestamp_end)) {
        lrc_parse_result_set(result,
                             LS_ERROR_PARSE_MALFORMED_TIMESTAMP,
                             "LRC timestamp is malformed",
                             source_line_index,
                             line_start);
        return false;
    }

    return lrc_parsed_file_append_line(parsed,
                                       LRC_PARSED_LINE_KIND_TIMESTAMPED,
                                       source_line_index,
                                       timestamp_hundredths,
                                       line_start + timestamp_end,
                                       line_len - timestamp_end);
}

static bool
lrc_parsed_file_copy_text(
    LrcParsedFile *parsed,
    char *text,
    int32 text_len,
    LrcParseResult *result
) {
    if (text_len < 0) {
        lrc_parse_result_set(result,
                             LS_ERROR_PARSE_INVALID_ARGUMENT,
                             "LRC text length is negative",
                             -1,
                             -1);
        return false;
    }
    if (text_len >= INT32_MAX) {
        lrc_parse_result_set(result,
                             LS_ERROR_PARSE_TOO_LARGE,
                             "LRC text is too large",
                             -1,
                             -1);
        return false;
    }

    ARRAY_INIT_COUNT(parsed->text, text_len + 1);
    if (text_len > 0) {
        memcpy64(parsed->text, text, text_len);
    }
    parsed->text[text_len] = '\0';
    parsed->text_len = text_len;

    return true;
}

static bool
lrc_parse_text(
    LrcParsedFile *parsed,
    char *text,
    int32 text_len,
    LrcParseResult *result
) {
    int32 line_start;
    int32 line_index;

    if (result) {
        lrc_parse_result_init(result);
    }
    if (parsed == NULL) {
        lrc_parse_result_set(result,
                             LS_ERROR_PARSE_INVALID_ARGUMENT,
                             "LRC parsed-file destination is missing",
                             -1,
                             -1);
        return false;
    }
    if ((text == NULL) && (text_len > 0)) {
        lrc_parse_result_set(result,
                             LS_ERROR_PARSE_INVALID_ARGUMENT,
                             "LRC text is missing",
                             -1,
                             -1);
        return false;
    }

    lrc_parsed_file_destroy(parsed);
    if (!lrc_parsed_file_copy_text(parsed, text, text_len, result)) {
        return false;
    }

    line_start = 0;
    line_index = 0;
    for (int32 i = 0; i < parsed->text_len; i += 1) {
        if (parsed->text[i] == '\n') {
            if (!lrc_parse_line(parsed,
                                line_index,
                                line_start,
                                i,
                                result)) {
                lrc_parsed_file_destroy(parsed);
                return false;
            }
            line_index += 1;
            line_start = i + 1;
        }
    }

    if ((line_start < parsed->text_len) || (parsed->text_len == 0)) {
        if (!lrc_parse_line(parsed,
                            line_index,
                            line_start,
                            parsed->text_len,
                            result)) {
            lrc_parsed_file_destroy(parsed);
            return false;
        }
    }

    return true;
}
#endif

static bool
lrc_output_line_validate(
    LrcOutputLine *line,
    int32 line_index,
    LrcWriteResult *result
) {
    if (line == NULL) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output line is missing",
                             NULL,
                             line_index);
        return false;
    }
    if (line->text_len < 0) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_LINE,
                             "LRC output line text length is negative",
                             NULL,
                             line_index);
        return false;
    }
    if ((line->text == NULL) && (line->text_len > 0)) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_LINE,
                             "LRC output line text is missing",
                             NULL,
                             line_index);
        return false;
    }

    switch (line->kind) {
    case LRC_OUTPUT_LINE_KIND_TIMESTAMPED:
        if (line->timestamp_hundredths < 0) {
            lrc_write_result_set(result,
                                 LS_ERROR_WRITE_INVALID_LINE,
                                 "LRC output timestamp is invalid",
                                 NULL,
                                 line_index);
            return false;
        }
        break;
    case LRC_OUTPUT_LINE_KIND_BLANK:
        break;
    default:
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_LINE,
                             "LRC output line kind is invalid",
                             NULL,
                             line_index);
        return false;
    }

    return true;
}

static bool
lrc_format_output_lines(
    StrBuilder *builder,
    LrcOutputLine *lines,
    int32 line_count,
    LrcWriteResult *result
) {
    if (result) {
        lrc_write_result_init(result);
    }
    if (builder == NULL) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output builder is missing",
                             NULL,
                             -1);
        return false;
    }
    if (line_count < 0) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output line count is negative",
                             NULL,
                             -1);
        return false;
    }
    if ((lines == NULL) && (line_count > 0)) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output lines are missing",
                             NULL,
                             -1);
        return false;
    }

    for (int32 i = 0; i < line_count; i += 1) {
        LrcOutputLine *line = lines + i;

        if (!lrc_output_line_validate(line, i, result)) {
            return false;
        }

        if (line->kind == LRC_OUTPUT_LINE_KIND_TIMESTAMPED) {
            LrcFormatResult format_result;

            if (!lrc_format_timestamped_line_hundredths(
                builder,
                line->timestamp_hundredths,
                line->text,
                line->text_len,
                &format_result
            )) {
                lrc_write_result_set(result,
                                     LS_ERROR_WRITE_FORMAT_FAILED,
                                     "LRC output line formatting failed",
                                     NULL,
                                     i);
                if (result) {
                    result->format_error = format_result.header.error;
                }
                return false;
            }
        } else {
            sb_append(builder, line->text, line->text_len);
        }
        sb_append_byte(builder, '\n');
    }

    return true;
}

static bool
lrc_make_temp_output_path(char *path, char *buffer, int32 buffer_len) {
    int32 len;

    len = snprintf2(buffer,
                    buffer_len,
                    "%s.tmp.%lld.XXXXXX",
                    path,
                    (int64)getpid());
    if ((len <= 0) || (len >= buffer_len)) {
        return false;
    }

    return true;
}

static bool
lrc_write_all_fd(int32 fd, char *text, int32 text_len) {
    int32 written = 0;

    while (written < text_len) {
        int64 n;

        n = write64(fd, text + written, text_len - written);
        if (n <= 0) {
            return false;
        }
        if (n > ((int64)text_len - written)) {
            return false;
        }
        written += (int32)n;
    }

    return true;
}

static bool
lrc_write_text_file_atomic(
    char *path,
    char *text,
    int32 text_len,
    LrcWriteResult *result
) {
    char temp_path[PATH_MAX];
    int32 fd;

    if (path == NULL) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output path is missing",
                             path,
                             -1);
        return false;
    }
    if (path[0] == '\0') {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output path is empty",
                             path,
                             -1);
        return false;
    }
    if ((text == NULL) && (text_len > 0)) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output text is missing",
                             path,
                             -1);
        return false;
    }
    if (text_len < 0) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output text length is negative",
                             path,
                             -1);
        return false;
    }
    if (!lrc_make_temp_output_path(path, temp_path, SIZEOF(temp_path))) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_TEMP_PATH_TOO_LONG,
                             "LRC temporary output path is too long",
                             path,
                             -1);
        return false;
    }

    fd = mkstemp(temp_path);
    if (fd < 0) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_TEMP_OPEN_FAILED,
                             "could not open temporary LRC output",
                             path,
                             -1);
        return false;
    }

    if (!lrc_write_all_fd(fd, text, text_len)) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_WRITE_FAILED,
                             "could not write temporary LRC output",
                             path,
                             -1);
        close(fd);
        unlink(temp_path);
        return false;
    }

    if (close(fd) < 0) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_CLOSE_FAILED,
                             "could not close temporary LRC output",
                             path,
                             -1);
        unlink(temp_path);
        return false;
    }

    if (rename(temp_path, path) < 0) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_RENAME_FAILED,
                             "could not rename temporary LRC output",
                             path,
                             -1);
        unlink(temp_path);
        return false;
    }

    return true;
}

static bool
lrc_write_output_file(
    char *path,
    LrcOutputLine *lines,
    int32 line_count,
    LrcWriteResult *result
) {
    StrBuilder builder;

    if (result) {
        lrc_write_result_init(result);
    }
    if (path == NULL) {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output path is missing",
                             path,
                             -1);
        return false;
    }
    if (path[0] == '\0') {
        lrc_write_result_set(result,
                             LS_ERROR_WRITE_INVALID_ARGUMENT,
                             "LRC output path is empty",
                             path,
                             -1);
        return false;
    }

    builder = (StrBuilder){0};
    if (!lrc_format_output_lines(&builder, lines, line_count, result)) {
        sb_free(&builder);
        return false;
    }
    if (!lrc_write_text_file_atomic(path,
                                    builder.data,
                                    builder.len,
                                    result)) {
        sb_free(&builder);
        return false;
    }

    sb_free(&builder);

    return true;
}

#if TESTING_lrc
#define CBASE_IMPLEMENT
#include "cbase.h"

static bool
lrc_format_timestamp_seconds(
    float seconds,
    char *buffer,
    int32 buffer_len,
    int32 *formatted_len,
    LrcFormatResult *result
) {
    int32 timestamp_hundredths;

    if (result) {
        lrc_format_result_init(result);
    }
    if (!lrc_timestamp_hundredths_from_seconds(seconds,
                                               &timestamp_hundredths,
                                               result)) {
        return false;
    }

    return lrc_format_timestamp_hundredths(timestamp_hundredths,
                                           buffer,
                                           buffer_len,
                                           formatted_len,
                                           result);
}

static bool
lrc_format_timestamped_line(
    StrBuilder *builder,
    float seconds,
    char *text,
    int32 text_len,
    LrcFormatResult *result
) {
    int32 timestamp_hundredths;

    if (result) {
        lrc_format_result_init(result);
    }
    if (!lrc_timestamp_hundredths_from_seconds(seconds,
                                               &timestamp_hundredths,
                                               result)) {
        return false;
    }

    return lrc_format_timestamped_line_hundredths(builder,
                                                  timestamp_hundredths,
                                                  text,
                                                  text_len,
                                                  result);
}

static int32
lrc_test_fail(char *name) {
    error2("lrc test failed: %s\n", name);

    return 1;
}

static void
lrc_test_assert_line(
    LrcParsedFile *parsed,
    int32 line_index,
    enum LrcParsedLineKind expected_kind,
    int32 expected_hundredths,
    char *expected_text,
    int32 expected_text_len
) {
    LrcParsedLine *line;

    ASSERT(line_index >= 0);
    ASSERT(line_index < parsed->line_count);

    line = &parsed->lines[line_index];
    ASSERT(line->kind == expected_kind);
    ASSERT(line->timestamp_hundredths == expected_hundredths);
    ASSERT(STREQUAL(line->text,
                     line->text_len,
                     expected_text,
                     expected_text_len));

    return;
}

static void
lrc_test_parse_timestamped_and_blank_lines(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult result;
    char text[] = "[00:00.20]Hello\n\n[01:02.34]World\n";

    if (!lrc_parse_text(&parsed, text, strlen32(text), &result)) {
        fatal(lrc_test_fail("parse timestamped and blank lines"));
    }

    ASSERT(parsed.line_count == 3);
    ASSERT(parsed.timestamped_line_count == 2);
    ASSERT(parsed.blank_line_count == 1);
    lrc_test_assert_line(&parsed,
                         0,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         20,
                         STRLIT("Hello"));
    lrc_test_assert_line(&parsed,
                         1,
                         LRC_PARSED_LINE_KIND_BLANK,
                         -1,
                         STRLIT(""));
    lrc_test_assert_line(&parsed,
                         2,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         6234,
                         STRLIT("World"));
    ASSERT(parsed.lines[2].timestamp_seconds == 62.34f);

    lrc_parsed_file_destroy(&parsed);

    return;
}

static void
lrc_test_parse_crlf_and_space_blank_line(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult result;
    char text[] = "[00:01.00]One\r\n  \t\r\n[00:02.00]Two\r\n";

    if (!lrc_parse_text(&parsed, text, strlen32(text), &result)) {
        fatal(lrc_test_fail("parse crlf and blank line"));
    }

    ASSERT(parsed.line_count == 3);
    ASSERT(parsed.timestamped_line_count == 2);
    ASSERT(parsed.blank_line_count == 1);
    lrc_test_assert_line(&parsed,
                         0,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         100,
                         STRLIT("One"));
    lrc_test_assert_line(&parsed,
                         1,
                         LRC_PARSED_LINE_KIND_BLANK,
                         -1,
                         STRLIT("  \t"));
    lrc_test_assert_line(&parsed,
                         2,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         200,
                         STRLIT("Two"));

    lrc_parsed_file_destroy(&parsed);

    return;
}

static void
lrc_test_reject_malformed_timestamps(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult result;
    char bad_seconds[] = "[00:60.00]Bad\n";
    char bad_fraction[] = "[00:00.1]Bad\n";

    if (lrc_parse_text(&parsed,
                       bad_seconds,
                       strlen32(bad_seconds),
                       &result)) {
        lrc_parsed_file_destroy(&parsed);
        fatal(lrc_test_fail("accepted bad seconds"));
    }
    ASSERT(result.header.error == LS_ERROR_PARSE_MALFORMED_TIMESTAMP);
    ASSERT(result.line_index == 0);

    memset64(&parsed, 0, SIZEOF(parsed));
    if (lrc_parse_text(&parsed,
                       bad_fraction,
                       strlen32(bad_fraction),
                       &result)) {
        lrc_parsed_file_destroy(&parsed);
        fatal(lrc_test_fail("accepted bad fraction"));
    }
    ASSERT(result.header.error == LS_ERROR_PARSE_MALFORMED_TIMESTAMP);
    ASSERT(result.line_index == 0);

    return;
}

static void
lrc_test_reject_untimed_text(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult result;
    char text[] = "[00:01.00]Good\nUntimed lyric\n";

    if (lrc_parse_text(&parsed, text, strlen32(text), &result)) {
        lrc_parsed_file_destroy(&parsed);
        fatal(lrc_test_fail("accepted untimed text"));
    }
    ASSERT(result.header.error == LS_ERROR_PARSE_UNTIMED_TEXT);
    ASSERT(result.line_index == 1);

    return;
}

static void
lrc_test_duplicate_timestamps_are_preserved(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult result;
    char text[] = "[00:01.00]A\n[00:01.00]B\n";

    if (!lrc_parse_text(&parsed, text, strlen32(text), &result)) {
        fatal(lrc_test_fail("parse duplicate timestamps"));
    }

    ASSERT(parsed.line_count == 2);
    ASSERT(parsed.timestamped_line_count == 2);
    lrc_test_assert_line(&parsed,
                         0,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         100,
                         STRLIT("A"));
    lrc_test_assert_line(&parsed,
                         1,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         100,
                         STRLIT("B"));

    lrc_parsed_file_destroy(&parsed);

    return;
}

static void
lrc_test_assert_timestamp(
    int32 timestamp_hundredths,
    char *expected,
    int32 expected_len
) {
    LrcFormatResult result;
    char buffer[32];
    int32 len;

    if (!lrc_format_timestamp_hundredths(timestamp_hundredths,
                                         buffer,
                                         SIZEOF(buffer),
                                         &len,
                                         &result)) {
        ASSERT(false);
    }

    ASSERT(len == expected_len);
    ASSERT_EQUAL(&buffer[0], expected);

    return;
}

static void
lrc_test_format_timestamp_hundredths(void) {
    lrc_test_assert_timestamp(0, STRLIT("[00:00.00]"));
    lrc_test_assert_timestamp(1, STRLIT("[00:00.01]"));
    lrc_test_assert_timestamp(5999, STRLIT("[00:59.99]"));
    lrc_test_assert_timestamp(6000, STRLIT("[01:00.00]"));
    lrc_test_assert_timestamp(6234, STRLIT("[01:02.34]"));
    lrc_test_assert_timestamp(360000, STRLIT("[60:00.00]"));

    return;
}

static void
lrc_test_format_timestamp_seconds_rounding(void) {
    LrcFormatResult result;
    char buffer[32];
    int32 hundredths;

    if (!lrc_timestamp_hundredths_from_seconds(1.231f,
                                               &hundredths,
                                               &result)) {
        fatal(lrc_test_fail("round seconds down"));
    }
    ASSERT(hundredths == 123);

    if (!lrc_format_timestamp_seconds(1.236f,
                                      buffer,
                                      SIZEOF(buffer),
                                      NULL,
                                      &result)) {
        fatal(lrc_test_fail("round seconds up"));
    }
    ASSERT(strequal(buffer, "[00:01.24]"));

    if (!lrc_format_timestamp_seconds(59.996f,
                                      buffer,
                                      SIZEOF(buffer),
                                      NULL,
                                      &result)) {
        fatal(lrc_test_fail("round across minute boundary"));
    }
    ASSERT(strequal(buffer, "[01:00.00]"));

    if (!lrc_format_timestamp_seconds(3600.0f,
                                      buffer,
                                      SIZEOF(buffer),
                                      NULL,
                                      &result)) {
        fatal(lrc_test_fail("format hour length timestamp"));
    }
    ASSERT(strequal(buffer, "[60:00.00]"));

    return;
}

static void
lrc_test_format_timestamped_line_preserves_text(void) {
    LrcFormatResult result;
    StrBuilder builder;
    char text[] = "Bang, bang, Café's hammer!";

    builder = (StrBuilder){0};
    if (!lrc_format_timestamped_line(&builder,
                                      14.14f,
                                      text,
                                      strlen32(text),
                                      &result)) {
        fatal(lrc_test_fail("format timestamped line"));
    }
    ASSERT(strequal(builder.data, "[00:14.14]Bang, bang, Café's hammer!"));

    sb_free(&builder);

    return;
}

static void
lrc_test_format_timestamped_empty_line(void) {
    LrcFormatResult result;
    StrBuilder builder;

    builder = (StrBuilder){0};
    if (!lrc_format_timestamped_line(&builder,
                                      3.40f,
                                      NULL,
                                      0,
                                      &result)) {
        fatal(lrc_test_fail("format timestamped empty line"));
    }
    ASSERT(strequal(builder.data, "[00:03.40]"));

    sb_free(&builder);

    return;
}

static void
lrc_test_format_reject_bad_inputs(void) {
    LrcFormatResult result;
    StrBuilder builder;
    char buffer[4];
    int32 hundredths;

    builder = (StrBuilder){0};

    if (lrc_timestamp_hundredths_from_seconds(-0.01f,
                                              &hundredths,
                                              &result)) {
        fatal(lrc_test_fail("accepted negative seconds"));
    }
    ASSERT(result.header.error == LS_ERROR_FORMAT_INVALID_TIMESTAMP);

    if (lrc_timestamp_hundredths_from_seconds(INFINITY,
                                              &hundredths,
                                              &result)) {
        fatal(lrc_test_fail("accepted infinite seconds"));
    }
    ASSERT(result.header.error == LS_ERROR_FORMAT_INVALID_TIMESTAMP);

    if (lrc_format_timestamp_hundredths(-1,
                                        buffer,
                                        SIZEOF(buffer),
                                        NULL,
                                        &result)) {
        fatal(lrc_test_fail("accepted negative hundredths"));
    }
    ASSERT(result.header.error == LS_ERROR_FORMAT_INVALID_TIMESTAMP);

    if (lrc_format_timestamp_hundredths(0,
                                        buffer,
                                        SIZEOF(buffer),
                                        NULL,
                                        &result)) {
        fatal(lrc_test_fail("accepted small buffer"));
    }
    ASSERT(result.header.error == LS_ERROR_FORMAT_TOO_LARGE);

    if (lrc_format_timestamped_line_hundredths(&builder,
                                               0,
                                               NULL,
                                               1,
                                               &result)) {
        fatal(lrc_test_fail("accepted missing line text"));
    }
    ASSERT(result.header.error == LS_ERROR_FORMAT_INVALID_ARGUMENT);

    sb_free(&builder);

    return;
}

static void
lrc_test_set_output_line(
    LrcOutputLine *line,
    enum LrcOutputLineKind kind,
    int32 timestamp_hundredths,
    char *text,
    int32 text_len
) {
    line->text = text;
    line->text_len = text_len;
    line->timestamp_hundredths = timestamp_hundredths;
    line->kind = kind;

    return;
}


static void
lrc_test_write_generated_file(void) {
    LrcOutputLine lines[3];
    LrcWriteResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    char first[] = "Olá café";
    char blank[] = "";
    char second[] = "World";
    char expected[] = "[00:00.00]Olá café\n\n[00:01.23]World\n";
    char *text;
    int32 text_len;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lrc_write");
    test_join_path(path, SIZEOF(path), temp_dir, "out.lrc");

    lrc_test_set_output_line(lines + 0,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             0,
                             first,
                             strlen32(first));
    lrc_test_set_output_line(lines + 1,
                             LRC_OUTPUT_LINE_KIND_BLANK,
                             -1,
                             blank,
                             strlen32(blank));
    lrc_test_set_output_line(lines + 2,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             123,
                             second,
                             strlen32(second));

    if (!lrc_write_output_file(path, lines, 3, &result)) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("write generated lrc file"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_NONE);
    ASSERT(util_file_exists(path));

    if ((text_len = read_entire_file(path, &text)) < 0) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("read generated lrc file"));
    }
    ASSERT(text_len == strlen32(expected));
    ASSERT_EQUAL(text, &expected[0]);

    free2(text, ((int64)text_len + 1)*SIZEOF(*text));
    test_remove_tree(temp_dir);

    return;
}

static void
lrc_test_write_timestamped_empty_line(void) {
    LrcOutputLine lines[3];
    LrcWriteResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    char first[] = "First";
    char empty[] = "";
    char second[] = "Second";
    char expected[] = "[00:01.00]First\n"
                      "[00:03.40]\n"
                      "[00:07.58]Second\n";
    char *text;
    int32 text_len;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lrc_empty_line");
    test_join_path(path, SIZEOF(path), temp_dir, "out.lrc");

    lrc_test_set_output_line(lines + 0,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             100,
                             first,
                             strlen32(first));
    lrc_test_set_output_line(lines + 1,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             340,
                             empty,
                             strlen32(empty));
    lrc_test_set_output_line(lines + 2,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             758,
                             second,
                             strlen32(second));

    if (!lrc_write_output_file(path, lines, 3, &result)) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("write timestamped empty lrc line"));
    }

    if ((text_len = read_entire_file(path, &text)) < 0) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("read timestamped empty lrc line"));
    }
    ASSERT(text_len == strlen32(expected));
    ASSERT_EQUAL(text, &expected[0]);

    free2(text, ((int64)text_len + 1)*SIZEOF(*text));
    test_remove_tree(temp_dir);

    return;
}

static void
lrc_test_write_overwrites_existing_file(void) {
    LrcOutputLine first_lines[1];
    LrcOutputLine second_lines[1];
    LrcWriteResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    char first[] = "First";
    char second[] = "Second";
    char expected[] = "[00:02.00]Second\n";
    char *text;
    int32 text_len;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lrc_overwrite");
    test_join_path(path, SIZEOF(path), temp_dir, "out.lrc");

    lrc_test_set_output_line(first_lines + 0,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             100,
                             first,
                             strlen32(first));
    lrc_test_set_output_line(second_lines + 0,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             200,
                             second,
                             strlen32(second));

    if (!lrc_write_output_file(path,
                               first_lines,
                               1,
                               &result)) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("write first lrc file"));
    }
    if (!lrc_write_output_file(path,
                               second_lines,
                               1,
                               &result)) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("overwrite lrc file"));
    }

    if ((text_len = read_entire_file(path, &text)) < 0) {
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("read overwritten lrc file"));
    }
    ASSERT(text_len == strlen32(expected));
    ASSERT_EQUAL(text, &expected[0]);

    free2(text, ((int64)text_len + 1)*SIZEOF(*text));
    test_remove_tree(temp_dir);

    return;
}

static void
lrc_test_write_rejects_bad_inputs(void) {
    LrcOutputLine lines[1];
    LrcWriteResult result;
    char text[] = "Bad";

    lrc_test_set_output_line(lines + 0,
                             LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
                             -1,
                             text,
                             strlen32(text));

    if (lrc_write_output_file(NULL, lines, 1, &result)) {
        fatal(lrc_test_fail("accepted missing output path"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_WRITE_INVALID_ARGUMENT);

    if (lrc_write_output_file("", lines, 1, &result)) {
        fatal(lrc_test_fail("accepted empty output path"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_WRITE_INVALID_ARGUMENT);

    if (lrc_write_output_file("bad.lrc", NULL, 1, &result)) {
        fatal(lrc_test_fail("accepted missing output lines"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_WRITE_INVALID_ARGUMENT);

    if (lrc_write_output_file("bad.lrc", lines, -1, &result)) {
        fatal(lrc_test_fail("accepted negative output line count"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_WRITE_INVALID_ARGUMENT);

    if (lrc_write_output_file("bad.lrc", lines, 1, &result)) {
        fatal(lrc_test_fail("accepted negative output timestamp"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_WRITE_INVALID_LINE);
    ASSERT(result.line_index == 0);

    lines[0].timestamp_hundredths = 0;
    lines[0].text = NULL;
    lines[0].text_len = 1;
    if (lrc_write_output_file("bad.lrc", lines, 1, &result)) {
        fatal(lrc_test_fail("accepted missing output line text"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_WRITE_INVALID_LINE);

    return;
}

static void
lrc_test_optional_maxwell_formatting(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult parse_result;
    LrcFormatResult format_result;
    StrBuilder builder;
    char *path;
    char *text;
    int32 text_len;

    path = getenv("LRC_TEST_MAXWELL_LRC");
    if (path == NULL) {
        path = "next-phase/maxwell.lrc";
    }
    if (!util_file_exists(path)) {
        return;
    }

    if ((text_len = read_entire_file(path, &text)) < 0) {
        fatal(lrc_test_fail("read maxwell lrc before formatting"));
    }
    if (!lrc_parse_text(&parsed, text, text_len, &parse_result)) {
        free2(text, ((int64)text_len + 1)*SIZEOF(*text));
        fatal(lrc_test_fail("parse maxwell lrc before formatting"));
    }

    builder = (StrBuilder){0};
    for (int32 i = 0; i < parsed.line_count; i += 1) {
        LrcParsedLine *line = parsed.lines + i;

        if (line->kind == LRC_PARSED_LINE_KIND_TIMESTAMPED) {
            if (!lrc_format_timestamped_line_hundredths(
                &builder,
                line->timestamp_hundredths,
                line->text,
                line->text_len,
                &format_result
            )) {
                lrc_parsed_file_destroy(&parsed);
                sb_free(&builder);
                free2(text, ((int64)text_len + 1)*SIZEOF(*text));
                fatal(lrc_test_fail("format maxwell lrc line"));
            }
        } else {
            sb_append(&builder, line->text, line->text_len);
        }
        sb_append_byte(&builder, '\n');
    }

    ASSERT(builder.len == text_len);
    ASSERT_EQUAL(builder.data, text);

    lrc_parsed_file_destroy(&parsed);
    sb_free(&builder);
    free2(text, ((int64)text_len + 1)*SIZEOF(*text));

    return;
}

static void
lrc_test_assert_same_parsed_structure(
    LrcParsedFile *a,
    LrcParsedFile *b
) {
    ASSERT(a->line_count == b->line_count);
    ASSERT(a->timestamped_line_count == b->timestamped_line_count);
    ASSERT(a->blank_line_count == b->blank_line_count);

    for (int32 i = 0; i < a->line_count; i += 1) {
        ASSERT(a->lines[i].kind == b->lines[i].kind);
        ASSERT(a->lines[i].timestamp_hundredths
               == b->lines[i].timestamp_hundredths);
        ASSERT(STREQUAL(a->lines[i].text,
                         a->lines[i].text_len,
                         b->lines[i].text,
                         b->lines[i].text_len));
    }

    return;
}

static void
lrc_test_output_lines_from_parsed(
    LrcParsedFile *parsed,
    LrcOutputLine *lines
) {
    for (int32 i = 0; i < parsed->line_count; i += 1) {
        LrcParsedLine *parsed_line = parsed->lines + i;
        enum LrcOutputLineKind kind;

        if (parsed_line->kind == LRC_PARSED_LINE_KIND_TIMESTAMPED) {
            kind = LRC_OUTPUT_LINE_KIND_TIMESTAMPED;
        } else {
            kind = LRC_OUTPUT_LINE_KIND_BLANK;
        }
        lrc_test_set_output_line(lines + i,
                                 kind,
                                 parsed_line->timestamp_hundredths,
                                 parsed_line->text,
                                 parsed_line->text_len);
    }

    return;
}

static void
lrc_test_optional_maxwell_write_structure(void) {
    LrcParsedFile parsed = {0};
    LrcParsedFile reparsed = {0};
    LrcParseResult parse_result;
    LrcWriteResult write_result;
    LrcOutputLine *lines;
    char temp_dir[PATH_MAX];
    char out_path[PATH_MAX];
    char *fixture_path;
    char *fixture_text;
    char *written_text;
    int32 fixture_text_len;
    int32 written_text_len;
    int32 line_count;

    fixture_path = getenv("LRC_TEST_MAXWELL_LRC");
    if (fixture_path == NULL) {
        fixture_path = "next-phase/maxwell.lrc";
    }
    if (!util_file_exists(fixture_path)) {
        return;
    }

    if ((fixture_text_len = read_entire_file(fixture_path,
                                             &fixture_text)) < 0) {
        fatal(lrc_test_fail("read maxwell lrc before write"));
    }
    if (!lrc_parse_text(&parsed,
                        fixture_text,
                        fixture_text_len,
                        &parse_result)) {
        free2(fixture_text,
              ((int64)fixture_text_len + 1)*SIZEOF(*fixture_text));
        fatal(lrc_test_fail("parse maxwell lrc before write"));
    }

    line_count = parsed.line_count;
    lines = malloc2(line_count*SIZEOF(*lines));
    lrc_test_output_lines_from_parsed(&parsed, lines);

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "lrc_maxwell_write");
    test_join_path(out_path, SIZEOF(out_path), temp_dir, "out.lrc");
    if (!lrc_write_output_file(out_path,
                               lines,
                               parsed.line_count,
                               &write_result)) {
        free2(lines, line_count*SIZEOF(*lines));
        lrc_parsed_file_destroy(&parsed);
        free2(fixture_text,
              ((int64)fixture_text_len + 1)*SIZEOF(*fixture_text));
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("write maxwell lrc structure"));
    }

    if ((written_text_len = read_entire_file(out_path, &written_text)) < 0) {
        free2(lines, line_count*SIZEOF(*lines));
        lrc_parsed_file_destroy(&parsed);
        free2(fixture_text,
              ((int64)fixture_text_len + 1)*SIZEOF(*fixture_text));
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("read written maxwell lrc"));
    }
    if (!lrc_parse_text(&reparsed,
                        written_text,
                        written_text_len,
                        &parse_result)) {
        free2(lines, line_count*SIZEOF(*lines));
        lrc_parsed_file_destroy(&parsed);
        free2(fixture_text,
              ((int64)fixture_text_len + 1)*SIZEOF(*fixture_text));
        free2(written_text,
              ((int64)written_text_len + 1)*SIZEOF(*written_text));
        test_remove_tree(temp_dir);
        fatal(lrc_test_fail("parse written maxwell lrc"));
    }

    lrc_test_assert_same_parsed_structure(&parsed, &reparsed);

    lrc_parsed_file_destroy(&reparsed);
    free2(lines, line_count*SIZEOF(*lines));
    lrc_parsed_file_destroy(&parsed);
    free2(fixture_text, ((int64)fixture_text_len + 1)*SIZEOF(*fixture_text));
    free2(written_text, ((int64)written_text_len + 1)*SIZEOF(*written_text));
    test_remove_tree(temp_dir);

    return;
}

static void
lrc_test_optional_maxwell_lrc(void) {
    LrcParsedFile parsed = {0};
    LrcParseResult result;
    char *path;
    char *text;
    int32 text_len;

    path = getenv("LRC_TEST_MAXWELL_LRC");
    if (path == NULL) {
        path = "next-phase/maxwell.lrc";
    }
    if (!util_file_exists(path)) {
        return;
    }

    if ((text_len = read_entire_file(path, &text)) < 0) {
        fatal(lrc_test_fail("read maxwell lrc"));
    }
    if (!lrc_parse_text(&parsed, text, text_len, &result)) {
        free2(text, ((int64)text_len + 1)*SIZEOF(*text));
        fatal(lrc_test_fail("parse maxwell lrc"));
    }

    ASSERT(parsed.line_count == 6);
    ASSERT(parsed.timestamped_line_count == 5);
    ASSERT(parsed.blank_line_count == 1);
    lrc_test_assert_line(&parsed,
                         0,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         20,
                         STRLIT("Can I take you out to the pictures, Joan?"));
    lrc_test_assert_line(&parsed,
                         1,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         684,
                         STRLIT("But as she's getting ready to go"));
    lrc_test_assert_line(&parsed,
                         2,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         1064,
                         STRLIT("A knock comes on the door"));
    lrc_test_assert_line(&parsed,
                         3,
                         LRC_PARSED_LINE_KIND_BLANK,
                         -1,
                         STRLIT(""));
    lrc_test_assert_line(&parsed,
                         4,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         1414,
                         STRLIT("Bang, bang, Maxwell's silver hammer"));
    lrc_test_assert_line(&parsed,
                         5,
                         LRC_PARSED_LINE_KIND_TIMESTAMPED,
                         1800,
                         STRLIT("Came down upon her head"));

    lrc_parsed_file_destroy(&parsed);
    free2(text, ((int64)text_len + 1)*SIZEOF(*text));

    return;
}

int32
main(void) {
    lrc_test_parse_timestamped_and_blank_lines();
    lrc_test_parse_crlf_and_space_blank_line();
    lrc_test_reject_malformed_timestamps();
    lrc_test_reject_untimed_text();
    lrc_test_duplicate_timestamps_are_preserved();
    lrc_test_format_timestamp_hundredths();
    lrc_test_format_timestamp_seconds_rounding();
    lrc_test_format_timestamped_line_preserves_text();
    lrc_test_format_timestamped_empty_line();
    lrc_test_format_reject_bad_inputs();
    lrc_test_write_generated_file();
    lrc_test_write_timestamped_empty_line();
    lrc_test_write_overwrites_existing_file();
    lrc_test_write_rejects_bad_inputs();
    lrc_test_optional_maxwell_formatting();
    lrc_test_optional_maxwell_write_structure();
    lrc_test_optional_maxwell_lrc();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_lrc */
