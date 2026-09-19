#if !defined(CTC_TOKENIZER_H)
#define CTC_TOKENIZER_H

#include "cbase.h"
#include "errors.h"
#include "lyrics.h"

typedef struct LrcCtcTokenizeResult {
    LrcResultHeader header;

    int32 byte_offset;
    int32 line_index;
    int32 token_id;
} LrcCtcTokenizeResult;

typedef struct LrcCtcTextToken {
    int32 token_id;
    int32 normalized_start;
    int32 normalized_end;
    int32 line_index;
    int32 segment_index;

    bool starts_segment;
} LrcCtcTextToken;

typedef struct LrcCtcTokenizedText {
    LrcCtcTextToken *tokens;

    int32 token_count;
} LrcCtcTokenizedText;

typedef struct LrcCtcTokenizerResult {
    LrcPathResultHeader path_header;

    int32 line_index;
    int32 token_id;
} LrcCtcTokenizerResult;

typedef struct LrcCtcToken {
    char *text;

    int32 text_len;
    int32 text_offset;
    int32 id;

    bool is_blank;
    bool is_unknown;
} LrcCtcToken;

typedef struct LrcCtcTokenizer {
    LrcCtcToken *tokens;
    StrBuilder text_storage;

    int32 token_count;
    int32 blank_id;
    int32 unknown_id;
} LrcCtcTokenizer;

static void lrc_ctc_tokenize_result_init(LrcCtcTokenizeResult *result);
static void lrc_ctc_tokenized_text_destroy(LrcCtcTokenizedText *text);
static bool lrc_ctc_tokenizer_tokenize_normalized(
    LrcCtcTokenizer *tokenizer, LrcLyricsNormalized *normalized,
    LrcCtcTokenizedText *tokens, LrcCtcTokenizeResult *result);
static void lrc_ctc_tokenizer_init(LrcCtcTokenizer *tokenizer);
static void lrc_ctc_tokenizer_destroy(LrcCtcTokenizer *tokenizer);
static void lrc_ctc_tokenizer_result_init(LrcCtcTokenizerResult *result);
static bool lrc_ctc_tokenizer_load_file(LrcCtcTokenizer *tokenizer, char *path,
                                        LrcCtcTokenizerResult *result);
static bool lrc_ctc_tokenizer_token_id(LrcCtcTokenizer *tokenizer, char *token,
                                       int32 token_len, int32 *id);

#endif /* CTC_TOKENIZER_H */
