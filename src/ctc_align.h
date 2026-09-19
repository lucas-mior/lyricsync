#if !defined(CTC_ALIGN_H)
#define CTC_ALIGN_H

#include "cbase.h"
#include "errors.h"
#include "ctc_inference.h"
#include "ctc_tokenizer.h"

typedef struct LrcCtcAlignResult {
    LrcResultHeader header;

    int32 frame_index;
    int32 token_index;
} LrcCtcAlignResult;

enum LrcCtcAlignStarMode {
    LRC_CTC_ALIGN_STAR_MODE_NONE,
    LRC_CTC_ALIGN_STAR_MODE_EDGES,
    LRC_CTC_ALIGN_STAR_MODE_SEGMENT,
};

typedef struct LrcCtcAlignPlan {
    int32 *target_token_ids;
    bool *target_segment_starts;

    int32 target_token_count;
    int32 blank_token_id;
    int32 star_token_id;

    enum LrcCtcAlignStarMode star_mode;
} LrcCtcAlignPlan;

typedef struct LrcCtcTrellis {
    float *scores;
    int32 *previous_states;

    int32 frame_count;
    int32 target_token_count;
    int32 state_count;
    int64 cell_count;

    int32 star_token_id;
    bool has_edge_stars;
    bool has_segment_stars;
} LrcCtcTrellis;

typedef struct LrcCtcPathStep {
    int32 frame_index;
    int32 state_index;
    int32 token_index;
    int32 token_id;

    bool is_blank;
    bool is_star;
} LrcCtcPathStep;

typedef struct LrcCtcPath {
    LrcCtcPathStep *steps;

    int32 step_count;
} LrcCtcPath;

typedef struct LrcCtcPathSegment {
    int32 token_index;
    int32 start_frame;
    int32 end_frame;

    float start_seconds;
    float end_seconds;
    float score;

    int32 token_id;

    bool is_blank;
    bool is_star;
} LrcCtcPathSegment;

typedef struct LrcCtcPathSegments {
    LrcCtcPathSegment *segments;

    int32 segment_count;
} LrcCtcPathSegments;

typedef struct LrcCtcAlignedTokenInterval {
    int32 target_token_index;
    int32 segment_start_index;
    int32 segment_end_index;
    int32 token_start_frame;
    int32 token_end_frame;
    int32 padded_start_frame;
    int32 padded_end_frame;

    float padded_start_seconds;
    float padded_end_seconds;

    bool is_star;
} LrcCtcAlignedTokenInterval;

typedef struct LrcCtcAlignedTokenIntervals {
    LrcCtcAlignedTokenInterval *intervals;

    int32 interval_count;
} LrcCtcAlignedTokenIntervals;

typedef struct LrcCtcTokenSpan {
    int32 token_index;
    int32 start_frame;
    int32 end_frame;
    int32 padded_start_frame;
    int32 padded_end_frame;

    float start_seconds;
    float end_seconds;
    float padded_start_seconds;
    float padded_end_seconds;
    float score;

    int32 token_id;
} LrcCtcTokenSpan;

typedef struct LrcCtcTokenSpans {
    LrcCtcTokenSpan *spans;

    int32 span_count;
} LrcCtcTokenSpans;

typedef struct LrcCtcWordSpan {
    int32 word_index;
    int32 token_start_index;
    int32 token_end_index;
    int32 span_start_index;
    int32 span_end_index;

    int32 normalized_start;
    int32 normalized_end;
    int32 line_index;

    float start_seconds;
    float end_seconds;
    float score;
} LrcCtcWordSpan;

typedef struct LrcCtcWordSpans {
    LrcCtcWordSpan *spans;

    int32 span_count;
} LrcCtcWordSpans;

enum LrcCtcLineTimestampKind {
    LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED,
    LRC_CTC_LINE_TIMESTAMP_KIND_BLANK,
};

typedef struct LrcCtcLineTimestamp {
    int32 word_start_index;
    int32 word_end_index;

    int32 line_index;

    float start_seconds;
    float end_seconds;
    float score;

    enum LrcCtcLineTimestampKind kind;
} LrcCtcLineTimestamp;

typedef struct LrcCtcLineTimestamps {
    LrcCtcLineTimestamp *lines;

    int32 line_count;
    int32 timestamped_line_count;
    int32 blank_line_count;
} LrcCtcLineTimestamps;

static void lrc_ctc_align_result_init(LrcCtcAlignResult *result);
static void lrc_ctc_trellis_destroy(LrcCtcTrellis *trellis);
static void lrc_ctc_path_destroy(LrcCtcPath *path);
static void lrc_ctc_path_segments_destroy(LrcCtcPathSegments *segments);
static void lrc_ctc_aligned_token_intervals_destroy(
    LrcCtcAlignedTokenIntervals *intervals);
static bool lrc_ctc_pad_token_intervals_with_blanks(
    LrcCtcPathSegments *segments, float frame_duration_seconds,
    LrcCtcAlignedTokenIntervals *intervals, LrcCtcAlignResult *result);
static void lrc_ctc_token_spans_destroy(LrcCtcTokenSpans *spans);
static void lrc_ctc_word_spans_destroy(LrcCtcWordSpans *spans);
static void lrc_ctc_line_timestamps_destroy(LrcCtcLineTimestamps *timestamps);
static float *lrc_ctc_trellis_cell(
    LrcCtcTrellis *trellis,
    int32 frame_index,
    int32 state_index
);
static void lrc_ctc_align_plan_init(LrcCtcAlignPlan *plan,
                                    int32 *target_token_ids,
                                    bool *target_segment_starts,
                                    int32 target_token_count,
                                    int32 blank_token_id,
                                    enum LrcCtcAlignStarMode star_mode,
                                    int32 star_token_id);
static bool lrc_ctc_trellis_score_forward_with_plan(LrcCtcTrellis *trellis,
                                                    LrcCtcEmissions *emissions,
                                                    LrcCtcAlignPlan *plan,
                                                    LrcCtcAlignResult *result);
static bool lrc_ctc_trellis_backtrack_with_plan(LrcCtcTrellis *trellis,
                                                LrcCtcEmissions *emissions,
                                                LrcCtcAlignPlan *plan,
                                                LrcCtcPath *path,
                                                LrcCtcAlignResult *result);
static bool lrc_ctc_path_to_segments(LrcCtcPath *path,
                                     LrcCtcEmissions *emissions,
                                     float frame_duration_seconds,
                                     LrcCtcPathSegments *segments,
                                     LrcCtcAlignResult *result);
static bool lrc_ctc_path_to_token_spans(LrcCtcPath *path,
                                        LrcCtcEmissions *emissions,
                                        float frame_duration_seconds,
                                        LrcCtcTokenSpans *spans,
                                        LrcCtcAlignResult *result);
static bool lrc_ctc_path_to_padded_token_spans_with_plan(
    LrcCtcPath *path, LrcCtcEmissions *emissions, LrcCtcAlignPlan *plan,
    float frame_duration_seconds, LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result);
static bool lrc_ctc_token_spans_to_word_spans(LrcCtcTokenSpans *token_spans,
                                              LrcCtcTokenizedText *tokens,
                                              LrcLyricsNormalized *normalized,
                                              LrcCtcWordSpans *word_spans,
                                              LrcCtcAlignResult *result);
static bool lrc_ctc_word_spans_to_line_timestamps(
    LrcCtcWordSpans *word_spans, LrcLyricsNormalized *normalized,
    LrcCtcLineTimestamps *line_timestamps, LrcCtcAlignResult *result);

#endif /* CTC_ALIGN_H */
