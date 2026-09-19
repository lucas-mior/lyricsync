#if !defined(LYRICS_FILE_H)
#define LYRICS_FILE_H

#include "cbase.h"
#include "errors.h"
#include "ctc_text.h"

typedef struct LrcLyricsLine {
    char *text;

    int32 text_len;
    int32 text_start;
    int32 text_end;
} LrcLyricsLine;

struct LrcLyrics {
    char *text;
    LrcLyricsLine *lines;

    int32 text_len;
    int32 line_count;
    int32 nonempty_line_count;

    bool had_utf8_bom;
};

typedef struct LrcLyricsLoadResult {
    LrcPathResultHeader path_header;

    int32 byte_offset;
} LrcLyricsLoadResult;

static void lrc_lyrics_destroy(LrcLyrics *lyrics);
static void lrc_lyrics_load_result_init(LrcLyricsLoadResult *result);
static bool lrc_lyrics_load_file(LrcLyrics *lyrics, char *path,
                                 LrcLyricsLoadResult *result);

#endif /* LYRICS_FILE_H */
