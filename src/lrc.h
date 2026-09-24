#if !defined(LRC_H)
#define LRC_H

#include "cbase.h"
#include "errors.h"

enum LrcParsedLineKind {
    LRC_PARSED_LINE_KIND_TIMESTAMPED,
    LRC_PARSED_LINE_KIND_BLANK,
};

enum LrcOutputLineKind {
    LRC_OUTPUT_LINE_KIND_TIMESTAMPED,
    LRC_OUTPUT_LINE_KIND_BLANK,
};

typedef struct LrcParsedLine {
    char *text;

    int32 text_len;
    int32 source_line_index;
    int32 timestamp_hundredths;

    float timestamp_seconds;
    enum LrcParsedLineKind kind;
} LrcParsedLine;

typedef struct LrcParseResult {
    LrcResultHeader header;

    int32 line_index;
    int32 byte_offset;
} LrcParseResult;

typedef struct LrcFormatResult {
    LrcResultHeader header;

    float seconds;
    int32 timestamp_hundredths;
} LrcFormatResult;

typedef struct LrcWriteResult {
    LrcPathResultHeader path_header;
    enum LsError format_error;

    int32 line_index;
} LrcWriteResult;

typedef struct LrcOutputLine {
    char *text;

    int32 text_len;
    int32 timestamp_hundredths;

    enum LrcOutputLineKind kind;
} LrcOutputLine;

typedef struct LrcParsedFile {
    char *text;
    LrcParsedLine *lines;

    int32 text_len;
    int32 line_count;
    int32 timestamped_line_count;
    int32 blank_line_count;
} LrcParsedFile;

static void lrc_format_result_init(LrcFormatResult *result);
static void lrc_write_result_init(LrcWriteResult *result);
static bool lrc_timestamp_hundredths_from_seconds(float seconds,
                                                  int32 *timestamp_hundredths,
                                                  LrcFormatResult *result);
static bool lrc_format_timestamp_hundredths(int32 timestamp_hundredths,
                                            char *buffer, int32 buffer_len,
                                            int32 *formatted_len,
                                            LrcFormatResult *result);
static bool lrc_format_timestamped_line_hundredths(String *builder,
                                                   int32 timestamp_hundredths,
                                                   char *text, int32 text_len,
                                                   LrcFormatResult *result);
#if TESTING
static void lrc_parse_result_init(LrcParseResult *result);
static void lrc_parsed_file_destroy(LrcParsedFile *parsed);
static bool lrc_parse_text(LrcParsedFile *parsed, char *text, int32 text_len,
                           LrcParseResult *result);
#endif
static bool lrc_format_output_lines(String *builder, LrcOutputLine *lines,
                                    int32 line_count, LrcWriteResult *result);
static bool lrc_write_output_file(char *path, LrcOutputLine *lines,
                                  int32 line_count, LrcWriteResult *result);

#endif /* LRC_H */
