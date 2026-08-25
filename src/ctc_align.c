#include "cbase.h"
#include "lyricsync.h"
#include "ctc_align.h"

#if !defined(TESTING_ctc_align)
#define TESTING_ctc_align 0
#endif

static void
lrc_ctc_align_result_init(LrcCtcAlignResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_init(&result->header);

    result->frame_index = -1;
    result->token_index = -1;

    return;
}

static void
lrc_ctc_align_result_set(
    LrcCtcAlignResult *result,
    enum LsError error,
    char *message,
    int64 frame_index,
    int64 token_index
) {
    if (result == NULL) {
        return;
    }

    lrc_result_header_set(&result->header, error, message);

    result->frame_index = (int32)CLAMP(frame_index, INT32_MIN, INT32_MAX);
    result->token_index = (int32)CLAMP(token_index, INT32_MIN, INT32_MAX);

    return;
}


static void
lrc_ctc_align_plan_init(
    LrcCtcAlignPlan *plan,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 blank_token_id,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id
) {
    if (plan == NULL) {
        return;
    }

    plan->target_token_ids = target_token_ids;
    plan->target_segment_starts = target_segment_starts;
    plan->target_token_count = target_token_count;
    plan->blank_token_id = blank_token_id;
    plan->star_mode = star_mode;
    plan->star_token_id = star_token_id;

    if (star_mode != LRC_CTC_ALIGN_STAR_MODE_SEGMENT) {
        plan->target_segment_starts = NULL;
    }
    if (star_mode == LRC_CTC_ALIGN_STAR_MODE_NONE) {
        plan->star_token_id = -1;
    }

    return;
}

static bool
lrc_ctc_align_plan_missing(LrcCtcAlignPlan *plan, LrcCtcAlignResult *result) {
    if (plan != NULL) {
        return false;
    }

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    lrc_ctc_align_result_set(
        result,
        LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
        "CTC alignment plan is missing",
        -1,
        -1
    );

    return true;
}

enum LrcCtcAlignStateKind {
    LRC_CTC_ALIGN_STATE_BLANK,
    LRC_CTC_ALIGN_STATE_TOKEN,
    LRC_CTC_ALIGN_STATE_STAR,
};

typedef struct LrcCtcAlignState {
    enum LrcCtcAlignStateKind kind;

    int32 token_index;
    int32 token_id;
} LrcCtcAlignState;

typedef struct LrcCtcAlignGraph {
    LrcCtcAlignState *states;

    int32 state_count;
    int32 target_token_count;
} LrcCtcAlignGraph;


static void
lrc_ctc_align_graph_destroy(LrcCtcAlignGraph *graph) {
    if (graph == NULL) {
        return;
    }

    free2(graph->states, graph->state_count*SIZEOF(*graph->states));

    memset64(graph, 0, SIZEOF(*graph));

    return;
}

static bool
lrc_ctc_align_segment_star_count(
    bool *target_segment_starts,
    int32 target_token_count,
    int32 *star_count,
    LrcCtcAlignResult *result
) {
    ASSERT(star_count);
    *star_count = 0;

    if (target_segment_starts == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC segment-star markers are missing",
            -1,
            -1
        );
        return false;
    }

    for (int32 i = 0; i < target_token_count; i += 1) {
        if (target_segment_starts[i]) {
            *star_count += 1;
        }
    }

    return true;
}

static bool
lrc_ctc_align_star_mode_extra_labels(
    enum LrcCtcAlignStarMode star_mode,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 *extra_labels,
    LrcCtcAlignResult *result
) {
    if (extra_labels == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph extra-label destination is missing",
            -1,
            -1
        );
        return false;
    }

    *extra_labels = 0;
    switch (star_mode) {
    case LRC_CTC_ALIGN_STAR_MODE_NONE:
        return true;
    case LRC_CTC_ALIGN_STAR_MODE_EDGES:
        *extra_labels = 2;
        return true;
    case LRC_CTC_ALIGN_STAR_MODE_SEGMENT:
        return lrc_ctc_align_segment_star_count(target_segment_starts,
                                                target_token_count,
                                                extra_labels,
                                                result);
    default:
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph star mode is invalid",
            -1,
            star_mode
        );
        return false;
    }
}

static bool
lrc_ctc_align_graph_label_count(
    int32 target_token_count,
    enum LrcCtcAlignStarMode star_mode,
    bool *target_segment_starts,
    int32 *label_count,
    LrcCtcAlignResult *result
) {
    int32 extra_labels;

    if (label_count == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph label-count destination is missing",
            -1,
            -1
        );
        return false;
    }
    *label_count = 0;

    if (target_token_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC graph target token count must be positive",
            -1,
            target_token_count
        );
        return false;
    }

    if (!lrc_ctc_align_star_mode_extra_labels(star_mode,
                                              target_segment_starts,
                                              target_token_count,
                                              &extra_labels,
                                              result)) {
        return false;
    }
    if (target_token_count > INT32_MAX - extra_labels) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_TOO_LARGE,
            "CTC graph label count is too large",
            -1,
            target_token_count
        );
        return false;
    }

    *label_count = target_token_count + extra_labels;

    return true;
}

static bool
lrc_ctc_align_graph_state_count_for_mode(
    int32 target_token_count,
    enum LrcCtcAlignStarMode star_mode,
    bool *target_segment_starts,
    int32 *state_count,
    LrcCtcAlignResult *result
) {
    int32 label_count;

    if (state_count == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph state-count destination is missing",
            -1,
            -1
        );
        return false;
    }
    *state_count = 0;

    if (!lrc_ctc_align_graph_label_count(target_token_count,
                                         star_mode,
                                         target_segment_starts,
                                         &label_count,
                                         result)) {
        return false;
    }
    if (label_count > (INT32_MAX - 1)/2) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_TOO_LARGE,
            "CTC graph state count is too large",
            -1,
            target_token_count
        );
        return false;
    }

    *state_count = 2*label_count + 1;

    return true;
}

static bool
lrc_ctc_align_checked_multiply(
    int64 left,
    int64 right,
    int64 *out,
    char *message,
    LrcCtcAlignResult *result
) {
    if (out == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC checked multiplication output is missing",
            left,
            right
        );
        return false;
    }
    *out = 0;

    if ((left < 0) || (right < 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            message,
            left,
            right
        );
        return false;
    }
    if ((right > 0) && (left > INT64_MAX/right)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_TOO_LARGE,
            message,
            left,
            right
        );
        return false;
    }

    *out = left*right;

    return true;
}

static void
lrc_ctc_align_graph_set_star_state(
    LrcCtcAlignState *state,
    int32 star_token_id
) {
    state->kind = LRC_CTC_ALIGN_STATE_STAR;
    state->token_index = -1;
    state->token_id = star_token_id;

    return;
}

static void
lrc_ctc_align_graph_set_token_state(
    LrcCtcAlignState *state,
    int32 *target_token_ids,
    int32 token_index
) {
    state->kind = LRC_CTC_ALIGN_STATE_TOKEN;
    state->token_index = token_index;
    state->token_id = target_token_ids[token_index];

    return;
}

static bool
lrc_ctc_align_graph_label_is_edge_star(
    int32 label_index,
    int32 label_count,
    enum LrcCtcAlignStarMode star_mode
) {
    if (star_mode != LRC_CTC_ALIGN_STAR_MODE_EDGES) {
        return false;
    }

    return (label_index == 0) || (label_index == label_count - 1);
}

static bool
lrc_ctc_align_graph_build_for_mode(
    LrcCtcAlignGraph *graph,
    int32 *target_token_ids,
    int32 target_token_count,
    enum LrcCtcAlignStarMode star_mode,
    bool *target_segment_starts,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    int32 state_count;
    int32 label_count;
    int64 alloc_size;
    int32 token_index;
    bool segment_star_pending;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (graph == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (target_token_ids == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph target token ids are missing",
            -1,
            -1
        );
        return false;
    }

    lrc_ctc_align_graph_destroy(graph);
    if (!lrc_ctc_align_graph_label_count(target_token_count,
                                         star_mode,
                                         target_segment_starts,
                                         &label_count,
                                         result)) {
        return false;
    }
    if (!lrc_ctc_align_graph_state_count_for_mode(target_token_count,
                                                  star_mode,
                                                  target_segment_starts,
                                                  &state_count,
                                                  result)) {
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        state_count,
        SIZEOF(*graph->states),
        &alloc_size,
        "CTC graph state allocation is too large",
        result
    )) {
        return false;
    }

    graph->states = malloc2(alloc_size);
    graph->state_count = state_count;
    graph->target_token_count = target_token_count;

    for (int32 i = 0; i < graph->state_count; i += 1) {
        LrcCtcAlignState *state = graph->states + i;

        state->kind = LRC_CTC_ALIGN_STATE_BLANK;
        state->token_index = -1;
        state->token_id = -1;
    }

    token_index = 0;
    segment_star_pending = false;
    for (int32 label_index = 0; label_index < label_count; label_index += 1) {
        int32 state_index = 2*label_index + 1;
        LrcCtcAlignState *state = graph->states + state_index;
        bool is_star;

        is_star = lrc_ctc_align_graph_label_is_edge_star(label_index,
                                                         label_count,
                                                         star_mode);
        if ((star_mode == LRC_CTC_ALIGN_STAR_MODE_SEGMENT)
            && (token_index < target_token_count)
            && target_segment_starts[token_index]
            && !segment_star_pending) {
            is_star = true;
            segment_star_pending = true;
        }
        if (is_star) {
            lrc_ctc_align_graph_set_star_state(state, star_token_id);
            continue;
        }

        ASSERT(token_index >= 0);
        ASSERT(token_index < target_token_count);
        lrc_ctc_align_graph_set_token_state(state,
                                            target_token_ids,
                                            token_index);
        token_index += 1;
        segment_star_pending = false;
    }
    ASSERT(token_index == target_token_count);

    return true;
}

static int32
lrc_ctc_required_frame_count_for_graph(LrcCtcAlignGraph *graph) {
    int32 label_count;
    int32 frame_count;
    int32 previous_token_id;

    if ((graph == NULL) || (graph->states == NULL)
        || (graph->state_count <= 0)) {
        return -1;
    }

    label_count = (graph->state_count - 1)/2;
    frame_count = label_count;
    previous_token_id = -1;
    for (int32 i = 1; i < graph->state_count; i += 2) {
        LrcCtcAlignState *state = graph->states + i;

        if ((previous_token_id >= 0)
            && (state->token_id == previous_token_id)) {
            if (frame_count >= INT32_MAX) {
                return -1;
            }
            frame_count += 1;
        }
        previous_token_id = state->token_id;
    }

    return frame_count;
}

static bool
lrc_ctc_align_graph_state_valid(LrcCtcAlignGraph *graph, int32 state_index) {
    if (graph == NULL) {
        return false;
    }
    if (graph->states == NULL) {
        return false;
    }
    if ((state_index < 0) || (state_index >= graph->state_count)) {
        return false;
    }

    return true;
}

static bool
lrc_ctc_align_state_can_skip(
    LrcCtcAlignGraph *graph,
    int32 from_state,
    int32 to_state
) {
    LrcCtcAlignState *from;
    LrcCtcAlignState *to;

    if (!lrc_ctc_align_graph_state_valid(graph, from_state)
        || !lrc_ctc_align_graph_state_valid(graph, to_state)) {
        return false;
    }
    if (to_state != from_state + 2) {
        return false;
    }

    from = graph->states + from_state;
    to = graph->states + to_state;
    if (from->kind == LRC_CTC_ALIGN_STATE_BLANK) {
        return false;
    }
    if (to->kind == LRC_CTC_ALIGN_STATE_BLANK) {
        return false;
    }

    return from->token_id != to->token_id;
}

static bool
lrc_ctc_align_graph_transition_allowed(
    LrcCtcAlignGraph *graph,
    int32 from_state,
    int32 to_state
) {
    if (!lrc_ctc_align_graph_state_valid(graph, from_state)
        || !lrc_ctc_align_graph_state_valid(graph, to_state)) {
        return false;
    }
    if (to_state == from_state) {
        return true;
    }
    if (to_state == from_state + 1) {
        return true;
    }

    return lrc_ctc_align_state_can_skip(graph, from_state, to_state);
}


static void
lrc_ctc_trellis_destroy(LrcCtcTrellis *trellis) {
    if (trellis == NULL) {
        return;
    }

    free2(trellis->scores,
          trellis->cell_count*SIZEOF(*trellis->scores));
    free2(trellis->previous_states,
          trellis->cell_count*SIZEOF(*trellis->previous_states));

    memset64(trellis, 0, SIZEOF(*trellis));

    return;
}



static void
lrc_ctc_path_destroy(LrcCtcPath *path) {
    if (path == NULL) {
        return;
    }

    ARRAY_FREE(path->steps);

    memset64(path, 0, SIZEOF(*path));

    return;
}


static void
lrc_ctc_path_segments_destroy(LrcCtcPathSegments *segments) {
    if (segments == NULL) {
        return;
    }

    ARRAY_FREE(segments->segments);

    memset64(segments, 0, SIZEOF(*segments));

    return;
}


static void
lrc_ctc_aligned_token_intervals_destroy(
    LrcCtcAlignedTokenIntervals *intervals
) {
    if (intervals == NULL) {
        return;
    }

    ARRAY_FREE(intervals->intervals);

    memset64(intervals, 0, SIZEOF(*intervals));

    return;
}


static void
lrc_ctc_token_spans_destroy(LrcCtcTokenSpans *spans) {
    if (spans == NULL) {
        return;
    }

    ARRAY_FREE(spans->spans);

    memset64(spans, 0, SIZEOF(*spans));

    return;
}


static void
lrc_ctc_word_spans_destroy(LrcCtcWordSpans *spans) {
    if (spans == NULL) {
        return;
    }

    ARRAY_FREE(spans->spans);

    memset64(spans, 0, SIZEOF(*spans));

    return;
}


static void
lrc_ctc_line_timestamps_destroy(LrcCtcLineTimestamps *timestamps) {
    if (timestamps == NULL) {
        return;
    }

    ARRAY_FREE(timestamps->lines);

    memset64(timestamps, 0, SIZEOF(*timestamps));

    return;
}

static bool
lrc_ctc_token_spans_allocate(
    LrcCtcTokenSpans *spans,
    int32 span_count,
    LrcCtcAlignResult *result
) {
    int64 alloc_size;

    if (spans == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC token spans destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (span_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path does not contain token frames",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        span_count,
        SIZEOF(*spans->spans),
        &alloc_size,
        "CTC token span allocation is too large",
        result
    )) {
        return false;
    }

    lrc_ctc_token_spans_destroy(spans);
    ARRAY_INIT_COUNT(spans->spans, span_count);
    spans->span_count = span_count;

    for (int32 i = 0; i < spans->span_count; i += 1) {
        spans->spans[i].token_index = -1;
        spans->spans[i].start_frame = -1;
        spans->spans[i].end_frame = -1;
        spans->spans[i].padded_start_frame = -1;
        spans->spans[i].padded_end_frame = -1;
        spans->spans[i].start_seconds = 0.0f;
        spans->spans[i].end_seconds = 0.0f;
        spans->spans[i].padded_start_seconds = 0.0f;
        spans->spans[i].padded_end_seconds = 0.0f;
        spans->spans[i].score = -INFINITY;
        spans->spans[i].token_id = -1;
    }

    return true;
}

static bool
lrc_ctc_path_segments_allocate(
    LrcCtcPathSegments *segments,
    int32 segment_count,
    LrcCtcAlignResult *result
) {
    int64 alloc_size;

    if (segments == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC path segments destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (segment_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path does not contain segments",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        segment_count,
        SIZEOF(*segments->segments),
        &alloc_size,
        "CTC path segment allocation is too large",
        result
    )) {
        return false;
    }

    lrc_ctc_path_segments_destroy(segments);
    ARRAY_INIT_COUNT(segments->segments, segment_count);
    segments->segment_count = segment_count;

    for (int32 i = 0; i < segments->segment_count; i += 1) {
        segments->segments[i].token_index = -1;
        segments->segments[i].start_frame = -1;
        segments->segments[i].end_frame = -1;
        segments->segments[i].start_seconds = 0.0f;
        segments->segments[i].end_seconds = 0.0f;
        segments->segments[i].score = -INFINITY;
        segments->segments[i].token_id = -1;
        segments->segments[i].is_blank = true;
        segments->segments[i].is_star = false;
    }

    return true;
}

static bool
lrc_ctc_aligned_token_intervals_allocate(
    LrcCtcAlignedTokenIntervals *intervals,
    int32 interval_count,
    LrcCtcAlignResult *result
) {
    int64 alloc_size;

    if (intervals == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC aligned token intervals destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (interval_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC aligned token interval count must be positive",
            -1,
            interval_count
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        interval_count,
        SIZEOF(*intervals->intervals),
        &alloc_size,
        "CTC aligned token interval allocation is too large",
        result
    )) {
        return false;
    }

    lrc_ctc_aligned_token_intervals_destroy(intervals);
    ARRAY_INIT_COUNT(intervals->intervals, interval_count);
    intervals->interval_count = interval_count;

    for (int32 i = 0; i < intervals->interval_count; i += 1) {
        intervals->intervals[i].target_token_index = -1;
        intervals->intervals[i].segment_start_index = -1;
        intervals->intervals[i].segment_end_index = -1;
        intervals->intervals[i].token_start_frame = -1;
        intervals->intervals[i].token_end_frame = -1;
        intervals->intervals[i].padded_start_frame = -1;
        intervals->intervals[i].padded_end_frame = -1;
        intervals->intervals[i].padded_start_seconds = 0.0f;
        intervals->intervals[i].padded_end_seconds = 0.0f;
        intervals->intervals[i].is_star = false;
    }

    return true;
}

static bool
lrc_ctc_path_allocate(
    LrcCtcPath *path,
    int32 step_count,
    LrcCtcAlignResult *result
) {
    int64 alloc_size;

    if (path == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC path destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (step_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC path step count must be positive",
            step_count,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        step_count,
        SIZEOF(*path->steps),
        &alloc_size,
        "CTC path allocation is too large",
        result
    )) {
        return false;
    }

    lrc_ctc_path_destroy(path);
    ARRAY_INIT_COUNT(path->steps, step_count);
    path->step_count = step_count;

    for (int32 i = 0; i < path->step_count; i += 1) {
        path->steps[i].frame_index = -1;
        path->steps[i].state_index = -1;
        path->steps[i].token_index = -1;
        path->steps[i].token_id = -1;
        path->steps[i].is_blank = true;
        path->steps[i].is_star = false;
    }

    return true;
}

static bool
lrc_ctc_trellis_dimensions_valid(
    int32 frame_count,
    int32 target_token_count,
    int32 state_count,
    int64 *cell_count,
    LrcCtcAlignResult *result
) {
    int64 cells;

    if (cell_count == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC trellis cell-count output is missing",
            frame_count,
            target_token_count
        );
        return false;
    }
    *cell_count = 0;

    if (frame_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC trellis frame count must be positive",
            frame_count,
            target_token_count
        );
        return false;
    }
    if (state_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC trellis state count must be positive",
            frame_count,
            state_count
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        frame_count,
        state_count,
        &cells,
        "CTC trellis cell count is too large",
        result
    )) {
        return false;
    }

    *cell_count = cells;

    return true;
}

static int32 *
lrc_ctc_trellis_previous_state_cell(
    LrcCtcTrellis *trellis,
    int32 frame_index,
    int32 state_index
) {
    if (trellis == NULL) {
        return NULL;
    }
    if (trellis->previous_states == NULL) {
        return NULL;
    }
    if ((frame_index < 0) || (frame_index >= trellis->frame_count)) {
        return NULL;
    }
    if ((state_index < 0) || (state_index >= trellis->state_count)) {
        return NULL;
    }

    return trellis->previous_states
           + (int64)frame_index*(int64)trellis->state_count
           + state_index;
}

static float *
lrc_ctc_trellis_cell(
    LrcCtcTrellis *trellis,
    int32 frame_index,
    int32 state_index
) {
    if (trellis == NULL) {
        return NULL;
    }
    if (trellis->scores == NULL) {
        return NULL;
    }
    if ((frame_index < 0) || (frame_index >= trellis->frame_count)) {
        return NULL;
    }
    if ((state_index < 0) || (state_index >= trellis->state_count)) {
        return NULL;
    }

    return trellis->scores
           + (int64)frame_index*(int64)trellis->state_count
           + state_index;
}

static bool
lrc_ctc_trellis_allocate_for_state_count(
    LrcCtcTrellis *trellis,
    int32 frame_count,
    int32 target_token_count,
    int32 state_count,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    int64 cell_count;
    int64 scores_size;
    int64 previous_states_size;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (trellis == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC trellis destination is missing",
            -1,
            -1
        );
        return false;
    }

    lrc_ctc_trellis_destroy(trellis);
    if (!lrc_ctc_trellis_dimensions_valid(frame_count,
                                          target_token_count,
                                          state_count,
                                          &cell_count,
                                          result)) {
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        cell_count,
        SIZEOF(*trellis->scores),
        &scores_size,
        "CTC trellis score allocation is too large",
        result
    )) {
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        cell_count,
        SIZEOF(*trellis->previous_states),
        &previous_states_size,
        "CTC trellis backpointer allocation is too large",
        result
    )) {
        return false;
    }

    trellis->scores = malloc2(scores_size);
    trellis->previous_states = malloc2(previous_states_size);
    trellis->frame_count = frame_count;
    trellis->target_token_count = target_token_count;
    trellis->state_count = state_count;
    trellis->cell_count = cell_count;
    trellis->star_token_id = star_token_id;
    trellis->has_edge_stars = star_mode == LRC_CTC_ALIGN_STAR_MODE_EDGES;
    trellis->has_segment_stars =
        star_mode == LRC_CTC_ALIGN_STAR_MODE_SEGMENT;

    for (int64 i = 0; i < trellis->cell_count; i += 1) {
        trellis->scores[i] = -INFINITY;
        trellis->previous_states[i] = -1;
    }

    return true;
}

static bool
lrc_ctc_align_emissions_ready(
    LrcCtcEmissions *emissions,
    LrcCtcAlignResult *result
) {
    if (emissions == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC emissions are missing",
            -1,
            -1
        );
        return false;
    }
    if ((emissions->values == NULL) || (emissions->frame_count <= 0)
        || (emissions->vocabulary_size <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_EMISSIONS,
            "CTC emissions are not prepared",
            -1,
            -1
        );
        return false;
    }
    if ((emissions->frame_count > INT32_MAX)
        || (emissions->vocabulary_size > INT32_MAX)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_TOO_LARGE,
            "CTC emissions dimensions exceed alignment index range",
            emissions->frame_count,
            emissions->vocabulary_size
        );
        return false;
    }
    if (emissions->frame_count > INT64_MAX/emissions->vocabulary_size) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_TOO_LARGE,
            "CTC emissions dimensions are too large",
            emissions->frame_count,
            emissions->vocabulary_size
        );
        return false;
    }
    if (emissions->value_count
        != emissions->frame_count*emissions->vocabulary_size) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_EMISSIONS,
            "CTC emissions value count does not match dimensions",
            -1,
            -1
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_trellis_emissions_ready(
    LrcCtcEmissions *emissions,
    int32 blank_token_id,
    LrcCtcAlignResult *result
) {
    if (!lrc_ctc_align_emissions_ready(emissions, result)) {
        return false;
    }
    if ((blank_token_id < 0)
        || ((int32)blank_token_id >= emissions->vocabulary_size)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_BLANK_TOKEN,
            "CTC blank token id is outside the vocabulary",
            -1,
            blank_token_id
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_trellis_prepare_for_graph(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    LrcCtcAlignGraph *graph,
    enum LrcCtcAlignStarMode star_mode,
    int32 blank_token_id,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    float *cell;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_trellis_emissions_ready(emissions,
                                         blank_token_id,
                                         result)) {
        return false;
    }
    if ((graph == NULL) || (graph->states == NULL)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC graph is missing for trellis preparation",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_trellis_allocate_for_state_count(trellis,
                                                  (int32)emissions->frame_count,
                                                  graph->target_token_count,
                                                  graph->state_count,
                                                  star_mode,
                                                  star_token_id,
                                                  result)) {
        return false;
    }

    cell = lrc_ctc_trellis_cell(trellis, 0, 0);
    ASSERT(cell);
    *cell = emissions->values[blank_token_id];
    for (int32 frame = 1; frame < trellis->frame_count; frame += 1) {
        float previous;
        float blank_score;

        cell = lrc_ctc_trellis_cell(trellis, frame - 1, 0);
        ASSERT(cell);
        previous = *cell;
        blank_score = emissions->values[frame*emissions->vocabulary_size
                                        + blank_token_id];

        cell = lrc_ctc_trellis_cell(trellis, frame, 0);
        ASSERT(cell);
        *cell = previous + blank_score;

        *lrc_ctc_trellis_previous_state_cell(trellis, frame, 0) = 0;
    }

    return true;
}

static float
lrc_ctc_emission_value(
    LrcCtcEmissions *emissions,
    int32 frame_index,
    int32 token_id
) {
    int64 index;

    ASSERT(emissions);
    ASSERT(emissions->values);
    ASSERT(frame_index >= 0);
    ASSERT(frame_index < emissions->frame_count);
    ASSERT(token_id >= 0);
    if ((int32)token_id == emissions->vocabulary_size) {
        return 0.0f;
    }
    ASSERT((int32)token_id < emissions->vocabulary_size);

    index = frame_index*emissions->vocabulary_size + token_id;

    return emissions->values[index];
}

static bool
lrc_ctc_target_tokens_valid(
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 blank_token_id,
    LrcCtcAlignResult *result
) {
    if (target_token_ids == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC target token ids are missing",
            -1,
            -1
        );
        return false;
    }
    if (target_token_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC target token count must be positive",
            -1,
            target_token_count
        );
        return false;
    }

    for (int32 i = 0; i < target_token_count; i += 1) {
        if ((target_token_ids[i] < 0)
            || ((int32)target_token_ids[i] >= emissions->vocabulary_size)
            || (target_token_ids[i] == blank_token_id)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN,
                "CTC target token id is invalid",
                -1,
                i
            );
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_star_token_valid(
    LrcCtcEmissions *emissions,
    enum LrcCtcAlignStarMode star_mode,
    int32 blank_token_id,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    if (star_mode == LRC_CTC_ALIGN_STAR_MODE_NONE) {
        return true;
    }
    if ((star_token_id < 0)
        || ((int32)star_token_id != emissions->vocabulary_size)
        || (star_token_id == blank_token_id)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN,
            "CTC star token id must be the synthetic emission column",
            -1,
            star_token_id
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_target_tokens_valid_for_mode(
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 blank_token_id,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    if (!lrc_ctc_target_tokens_valid(emissions,
                                     target_token_ids,
                                     target_token_count,
                                     blank_token_id,
                                     result)) {
        return false;
    }
    if (!lrc_ctc_star_token_valid(emissions,
                                  star_mode,
                                  blank_token_id,
                                  star_token_id,
                                  result)) {
        return false;
    }

    for (int32 i = 0; i < target_token_count; i += 1) {
        if ((star_mode != LRC_CTC_ALIGN_STAR_MODE_NONE)
            && (target_token_ids[i] == star_token_id)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN,
                "CTC target token id cannot be the star token",
                -1,
                i
            );
            return false;
        }
    }

    return true;
}

static int32
lrc_ctc_align_graph_emission_token_id(
    LrcCtcAlignGraph *graph,
    int32 state_index,
    int32 blank_token_id
) {
    LrcCtcAlignState *state;

    ASSERT(lrc_ctc_align_graph_state_valid(graph, state_index));

    state = graph->states + state_index;
    if (state->kind == LRC_CTC_ALIGN_STATE_BLANK) {
        return blank_token_id;
    }

    ASSERT((state->kind == LRC_CTC_ALIGN_STATE_TOKEN)
           || (state->kind == LRC_CTC_ALIGN_STATE_STAR));
    return state->token_id;
}

static void
lrc_ctc_trellis_try_candidate(
    LrcCtcTrellis *trellis,
    LrcCtcAlignGraph *graph,
    int32 frame,
    int32 state,
    int32 previous_state,
    float emission,
    float *best_score,
    int32 *best_previous_state
) {
    float *previous_cell;
    float candidate;

    ASSERT(trellis);
    ASSERT(graph);
    ASSERT(best_score);
    ASSERT(best_previous_state);

    if (!lrc_ctc_align_graph_transition_allowed(graph, previous_state, state)) {
        return;
    }

    previous_cell = lrc_ctc_trellis_cell(trellis, frame - 1, previous_state);
    ASSERT(previous_cell);
    if (!isfinite(*previous_cell)) {
        return;
    }

    candidate = *previous_cell + emission;
    if (candidate > *best_score) {
        *best_score = candidate;
        *best_previous_state = previous_state;
    }

    return;
}

static bool
lrc_ctc_trellis_score_forward_for_mode(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 blank_token_id,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignGraph graph = {0};
    int32 required_frame_count;
    float *cell;
    int32 *previous_state_cell;
    bool ok;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_trellis_emissions_ready(emissions,
                                         blank_token_id,
                                         result)) {
        return false;
    }
    if (!lrc_ctc_target_tokens_valid_for_mode(emissions,
                                              target_token_ids,
                                              target_token_count,
                                              blank_token_id,
                                              star_mode,
                                              star_token_id,
                                              result)) {
        return false;
    }

    if (!lrc_ctc_align_graph_build_for_mode(&graph,
                                            target_token_ids,
                                            target_token_count,
                                            star_mode,
                                            target_segment_starts,
                                            star_token_id,
                                            result)) {
        return false;
    }

    required_frame_count = lrc_ctc_required_frame_count_for_graph(&graph);
    if (required_frame_count <= 0) {
        lrc_ctc_align_graph_destroy(&graph);
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_TOO_LARGE,
            "CTC required alignment frame count is invalid",
            -1,
            target_token_count
        );
        return false;
    }
    if (emissions->frame_count < required_frame_count) {
        lrc_ctc_align_graph_destroy(&graph);
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_IMPOSSIBLE_ALIGNMENT,
            "CTC emissions have too few frames for target tokens",
            emissions->frame_count,
            target_token_count
        );
        return false;
    }

    ok = lrc_ctc_trellis_prepare_for_graph(trellis,
                                           emissions,
                                           &graph,
                                           star_mode,
                                           blank_token_id,
                                           star_token_id,
                                           result);
    if (!ok) {
        lrc_ctc_align_graph_destroy(&graph);
        return false;
    }

    cell = lrc_ctc_trellis_cell(trellis, 0, 1);
    ASSERT(cell);
    *cell = lrc_ctc_emission_value(
        emissions,
        0,
        lrc_ctc_align_graph_emission_token_id(&graph, 1, blank_token_id)
    );

    for (int32 frame = 1; frame < trellis->frame_count; frame += 1) {
        for (int32 state = 1; state < trellis->state_count; state += 1) {
            float emission;
            float best_score;
            int32 best_previous_state;
            int32 token_id;

            token_id = lrc_ctc_align_graph_emission_token_id(&graph,
                                                             state,
                                                             blank_token_id);
            emission = lrc_ctc_emission_value(emissions, frame, token_id);
            best_score = -INFINITY;
            best_previous_state = -1;

            lrc_ctc_trellis_try_candidate(trellis,
                                          &graph,
                                          frame,
                                          state,
                                          state,
                                          emission,
                                          &best_score,
                                          &best_previous_state);
            lrc_ctc_trellis_try_candidate(trellis,
                                          &graph,
                                          frame,
                                          state,
                                          state - 1,
                                          emission,
                                          &best_score,
                                          &best_previous_state);
            lrc_ctc_trellis_try_candidate(trellis,
                                          &graph,
                                          frame,
                                          state,
                                          state - 2,
                                          emission,
                                          &best_score,
                                          &best_previous_state);

            cell = lrc_ctc_trellis_cell(trellis, frame, state);
            ASSERT(cell);
            *cell = best_score;

            previous_state_cell = lrc_ctc_trellis_previous_state_cell(
                trellis,
                frame,
                state
            );
            ASSERT(previous_state_cell);
            *previous_state_cell = best_previous_state;
        }
    }

    lrc_ctc_align_graph_destroy(&graph);

    return true;
}

static bool
lrc_ctc_trellis_score_forward_with_plan(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    LrcCtcAlignPlan *plan,
    LrcCtcAlignResult *result
) {
    if (lrc_ctc_align_plan_missing(plan, result)) {
        return false;
    }

    return lrc_ctc_trellis_score_forward_for_mode(
        trellis,
        emissions,
        plan->target_token_ids,
        plan->target_segment_starts,
        plan->target_token_count,
        plan->blank_token_id,
        plan->star_mode,
        plan->star_token_id,
        result
    );
}

static bool
lrc_ctc_trellis_ready_for_backtracking(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 target_token_count,
    enum LrcCtcAlignStarMode star_mode,
    bool *target_segment_starts,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    int32 state_count;
    int64 expected_cell_count;
    bool has_edge_stars;
    bool has_segment_stars;

    if (trellis == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC trellis is missing",
            -1,
            -1
        );
        return false;
    }
    if ((trellis->scores == NULL) || (trellis->previous_states == NULL)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TRELLIS,
            "CTC trellis has not been scored",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_graph_state_count_for_mode(target_token_count,
                                                  star_mode,
                                                  target_segment_starts,
                                                  &state_count,
                                                  result)) {
        return false;
    }

    has_edge_stars = star_mode == LRC_CTC_ALIGN_STAR_MODE_EDGES;
    has_segment_stars = star_mode == LRC_CTC_ALIGN_STAR_MODE_SEGMENT;
    if ((trellis->frame_count != emissions->frame_count)
        || (trellis->target_token_count != target_token_count)
        || (trellis->state_count != state_count)
        || (trellis->has_edge_stars != has_edge_stars)
        || (trellis->has_segment_stars != has_segment_stars)
        || (trellis->star_token_id != star_token_id)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TRELLIS,
            "CTC trellis dimensions do not match inputs",
            trellis->frame_count,
            trellis->target_token_count
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        trellis->frame_count,
        trellis->state_count,
        &expected_cell_count,
        "CTC trellis dimensions are too large",
        result
    )) {
        return false;
    }
    if (trellis->cell_count != expected_cell_count) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TRELLIS,
            "CTC trellis cell count does not match dimensions",
            trellis->frame_count,
            trellis->state_count
        );
        return false;
    }

    return true;
}

static void
lrc_ctc_path_set_blank_step(
    LrcCtcPath *path,
    int32 frame_index,
    int32 state_index,
    int32 blank_token_id
) {
    ASSERT(path);
    ASSERT(path->steps);
    ASSERT(frame_index >= 0);
    ASSERT(frame_index < path->step_count);
    ASSERT(state_index >= 0);

    path->steps[frame_index].frame_index = frame_index;
    path->steps[frame_index].state_index = state_index;
    path->steps[frame_index].token_index = -1;
    path->steps[frame_index].token_id = blank_token_id;
    path->steps[frame_index].is_blank = true;
    path->steps[frame_index].is_star = false;

    return;
}

static void
lrc_ctc_path_set_star_step(
    LrcCtcPath *path,
    int32 frame_index,
    int32 state_index,
    int32 star_token_id
) {
    ASSERT(path);
    ASSERT(path->steps);
    ASSERT(frame_index >= 0);
    ASSERT(frame_index < path->step_count);
    ASSERT(state_index >= 0);
    ASSERT(star_token_id >= 0);

    path->steps[frame_index].frame_index = frame_index;
    path->steps[frame_index].state_index = state_index;
    path->steps[frame_index].token_index = -1;
    path->steps[frame_index].token_id = star_token_id;
    path->steps[frame_index].is_blank = false;
    path->steps[frame_index].is_star = true;

    return;
}

static void
lrc_ctc_path_set_token_step(
    LrcCtcPath *path,
    int32 frame_index,
    int32 state_index,
    int32 token_index,
    int32 token_id
) {
    ASSERT(path);
    ASSERT(path->steps);
    ASSERT(frame_index >= 0);
    ASSERT(frame_index < path->step_count);
    ASSERT(state_index >= 0);
    ASSERT(token_index >= 0);

    path->steps[frame_index].frame_index = frame_index;
    path->steps[frame_index].state_index = state_index;
    path->steps[frame_index].token_index = token_index;
    path->steps[frame_index].token_id = token_id;
    path->steps[frame_index].is_blank = false;
    path->steps[frame_index].is_star = false;

    return;
}

static void
lrc_ctc_path_set_graph_state_step(
    LrcCtcPath *path,
    LrcCtcAlignGraph *graph,
    int32 frame_index,
    int32 state_index,
    int32 blank_token_id
) {
    LrcCtcAlignState *state;

    ASSERT(path);
    ASSERT(path->steps);
    ASSERT(lrc_ctc_align_graph_state_valid(graph, state_index));
    ASSERT(frame_index >= 0);
    ASSERT(frame_index < path->step_count);

    state = graph->states + state_index;
    if (state->kind == LRC_CTC_ALIGN_STATE_BLANK) {
        lrc_ctc_path_set_blank_step(path,
                                    frame_index,
                                    state_index,
                                    blank_token_id);
        return;
    }

    if (state->kind == LRC_CTC_ALIGN_STATE_STAR) {
        lrc_ctc_path_set_star_step(path,
                                   frame_index,
                                   state_index,
                                   state->token_id);
        return;
    }

    ASSERT(state->kind == LRC_CTC_ALIGN_STATE_TOKEN);
    lrc_ctc_path_set_token_step(path,
                                frame_index,
                                state_index,
                                state->token_index,
                                state->token_id);

    return;
}

static bool
lrc_ctc_trellis_best_final_state(
    LrcCtcTrellis *trellis,
    int32 *final_state,
    LrcCtcAlignResult *result
) {
    int32 final_blank_state;
    int32 final_token_state;
    float *blank_cell;
    float *token_cell;

    ASSERT(trellis);
    ASSERT(trellis->scores);
    ASSERT(final_state);

    final_blank_state = trellis->state_count - 1;
    final_token_state = trellis->state_count - 2;

    blank_cell = lrc_ctc_trellis_cell(trellis,
                                      trellis->frame_count - 1,
                                      final_blank_state);
    token_cell = lrc_ctc_trellis_cell(trellis,
                                      trellis->frame_count - 1,
                                      final_token_state);
    ASSERT(blank_cell);
    ASSERT(token_cell);

    if (!isfinite(*blank_cell) && !isfinite(*token_cell)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_IMPOSSIBLE_ALIGNMENT,
            "CTC target tokens cannot fit in the available frames",
            trellis->frame_count - 1,
            trellis->target_token_count - 1
        );
        return false;
    }

    *final_state = final_token_state;
    if (*blank_cell > *token_cell) {
        *final_state = final_blank_state;
    }

    return true;
}

static bool
lrc_ctc_trellis_backtrack_for_mode(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 blank_token_id,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id,
    LrcCtcPath *path,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignGraph graph = {0};
    int32 state;
    int32 frame;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_trellis_emissions_ready(emissions,
                                         blank_token_id,
                                         result)) {
        return false;
    }
    if (!lrc_ctc_target_tokens_valid_for_mode(emissions,
                                              target_token_ids,
                                              target_token_count,
                                              blank_token_id,
                                              star_mode,
                                              star_token_id,
                                              result)) {
        return false;
    }
    if (!lrc_ctc_trellis_ready_for_backtracking(trellis,
                                                emissions,
                                                target_token_count,
                                                star_mode,
                                                target_segment_starts,
                                                star_token_id,
                                                result)) {
        return false;
    }
    if (!lrc_ctc_path_allocate(path, trellis->frame_count, result)) {
        return false;
    }

    if (!lrc_ctc_align_graph_build_for_mode(&graph,
                                            target_token_ids,
                                            target_token_count,
                                            star_mode,
                                            target_segment_starts,
                                            star_token_id,
                                            result)) {
        lrc_ctc_path_destroy(path);
        return false;
    }
    if (!lrc_ctc_trellis_best_final_state(trellis, &state, result)) {
        lrc_ctc_align_graph_destroy(&graph);
        lrc_ctc_path_destroy(path);
        return false;
    }

    frame = trellis->frame_count - 1;
    while (true) {
        int32 *previous_state_cell;
        int32 previous_state;

        lrc_ctc_path_set_graph_state_step(path,
                                          &graph,
                                          frame,
                                          state,
                                          blank_token_id);
        if (frame == 0) {
            break;
        }

        previous_state_cell = lrc_ctc_trellis_previous_state_cell(trellis,
                                                                  frame,
                                                                  state);
        ASSERT(previous_state_cell);
        previous_state = *previous_state_cell;
        if (!lrc_ctc_align_graph_transition_allowed(&graph,
                                                     previous_state,
                                                     state)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_TRELLIS,
                "CTC trellis previous-state backpointer is invalid",
                frame,
                state
            );
            lrc_ctc_align_graph_destroy(&graph);
            lrc_ctc_path_destroy(path);
            return false;
        }

        state = previous_state;
        frame -= 1;
    }

    lrc_ctc_align_graph_destroy(&graph);

    return true;
}

static bool
lrc_ctc_trellis_backtrack_with_plan(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    LrcCtcAlignPlan *plan,
    LrcCtcPath *path,
    LrcCtcAlignResult *result
) {
    if (lrc_ctc_align_plan_missing(plan, result)) {
        return false;
    }

    return lrc_ctc_trellis_backtrack_for_mode(
        trellis,
        emissions,
        plan->target_token_ids,
        plan->target_segment_starts,
        plan->target_token_count,
        plan->blank_token_id,
        plan->star_mode,
        plan->star_token_id,
        path,
        result
    );
}

static bool
lrc_ctc_path_step_valid(
    LrcCtcPathStep *step,
    LrcCtcEmissions *emissions,
    LrcCtcAlignResult *result
) {
    if ((step->frame_index < 0)
        || (step->frame_index >= emissions->frame_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path frame index is outside emissions",
            step->frame_index,
            step->token_index
        );
        return false;
    }
    if (step->state_index < 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path state index is invalid",
            step->frame_index,
            step->token_index
        );
        return false;
    }
    if (step->is_blank) {
        return true;
    }
    if (step->is_star) {
        if ((step->token_index != -1)
            || ((int32)step->token_id != emissions->vocabulary_size)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC path star token is invalid",
                step->frame_index,
                step->token_index
            );
            return false;
        }
        return true;
    }
    if (step->token_index < 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path token index is invalid",
            step->frame_index,
            step->token_index
        );
        return false;
    }
    if ((step->token_id < 0)
        || ((int32)step->token_id >= emissions->vocabulary_size)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path token id is outside emissions",
            step->frame_index,
            step->token_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_path_ready_for_spans(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    float frame_duration_seconds,
    LrcCtcAlignResult *result
) {
    if (path == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC path is missing",
            -1,
            -1
        );
        return false;
    }
    if ((path->steps == NULL) || (path->step_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path has no steps",
            -1,
            -1
        );
        return false;
    }
    if (!isfinite(frame_duration_seconds)
        || (frame_duration_seconds <= 0.0f)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_FRAME_DURATION,
            "CTC frame duration must be positive and finite",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_emissions_ready(emissions, result)) {
        return false;
    }

    for (int32 i = 0; i < path->step_count; i += 1) {
        if (!lrc_ctc_path_step_valid(path->steps + i, emissions, result)) {
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_path_steps_share_label(
    LrcCtcPathStep *a,
    LrcCtcPathStep *b
) {
    ASSERT(a);
    ASSERT(b);

    if (a->is_blank && b->is_blank) {
        return true;
    }
    if (a->is_star && b->is_star) {
        return true;
    }
    if (a->is_blank || b->is_blank || a->is_star || b->is_star) {
        return false;
    }

    return a->token_id == b->token_id;
}

static int32
lrc_ctc_path_count_segments(LrcCtcPath *path) {
    int32 count;

    ASSERT(path);
    ASSERT(path->steps);
    ASSERT(path->step_count > 0);

    count = 1;
    for (int32 i = 1; i < path->step_count; i += 1) {
        if (!lrc_ctc_path_steps_share_label(path->steps + i - 1,
                                            path->steps + i)) {
            count += 1;
        }
    }

    return count;
}

static bool
lrc_ctc_path_step_score(
    LrcCtcPathStep *step,
    LrcCtcEmissions *emissions,
    float *score,
    LrcCtcAlignResult *result
) {
    ASSERT(step);
    ASSERT(emissions);
    ASSERT(score);

    if (step->token_id < 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path step token id is invalid",
            step->frame_index,
            step->token_index
        );
        return false;
    }
    if (((int32)step->token_id > emissions->vocabulary_size)
        || (!step->is_star
            && ((int32)step->token_id >= emissions->vocabulary_size))) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path step token id is outside emissions",
            step->frame_index,
            step->token_index
        );
        return false;
    }

    *score = lrc_ctc_emission_value(emissions,
                                    step->frame_index,
                                    step->token_id);

    return true;
}

static void
lrc_ctc_path_segment_finish(
    LrcCtcPathSegment *segment,
    int32 score_count,
    float score_sum,
    float frame_duration_seconds
) {
    ASSERT(segment);
    ASSERT(segment->start_frame >= 0);
    ASSERT(segment->end_frame > segment->start_frame);
    ASSERT(score_count > 0);

    segment->start_seconds = (float)segment->start_frame*frame_duration_seconds;
    segment->end_seconds = (float)segment->end_frame*frame_duration_seconds;
    segment->score = score_sum/(float)score_count;

    return;
}

static bool
lrc_ctc_path_to_segments(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    float frame_duration_seconds,
    LrcCtcPathSegments *segments,
    LrcCtcAlignResult *result
) {
    int32 segment_count;
    int32 segment_index;
    int32 score_count;
    float score_sum;
    LrcCtcPathSegment *segment;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_path_ready_for_spans(path,
                                      emissions,
                                      frame_duration_seconds,
                                      result)) {
        return false;
    }

    segment_count = lrc_ctc_path_count_segments(path);
    if (!lrc_ctc_path_segments_allocate(segments, segment_count, result)) {
        return false;
    }

    segment_index = -1;
    score_count = 0;
    score_sum = 0.0f;
    segment = NULL;
    for (int32 i = 0; i < path->step_count; i += 1) {
        LrcCtcPathStep *step = path->steps + i;
        float score;

        if ((i == 0)
            || !lrc_ctc_path_steps_share_label(path->steps + i - 1,
                                               step)) {
            if (segment) {
                lrc_ctc_path_segment_finish(segment,
                                            score_count,
                                            score_sum,
                                            frame_duration_seconds);
            }

            segment_index += 1;
            ASSERT(segment_index < segments->segment_count);
            segment = segments->segments + segment_index;
            segment->token_index = step->token_index;
            segment->start_frame = step->frame_index;
            segment->end_frame = step->frame_index + 1;
            segment->token_id = step->token_id;
            segment->is_blank = step->is_blank;
            segment->is_star = step->is_star;
            score_count = 0;
            score_sum = 0.0f;
        }

        ASSERT(segment);
        if (step->frame_index + 1 > segment->end_frame) {
            segment->end_frame = step->frame_index + 1;
        }
        if (!lrc_ctc_path_step_score(step, emissions, &score, result)) {
            lrc_ctc_path_segments_destroy(segments);
            return false;
        }
        score_sum += score;
        score_count += 1;
    }

    if (segment) {
        lrc_ctc_path_segment_finish(segment,
                                    score_count,
                                    score_sum,
                                    frame_duration_seconds);
    }
    ASSERT(segment_index + 1 == segments->segment_count);

    return true;
}

static bool
lrc_ctc_path_segment_valid_for_intervals(
    LrcCtcPathSegment *segment,
    int32 segment_index,
    int32 previous_end_frame,
    LrcCtcAlignResult *result
) {
    if ((segment->start_frame < 0)
        || (segment->end_frame <= segment->start_frame)
        || (segment->start_frame < previous_end_frame)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path segment frame range is invalid",
            segment->start_frame,
            segment_index
        );
        return false;
    }
    if (segment->is_blank) {
        if (segment->is_star || (segment->token_index != -1)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC blank path segment has invalid labels",
                segment->start_frame,
                segment_index
            );
            return false;
        }
        return true;
    }
    if (segment->is_star) {
        if ((segment->token_id < 0) || (segment->token_index != -1)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC star path segment has invalid labels",
                segment->start_frame,
                segment_index
            );
            return false;
        }
        return true;
    }
    if ((segment->token_index < 0) || (segment->token_id < 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC token path segment has invalid labels",
            segment->start_frame,
            segment_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_path_segments_ready_for_intervals(
    LrcCtcPathSegments *segments,
    LrcCtcAlignedTokenIntervals *intervals,
    LrcCtcAlignResult *result
) {
    int32 previous_end_frame;

    if ((segments == NULL) || (intervals == NULL)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC segment interval conversion received invalid arguments",
            -1,
            -1
        );
        return false;
    }
    if ((segments->segments == NULL) || (segments->segment_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path segments are empty",
            -1,
            -1
        );
        return false;
    }

    previous_end_frame = -1;
    for (int32 i = 0; i < segments->segment_count; i += 1) {
        LrcCtcPathSegment *segment = segments->segments + i;

        if (!lrc_ctc_path_segment_valid_for_intervals(segment,
                                                     i,
                                                     previous_end_frame,
                                                     result)) {
            return false;
        }
        previous_end_frame = segment->end_frame;
    }

    return true;
}

static bool
lrc_ctc_interval_segment_matches_state(
    LrcCtcPathSegment *segment,
    LrcCtcAlignState *state,
    int32 segment_index,
    int32 label_index,
    LrcCtcAlignResult *result
) {
    if (state->kind == LRC_CTC_ALIGN_STATE_STAR) {
        if (!segment->is_star || (segment->token_id != state->token_id)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC path segment does not match target star",
                segment->start_frame,
                label_index
            );
            return false;
        }
        return true;
    }
    if (state->kind != LRC_CTC_ALIGN_STATE_TOKEN) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC interval target stream contains an invalid label",
            segment->start_frame,
            label_index
        );
        return false;
    }
    if (segment->is_star || (segment->token_id != state->token_id)
        || (segment->token_index != state->token_index)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path segment does not match target token",
            segment->start_frame,
            segment_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_path_segments_to_aligned_token_intervals(
    LrcCtcPathSegments *segments,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id,
    LrcCtcAlignedTokenIntervals *intervals,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignGraph graph = {0};
    int32 label_count;
    int32 label_index;
    bool ok;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_path_segments_ready_for_intervals(segments,
                                                   intervals,
                                                   result)) {
        return false;
    }
    if ((star_mode != LRC_CTC_ALIGN_STAR_MODE_NONE)
        && (star_token_id < 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN,
            "CTC interval star token id is invalid",
            -1,
            star_token_id
        );
        return false;
    }

    if (!lrc_ctc_align_graph_build_for_mode(&graph,
                                            target_token_ids,
                                            target_token_count,
                                            star_mode,
                                            target_segment_starts,
                                            star_token_id,
                                            result)) {
        lrc_ctc_align_graph_destroy(&graph);
        return false;
    }

    label_count = (graph.state_count - 1)/2;
    if (!lrc_ctc_aligned_token_intervals_allocate(intervals,
                                                  label_count,
                                                  result)) {
        lrc_ctc_align_graph_destroy(&graph);
        return false;
    }

    ok = true;
    label_index = 0;
    for (int32 i = 0; i < segments->segment_count; i += 1) {
        LrcCtcAlignedTokenInterval *interval;
        LrcCtcPathSegment *segment = segments->segments + i;
        LrcCtcAlignState *state;

        if (segment->is_blank) {
            continue;
        }
        if (label_index >= label_count) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC path segments contain too many target labels",
                segment->start_frame,
                i
            );
            ok = false;
            break;
        }

        state = graph.states + 2*label_index + 1;
        if (!lrc_ctc_interval_segment_matches_state(segment,
                                                    state,
                                                    i,
                                                    label_index,
                                                    result)) {
            ok = false;
            break;
        }

        interval = intervals->intervals + label_index;
        interval->target_token_index = state->token_index;
        interval->segment_start_index = i;
        interval->segment_end_index = i + 1;
        interval->token_start_frame = segment->start_frame;
        interval->token_end_frame = segment->end_frame;
        interval->padded_start_frame = segment->start_frame;
        interval->padded_end_frame = segment->end_frame;
        interval->padded_start_seconds = segment->start_seconds;
        interval->padded_end_seconds = segment->end_seconds;
        interval->is_star = state->kind == LRC_CTC_ALIGN_STATE_STAR;

        label_index += 1;
    }
    if (ok && (label_index != label_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path segments do not cover all target labels",
            -1,
            label_index
        );
        ok = false;
    }

    if (!ok) {
        lrc_ctc_aligned_token_intervals_destroy(intervals);
    }
    lrc_ctc_align_graph_destroy(&graph);

    return ok;
}


static bool
lrc_ctc_aligned_token_interval_valid_for_padding(
    LrcCtcAlignedTokenInterval *interval,
    int32 interval_index,
    int32 segment_count,
    LrcCtcAlignResult *result
) {
    if ((interval->segment_start_index < 0)
        || (interval->segment_end_index <= interval->segment_start_index)
        || (interval->segment_end_index > segment_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC aligned token interval segment range is invalid",
            -1,
            interval_index
        );
        return false;
    }
    if ((interval->token_start_frame < 0)
        || (interval->token_end_frame <= interval->token_start_frame)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC aligned token interval frame range is invalid",
            interval->token_start_frame,
            interval_index
        );
        return false;
    }
    if (interval->is_star) {
        if (interval->target_token_index != -1) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC aligned star interval has target token index",
                interval->token_start_frame,
                interval_index
            );
            return false;
        }
        return true;
    }
    if (interval->target_token_index < 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC aligned token interval target index is invalid",
            interval->token_start_frame,
            interval_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_aligned_token_intervals_ready_for_padding(
    LrcCtcPathSegments *segments,
    float frame_duration_seconds,
    LrcCtcAlignedTokenIntervals *intervals,
    LrcCtcAlignResult *result
) {
    if (!isfinite(frame_duration_seconds)
        || (frame_duration_seconds <= 0.0f)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_FRAME_DURATION,
            "CTC frame duration must be positive and finite",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_path_segments_ready_for_intervals(segments,
                                                   intervals,
                                                   result)) {
        return false;
    }
    if ((intervals->intervals == NULL)
        || (intervals->interval_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS,
            "CTC aligned token intervals are empty",
            -1,
            -1
        );
        return false;
    }

    for (int32 i = 0; i < intervals->interval_count; i += 1) {
        if (!lrc_ctc_aligned_token_interval_valid_for_padding(
            intervals->intervals + i,
            i,
            segments->segment_count,
            result
        )) {
            return false;
        }
    }

    return true;
}

static int32
lrc_ctc_blank_midpoint_frame(LrcCtcPathSegment *segment) {
    ASSERT(segment);
    ASSERT(segment->is_blank);
    ASSERT(segment->start_frame >= 0);
    ASSERT(segment->end_frame > segment->start_frame);

    return (segment->start_frame + segment->end_frame)/2;
}

static bool
lrc_ctc_pad_token_intervals_with_blanks(
    LrcCtcPathSegments *segments,
    float frame_duration_seconds,
    LrcCtcAlignedTokenIntervals *intervals,
    LrcCtcAlignResult *result
) {
    LrcCtcPathSegment *path_segments;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_aligned_token_intervals_ready_for_padding(
        segments,
        frame_duration_seconds,
        intervals,
        result
    )) {
        return false;
    }

    path_segments = segments->segments;
    if (path_segments == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC path segments are empty",
            -1,
            -1
        );
        return false;
    }

    for (int32 i = 0; i < intervals->interval_count; i += 1) {
        LrcCtcAlignedTokenInterval *interval = intervals->intervals + i;
        LrcCtcPathSegment *previous = NULL;
        LrcCtcPathSegment *next;
        int32 padded_start_frame = interval->token_start_frame;
        int32 padded_end_frame = interval->token_end_frame;


        if (interval->segment_start_index > 0) {
            previous = path_segments + interval->segment_start_index - 1;
        }
        if (previous && previous->is_blank) {
            if (i == 0) {
                padded_start_frame = previous->start_frame;
            } else {
                padded_start_frame = lrc_ctc_blank_midpoint_frame(previous);
            }
        }

        next = NULL;
        if (interval->segment_end_index < segments->segment_count) {
            next = path_segments + interval->segment_end_index;
        }
        if (next && next->is_blank) {
            if (i == intervals->interval_count - 1) {
                padded_end_frame = next->end_frame;
            } else {
                padded_end_frame = lrc_ctc_blank_midpoint_frame(next);
            }
        }

        if (padded_end_frame < padded_start_frame) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC padded token interval frame range is invalid",
                padded_start_frame,
                i
            );
            return false;
        }

        interval->padded_start_frame = padded_start_frame;
        interval->padded_end_frame = padded_end_frame;
        interval->padded_start_seconds = (
            (float)padded_start_frame*frame_duration_seconds
        );
        interval->padded_end_seconds = (
            (float)padded_end_frame*frame_duration_seconds
        );
    }

    return true;
}

static bool
lrc_ctc_path_step_starts_span(
    LrcCtcPath *path,
    int32 step_index
) {
    LrcCtcPathStep *step;
    LrcCtcPathStep *previous;

    ASSERT(path);
    ASSERT(path->steps);
    ASSERT(step_index >= 0);
    ASSERT(step_index < path->step_count);

    step = path->steps + step_index;
    if (step->is_blank || step->is_star) {
        return false;
    }
    if (step_index <= 0) {
        return true;
    }

    previous = path->steps + step_index - 1;
    if (previous->is_blank || previous->is_star) {
        return true;
    }

    return previous->token_index != step->token_index;
}

static int32
lrc_ctc_path_count_token_spans(LrcCtcPath *path) {
    int32 count;

    ASSERT(path);
    ASSERT(path->steps);

    count = 0;
    for (int32 i = 0; i < path->step_count; i += 1) {
        if (lrc_ctc_path_step_starts_span(path, i)) {
            count += 1;
        }
    }

    return count;
}

static void
lrc_ctc_token_span_finish(
    LrcCtcTokenSpan *span,
    int32 score_count,
    float score_sum,
    float frame_duration_seconds
) {
    ASSERT(span);
    ASSERT(span->start_frame >= 0);
    ASSERT(span->end_frame > span->start_frame);
    ASSERT(score_count > 0);

    span->start_seconds = (float)span->start_frame*frame_duration_seconds;
    span->end_seconds = (float)span->end_frame*frame_duration_seconds;
    span->padded_start_frame = span->start_frame;
    span->padded_end_frame = span->end_frame;
    span->padded_start_seconds = span->start_seconds;
    span->padded_end_seconds = span->end_seconds;
    span->score = score_sum/(float)score_count;

    return;
}

static bool
lrc_ctc_path_to_token_spans(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    float frame_duration_seconds,
    LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result
) {
    int32 span_count;
    int32 span_index;
    int32 score_count;
    int32 previous_token_index;
    float score_sum;
    LrcCtcTokenSpan *span;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_path_ready_for_spans(path,
                                      emissions,
                                      frame_duration_seconds,
                                      result)) {
        return false;
    }

    span_count = lrc_ctc_path_count_token_spans(path);
    if (!lrc_ctc_token_spans_allocate(spans, span_count, result)) {
        return false;
    }

    span_index = -1;
    score_count = 0;
    previous_token_index = -1;
    score_sum = 0.0f;
    span = NULL;
    for (int32 i = 0; i < path->step_count; i += 1) {
        LrcCtcPathStep *step = path->steps + i;
        float score;

        if (step->is_blank || step->is_star) {
            if (span) {
                lrc_ctc_token_span_finish(span,
                                          score_count,
                                          score_sum,
                                          frame_duration_seconds);
                span = NULL;
                score_count = 0;
                score_sum = 0.0f;
            }
            continue;
        }

        if (lrc_ctc_path_step_starts_span(path, i)) {
            if (span) {
                lrc_ctc_token_span_finish(span,
                                          score_count,
                                          score_sum,
                                          frame_duration_seconds);
            }

            if (step->token_index != previous_token_index + 1) {
                lrc_ctc_align_result_set(
                    result,
                    LS_ERROR_CTC_ALIGN_INVALID_PATH,
                    "CTC path token states are not target ordered",
                    step->frame_index,
                    step->token_index
                );
                lrc_ctc_token_spans_destroy(spans);
                return false;
            }

            span_index += 1;
            previous_token_index = step->token_index;
            ASSERT(span_index < spans->span_count);
            span = spans->spans + span_index;
            span->token_index = step->token_index;
            span->start_frame = step->frame_index;
            span->end_frame = step->frame_index + 1;
            span->token_id = step->token_id;
            score_count = 0;
            score_sum = 0.0f;
        }

        ASSERT(span);
        if (step->frame_index + 1 > span->end_frame) {
            span->end_frame = step->frame_index + 1;
        }

        score = lrc_ctc_emission_value(emissions,
                                       step->frame_index,
                                       step->token_id);
        score_sum += score;
        score_count += 1;
    }

    if (span) {
        lrc_ctc_token_span_finish(span,
                                  score_count,
                                  score_sum,
                                  frame_duration_seconds);
    }
    ASSERT(span_index + 1 == spans->span_count);

    return true;
}

static bool
lrc_ctc_token_span_has_padded_timing(LrcCtcTokenSpan *span) {
    ASSERT(span);

    if ((span->padded_start_frame < 0) && (span->padded_end_frame < 0)) {
        return false;
    }

    return true;
}

static float
lrc_ctc_token_span_start_seconds(LrcCtcTokenSpan *span) {
    ASSERT(span);

    return span->start_seconds;
}

static float
lrc_ctc_token_span_end_seconds(LrcCtcTokenSpan *span) {
    ASSERT(span);

    return span->end_seconds;
}

static bool
lrc_ctc_token_span_padded_timing_valid(
    LrcCtcTokenSpan *span,
    int32 span_index,
    LrcCtcAlignResult *result
) {
    if (!lrc_ctc_token_span_has_padded_timing(span)) {
        return true;
    }
    if ((span->padded_start_frame < 0)
        || (span->padded_end_frame < span->padded_start_frame)
        || !isfinite(span->padded_start_seconds)
        || !isfinite(span->padded_end_seconds)
        || (span->padded_end_seconds < span->padded_start_seconds)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC padded token span timing is invalid",
            span->padded_start_frame,
            span_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_token_span_apply_padded_interval(
    LrcCtcTokenSpan *span,
    LrcCtcAlignedTokenInterval *interval,
    int32 span_index,
    int32 interval_index,
    LrcCtcAlignResult *result
) {
    if ((span->token_index != interval->target_token_index)
        || (span->start_frame != interval->token_start_frame)
        || (span->end_frame != interval->token_end_frame)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC padded interval does not match token span",
            interval->token_start_frame,
            span_index
        );
        return false;
    }
    if ((interval->padded_start_frame < 0)
        || (interval->padded_end_frame < interval->padded_start_frame)
        || !isfinite(interval->padded_start_seconds)
        || !isfinite(interval->padded_end_seconds)
        || (interval->padded_end_seconds < interval->padded_start_seconds)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC padded interval timing is invalid",
            interval->padded_start_frame,
            interval_index
        );
        return false;
    }

    span->padded_start_frame = interval->padded_start_frame;
    span->padded_end_frame = interval->padded_end_frame;
    span->padded_start_seconds = interval->padded_start_seconds;
    span->padded_end_seconds = interval->padded_end_seconds;

    return true;
}

static bool
lrc_ctc_token_spans_apply_padded_intervals(
    LrcCtcTokenSpans *token_spans,
    LrcCtcAlignedTokenIntervals *intervals,
    LrcCtcAlignResult *result
) {
    int32 span_index;

    if ((token_spans == NULL) || (intervals == NULL)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC padded-token conversion received invalid arguments",
            -1,
            -1
        );
        return false;
    }
    if ((token_spans->spans == NULL) || (token_spans->span_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token spans are empty",
            -1,
            -1
        );
        return false;
    }
    if ((intervals->intervals == NULL) || (intervals->interval_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC aligned token intervals are empty",
            -1,
            -1
        );
        return false;
    }

    span_index = 0;
    for (int32 i = 0; i < intervals->interval_count; i += 1) {
        LrcCtcAlignedTokenInterval *interval = intervals->intervals + i;
        LrcCtcTokenSpan *span;

        if (interval->is_star) {
            continue;
        }
        if (span_index >= token_spans->span_count) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_PATH,
                "CTC aligned token intervals have too many tokens",
                -1,
                i
            );
            return false;
        }

        span = token_spans->spans + span_index;
        if (!lrc_ctc_token_span_apply_padded_interval(span,
                                                      interval,
                                                      span_index,
                                                      i,
                                                      result)) {
            return false;
        }
        span_index += 1;
    }
    if (span_index != token_spans->span_count) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_PATH,
            "CTC aligned token intervals do not cover token spans",
            -1,
            span_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_path_to_padded_token_spans_for_mode(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    enum LrcCtcAlignStarMode star_mode,
    int32 star_token_id,
    float frame_duration_seconds,
    LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result
) {
    LrcCtcPathSegments segments = {0};
    LrcCtcAlignedTokenIntervals intervals = {0};
    bool ok;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if ((target_token_ids == NULL) || (target_token_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC padded-token conversion target stream is missing",
            -1,
            -1
        );
        return false;
    }


    ok = lrc_ctc_path_to_token_spans(path,
                                     emissions,
                                     frame_duration_seconds,
                                     spans,
                                     result);
    if (ok) {
        ok = lrc_ctc_path_to_segments(path,
                                      emissions,
                                      frame_duration_seconds,
                                      &segments,
                                      result);
    }
    if (ok) {
        ok = lrc_ctc_path_segments_to_aligned_token_intervals(
            &segments,
            target_token_ids,
            target_segment_starts,
            target_token_count,
            star_mode,
            star_token_id,
            &intervals,
            result
        );
    }
    if (ok) {
        ok = lrc_ctc_pad_token_intervals_with_blanks(
            &segments,
            frame_duration_seconds,
            &intervals,
            result
        );
    }
    if (ok) {
        ok = lrc_ctc_token_spans_apply_padded_intervals(spans,
                                                        &intervals,
                                                        result);
    }

    lrc_ctc_aligned_token_intervals_destroy(&intervals);
    lrc_ctc_path_segments_destroy(&segments);
    if (!ok) {
        lrc_ctc_token_spans_destroy(spans);
    }

    return ok;
}

static bool
lrc_ctc_path_to_padded_token_spans_with_plan(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    LrcCtcAlignPlan *plan,
    float frame_duration_seconds,
    LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result
) {
    if (lrc_ctc_align_plan_missing(plan, result)) {
        return false;
    }

    return lrc_ctc_path_to_padded_token_spans_for_mode(
        path,
        emissions,
        plan->target_token_ids,
        plan->target_segment_starts,
        plan->target_token_count,
        plan->star_mode,
        plan->star_token_id,
        frame_duration_seconds,
        spans,
        result
    );
}

static bool
lrc_ctc_word_spans_allocate(
    LrcCtcWordSpans *spans,
    int32 span_count,
    LrcCtcAlignResult *result
) {
    int64 alloc_size;

    if (spans == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC word spans destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (span_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC token spans did not produce words",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        span_count,
        SIZEOF(*spans->spans),
        &alloc_size,
        "CTC word span allocation is too large",
        result
    )) {
        return false;
    }

    lrc_ctc_word_spans_destroy(spans);
    ARRAY_INIT_COUNT(spans->spans, span_count);
    spans->span_count = span_count;

    for (int32 i = 0; i < spans->span_count; i += 1) {
        spans->spans[i].word_index = -1;
        spans->spans[i].token_start_index = -1;
        spans->spans[i].token_end_index = -1;
        spans->spans[i].span_start_index = -1;
        spans->spans[i].span_end_index = -1;

        spans->spans[i].normalized_start = -1;
        spans->spans[i].normalized_end = -1;
        spans->spans[i].line_index = -1;

        spans->spans[i].start_seconds = 0.0f;
        spans->spans[i].end_seconds = 0.0f;
        spans->spans[i].score = -INFINITY;
    }

    return true;
}

static bool
lrc_ctc_word_inputs_ready(
    LrcCtcTokenSpans *token_spans,
    LrcCtcTokenizedText *tokens,
    LrcLyricsNormalized *normalized,
    LrcCtcWordSpans *word_spans,
    LrcCtcAlignResult *result
) {
    if ((token_spans == NULL) || (tokens == NULL)
        || (normalized == NULL) || (word_spans == NULL)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC word-span conversion received invalid arguments",
            -1,
            -1
        );
        return false;
    }
    if ((token_spans->spans == NULL) || (token_spans->span_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token spans are empty",
            -1,
            -1
        );
        return false;
    }
    if ((tokens->tokens == NULL) || (tokens->token_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC tokenized text is empty",
            -1,
            -1
        );
        return false;
    }
    if ((normalized->text == NULL) || (normalized->text_len <= 0)
        || (normalized->byte_count != normalized->text_len)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_NORMALIZED_TEXT,
            "normalized lyrics are not ready",
            -1,
            -1
        );
        return false;
    }
    if (token_spans->span_count != tokens->token_count) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token spans must match tokenized text",
            token_spans->span_count,
            tokens->token_count
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_token_range_valid(
    LrcCtcTextToken *token,
    LrcLyricsNormalized *normalized,
    int32 token_index,
    LrcCtcAlignResult *result
) {
    if ((token->normalized_start < 0)
        || (token->normalized_end <= token->normalized_start)
        || (token->normalized_end > normalized->text_len)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC token normalized range is invalid",
            token->normalized_start,
            token_index
        );
        return false;
    }
    if ((token->line_index < 0)
        || (token->line_index >= normalized->line_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC token line index is invalid",
            -1,
            token_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_token_span_resolve_token(
    LrcCtcTokenSpan *span,
    LrcCtcTokenizedText *tokens,
    int32 span_index,
    int32 previous_token_index,
    LrcCtcTextToken **token_out,
    LrcCtcAlignResult *result
) {
    LrcCtcTextToken *token;

    ASSERT(token_out);
    *token_out = NULL;

    if ((span->token_index < 0)
        || (span->token_index >= tokens->token_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token span target index is outside tokenized text",
            -1,
            span_index
        );
        return false;
    }
    if (span->token_index != previous_token_index + 1) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token spans must cover target tokens in order",
            -1,
            span_index
        );
        return false;
    }

    token = tokens->tokens + span->token_index;
    if (span->token_id != token->token_id) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token span id does not match target token index",
            -1,
            span->token_index
        );
        return false;
    }
    if (!isfinite(span->start_seconds) || !isfinite(span->end_seconds)
        || (span->end_seconds < span->start_seconds)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token span timing is invalid",
            -1,
            span->token_index
        );
        return false;
    }
    if (!lrc_ctc_token_span_padded_timing_valid(span,
                                                span_index,
                                                result)) {
        return false;
    }
    if (!isfinite(span->score)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token span score is invalid",
            -1,
            span->token_index
        );
        return false;
    }

    *token_out = token;

    return true;
}

static bool
lrc_ctc_normalized_range_is_space(
    LrcLyricsNormalized *normalized,
    int32 start,
    int32 end
) {
    ASSERT(normalized);
    ASSERT(normalized->text);
    ASSERT(start >= 0);
    ASSERT(end > start);
    ASSERT(end <= normalized->text_len);

    for (int32 i = start; i < end; i += 1) {
        if (normalized->text[i] != ' ') {
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_normalized_range_has_space(
    LrcLyricsNormalized *normalized,
    int32 start,
    int32 end
) {
    ASSERT(normalized);
    ASSERT(normalized->text);
    ASSERT(start >= 0);
    ASSERT(end > start);
    ASSERT(end <= normalized->text_len);

    for (int32 i = start; i < end; i += 1) {
        if (normalized->text[i] == ' ') {
            return true;
        }
    }

    return false;
}

static bool
lrc_ctc_tokenized_text_uses_segments(
    LrcCtcTokenizedText *tokens,
    LrcLyricsNormalized *normalized
) {
    if ((tokens == NULL) || (normalized == NULL)) {
        return false;
    }
    if ((normalized->segments == NULL) || (normalized->segment_count <= 0)) {
        return false;
    }
    if ((tokens->tokens == NULL) || (tokens->token_count <= 0)) {
        return false;
    }

    for (int32 i = 0; i < tokens->token_count; i += 1) {
        if (tokens->tokens[i].segment_index < 0) {
            return false;
        }
    }

    return true;
}

static bool
lrc_ctc_token_segment_valid(
    LrcCtcTextToken *token,
    LrcLyricsNormalized *normalized,
    int32 token_index,
    CtcTextSegment **segment_out,
    LrcCtcAlignResult *result
) {
    CtcTextSegment *segment;

    ASSERT(segment_out);
    *segment_out = NULL;

    if ((token->segment_index < 0)
        || (token->segment_index >= normalized->segment_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC token segment index is invalid",
            -1,
            token_index
        );
        return false;
    }

    segment = normalized->segments + token->segment_index;
    if ((segment->normalized_start < 0)
        || (segment->normalized_end <= segment->normalized_start)
        || (segment->normalized_end > normalized->text_len)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_NORMALIZED_TEXT,
            "CTC segment normalized range is invalid",
            segment->normalized_start,
            token_index
        );
        return false;
    }
    if ((segment->line_index < 0)
        || (segment->line_index >= normalized->line_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_NORMALIZED_TEXT,
            "CTC segment line index is invalid",
            -1,
            token_index
        );
        return false;
    }
    if (token->line_index != segment->line_index) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC token line index does not match its segment",
            -1,
            token_index
        );
        return false;
    }
    if ((token->normalized_start < segment->normalized_start)
        || (token->normalized_end > segment->normalized_end)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC token normalized range is outside its segment",
            token->normalized_start,
            token_index
        );
        return false;
    }

    *segment_out = segment;

    return true;
}

static bool
lrc_ctc_segment_word_count(
    LrcCtcTokenSpans *token_spans,
    LrcCtcTokenizedText *tokens,
    LrcLyricsNormalized *normalized,
    int32 *word_count,
    LrcCtcAlignResult *result
) {
    int32 previous_token_index;
    int32 previous_segment_index;

    ASSERT(word_count);

    *word_count = 0;
    previous_token_index = -1;
    previous_segment_index = -1;
    for (int32 i = 0; i < token_spans->span_count; i += 1) {
        LrcCtcTokenSpan *span = token_spans->spans + i;
        LrcCtcTextToken *token;
        CtcTextSegment *segment;

        if (!lrc_ctc_token_span_resolve_token(span,
                                              tokens,
                                              i,
                                              previous_token_index,
                                              &token,
                                              result)) {
            return false;
        }
        if (!lrc_ctc_token_range_valid(token,
                                       normalized,
                                       span->token_index,
                                       result)) {
            return false;
        }
        if (!lrc_ctc_token_segment_valid(token,
                                         normalized,
                                         span->token_index,
                                         &segment,
                                         result)) {
            return false;
        }
        if (token->segment_index < previous_segment_index) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
                "CTC token segments must be ordered",
                -1,
                span->token_index
            );
            return false;
        }
        if (token->segment_index != previous_segment_index) {
            *word_count += 1;
            previous_segment_index = token->segment_index;
        }

        (void)segment;
        previous_token_index = span->token_index;
    }

    return true;
}

static void
lrc_ctc_segment_word_span_start(
    LrcCtcWordSpan *word,
    int32 word_index,
    int32 span_index,
    LrcCtcTokenSpan *token_span,
    CtcTextSegment *segment
) {
    word->word_index = word_index;
    word->token_start_index = token_span->token_index;
    word->token_end_index = token_span->token_index + 1;
    word->span_start_index = span_index;
    word->span_end_index = span_index + 1;

    word->normalized_start = segment->normalized_start;
    word->normalized_end = segment->normalized_end;
    word->line_index = segment->line_index;

    word->start_seconds = lrc_ctc_token_span_start_seconds(token_span);
    word->end_seconds = lrc_ctc_token_span_end_seconds(token_span);
    word->score = token_span->score;

    return;
}

static bool
lrc_ctc_segment_word_span_extend(
    LrcCtcWordSpan *word,
    int32 span_index,
    LrcCtcTokenSpan *token_span,
    CtcTextSegment *segment,
    int32 score_count,
    float *score_sum,
    LrcCtcAlignResult *result
) {
    if (segment->line_index != word->line_index) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC segment cannot cross lyric lines",
            segment->normalized_start,
            token_span->token_index
        );
        return false;
    }
    if ((lrc_ctc_token_span_start_seconds(token_span) + 0.00001f)
        < word->end_seconds) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token spans must be time ordered",
            -1,
            token_span->token_index
        );
        return false;
    }

    word->token_end_index = token_span->token_index + 1;
    word->span_end_index = span_index + 1;
    word->normalized_start = segment->normalized_start;
    word->normalized_end = segment->normalized_end;
    word->end_seconds = lrc_ctc_token_span_end_seconds(token_span);
    *score_sum += token_span->score;
    word->score = *score_sum/(float)score_count;

    return true;
}

static bool
lrc_ctc_token_spans_to_segment_word_spans(
    LrcCtcTokenSpans *token_spans,
    LrcCtcTokenizedText *tokens,
    LrcLyricsNormalized *normalized,
    LrcCtcWordSpans *word_spans,
    LrcCtcAlignResult *result
) {
    LrcCtcWordSpan *word;
    int32 word_count;
    int32 word_index;
    int32 score_count;
    int32 previous_token_index;
    int32 previous_segment_index;
    float score_sum;

    lrc_ctc_word_spans_destroy(word_spans);
    if (!lrc_ctc_segment_word_count(token_spans,
                                    tokens,
                                    normalized,
                                    &word_count,
                                    result)) {
        return false;
    }
    if (!lrc_ctc_word_spans_allocate(word_spans, word_count, result)) {
        return false;
    }

    word = NULL;
    word_index = -1;
    score_count = 0;
    score_sum = 0.0f;
    previous_token_index = -1;
    previous_segment_index = -1;
    for (int32 i = 0; i < token_spans->span_count; i += 1) {
        LrcCtcTokenSpan *token_span = token_spans->spans + i;
        LrcCtcTextToken *token;
        CtcTextSegment *segment;

        if (!lrc_ctc_token_span_resolve_token(token_span,
                                              tokens,
                                              i,
                                              previous_token_index,
                                              &token,
                                              result)) {
            lrc_ctc_word_spans_destroy(word_spans);
            return false;
        }
        if (!lrc_ctc_token_segment_valid(token,
                                         normalized,
                                         token_span->token_index,
                                         &segment,
                                         result)) {
            lrc_ctc_word_spans_destroy(word_spans);
            return false;
        }
        if (token->segment_index < previous_segment_index) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
                "CTC token segments must be ordered",
                -1,
                token_span->token_index
            );
            lrc_ctc_word_spans_destroy(word_spans);
            return false;
        }
        if ((word == NULL)
            || (token->segment_index != previous_segment_index)) {
            word_index += 1;
            ASSERT(word_index < word_spans->span_count);
            word = word_spans->spans + word_index;
            score_count = 1;
            score_sum = token_span->score;
            lrc_ctc_segment_word_span_start(word,
                                            word_index,
                                            i,
                                            token_span,
                                            segment);
            previous_segment_index = token->segment_index;
            previous_token_index = token_span->token_index;
            continue;
        }

        score_count += 1;
        if (!lrc_ctc_segment_word_span_extend(word,
                                              i,
                                              token_span,
                                              segment,
                                              score_count,
                                              &score_sum,
                                              result)) {
            lrc_ctc_word_spans_destroy(word_spans);
            return false;
        }
        previous_token_index = token_span->token_index;
    }
    ASSERT(word_index + 1 == word_spans->span_count);

    return true;
}

static bool
lrc_ctc_word_count(
    LrcCtcTokenSpans *token_spans,
    LrcCtcTokenizedText *tokens,
    LrcLyricsNormalized *normalized,
    int32 *word_count,
    LrcCtcAlignResult *result
) {
    bool in_word;
    int32 previous_token_index;
    int32 previous_end;

    ASSERT(word_count);

    if (lrc_ctc_tokenized_text_uses_segments(tokens, normalized)) {
        return lrc_ctc_segment_word_count(token_spans,
                                          tokens,
                                          normalized,
                                          word_count,
                                          result);
    }

    *word_count = 0;
    in_word = false;
    previous_token_index = -1;
    previous_end = -1;
    for (int32 i = 0; i < token_spans->span_count; i += 1) {
        LrcCtcTokenSpan *span = token_spans->spans + i;
        LrcCtcTextToken *token;

        if (!lrc_ctc_token_span_resolve_token(span,
                                              tokens,
                                              i,
                                              previous_token_index,
                                              &token,
                                              result)) {
            return false;
        }
        if (!lrc_ctc_token_range_valid(token,
                                       normalized,
                                       span->token_index,
                                       result)) {
            return false;
        }
        if ((previous_end >= 0)
            && (previous_end < token->normalized_start)
            && lrc_ctc_normalized_range_has_space(normalized,
                                                  previous_end,
                                                  token->normalized_start)) {
            in_word = false;
        }
        if (lrc_ctc_normalized_range_is_space(normalized,
                                              token->normalized_start,
                                              token->normalized_end)) {
            in_word = false;
            previous_token_index = span->token_index;
            previous_end = token->normalized_end;
            continue;
        }
        if (lrc_ctc_normalized_range_has_space(normalized,
                                               token->normalized_start,
                                               token->normalized_end)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
                "CTC token spans cannot split a mixed word/space token",
                token->normalized_start,
                i
            );
            return false;
        }
        if (!in_word) {
            *word_count += 1;
            in_word = true;
        }
        previous_token_index = span->token_index;
        previous_end = token->normalized_end;
    }

    return true;
}

static void
lrc_ctc_word_span_start(
    LrcCtcWordSpan *word,
    int32 word_index,
    int32 span_index,
    LrcCtcTokenSpan *token_span,
    LrcCtcTextToken *token
) {
    word->word_index = word_index;
    word->token_start_index = token_span->token_index;
    word->token_end_index = token_span->token_index + 1;
    word->span_start_index = span_index;
    word->span_end_index = span_index + 1;

    word->normalized_start = token->normalized_start;
    word->normalized_end = token->normalized_end;
    word->line_index = token->line_index;

    word->start_seconds = lrc_ctc_token_span_start_seconds(token_span);
    word->end_seconds = lrc_ctc_token_span_end_seconds(token_span);
    word->score = token_span->score;

    return;
}

static bool
lrc_ctc_word_span_extend(
    LrcCtcWordSpan *word,
    int32 span_index,
    LrcCtcTokenSpan *token_span,
    LrcCtcTextToken *token,
    int32 score_count,
    float *score_sum,
    LrcCtcAlignResult *result
) {
    if (token->line_index != word->line_index) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT,
            "CTC word cannot cross lyric lines",
            token->normalized_start,
            token_span->token_index
        );
        return false;
    }
    if ((lrc_ctc_token_span_start_seconds(token_span) + 0.00001f)
        < word->end_seconds) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS,
            "CTC token spans must be time ordered",
            -1,
            token_span->token_index
        );
        return false;
    }

    word->token_end_index = token_span->token_index + 1;
    word->span_end_index = span_index + 1;
    word->normalized_end = token->normalized_end;
    word->end_seconds = lrc_ctc_token_span_end_seconds(token_span);
    *score_sum += token_span->score;
    word->score = *score_sum/(float)score_count;

    return true;
}

static bool
lrc_ctc_token_spans_to_word_spans(
    LrcCtcTokenSpans *token_spans,
    LrcCtcTokenizedText *tokens,
    LrcLyricsNormalized *normalized,
    LrcCtcWordSpans *word_spans,
    LrcCtcAlignResult *result
) {
    LrcCtcWordSpan *word;
    int32 word_count;
    int32 word_index;
    int32 score_count;
    int32 previous_token_index;
    int32 previous_end;
    float score_sum;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_word_inputs_ready(token_spans,
                                   tokens,
                                   normalized,
                                   word_spans,
                                   result)) {
        return false;
    }

    if (lrc_ctc_tokenized_text_uses_segments(tokens, normalized)) {
        return lrc_ctc_token_spans_to_segment_word_spans(token_spans,
                                                         tokens,
                                                         normalized,
                                                         word_spans,
                                                         result);
    }

    lrc_ctc_word_spans_destroy(word_spans);
    if (!lrc_ctc_word_count(token_spans,
                            tokens,
                            normalized,
                            &word_count,
                            result)) {
        return false;
    }
    if (!lrc_ctc_word_spans_allocate(word_spans, word_count, result)) {
        return false;
    }

    word = NULL;
    word_index = -1;
    score_count = 0;
    score_sum = 0.0f;
    previous_token_index = -1;
    previous_end = -1;
    for (int32 i = 0; i < token_spans->span_count; i += 1) {
        LrcCtcTokenSpan *token_span = token_spans->spans + i;
        LrcCtcTextToken *token;

        if (!lrc_ctc_token_span_resolve_token(token_span,
                                              tokens,
                                              i,
                                              previous_token_index,
                                              &token,
                                              result)) {
            lrc_ctc_word_spans_destroy(word_spans);
            return false;
        }
        if ((previous_end >= 0)
            && (previous_end < token->normalized_start)
            && lrc_ctc_normalized_range_has_space(normalized,
                                                  previous_end,
                                                  token->normalized_start)) {
            word = NULL;
            score_count = 0;
            score_sum = 0.0f;
        }
        if (lrc_ctc_normalized_range_is_space(normalized,
                                              token->normalized_start,
                                              token->normalized_end)) {
            word = NULL;
            score_count = 0;
            score_sum = 0.0f;
            previous_token_index = token_span->token_index;
            previous_end = token->normalized_end;
            continue;
        }

        if (word == NULL) {
            word_index += 1;
            ASSERT(word_index < word_spans->span_count);
            word = word_spans->spans + word_index;
            score_count = 1;
            score_sum = token_span->score;
            lrc_ctc_word_span_start(word,
                                    word_index,
                                    i,
                                    token_span,
                                    token);
            previous_token_index = token_span->token_index;
            previous_end = token->normalized_end;
            continue;
        }

        score_count += 1;
        if (!lrc_ctc_word_span_extend(word,
                                      i,
                                      token_span,
                                      token,
                                      score_count,
                                      &score_sum,
                                      result)) {
            lrc_ctc_word_spans_destroy(word_spans);
            return false;
        }
        previous_token_index = token_span->token_index;
        previous_end = token->normalized_end;
    }
    ASSERT(word_index + 1 == word_spans->span_count);

    return true;
}

static bool
lrc_ctc_line_timestamps_allocate(
    LrcCtcLineTimestamps *timestamps,
    int32 line_count,
    LrcCtcAlignResult *result
) {
    int64 alloc_size;

    if (timestamps == NULL) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC line timestamps destination is missing",
            -1,
            -1
        );
        return false;
    }
    if (line_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word spans did not produce lyric lines",
            -1,
            -1
        );
        return false;
    }
    if (!lrc_ctc_align_checked_multiply(
        line_count,
        SIZEOF(*timestamps->lines),
        &alloc_size,
        "CTC line timestamp allocation is too large",
        result
    )) {
        return false;
    }

    lrc_ctc_line_timestamps_destroy(timestamps);
    ARRAY_INIT_COUNT(timestamps->lines, line_count);
    timestamps->line_count = line_count;
    timestamps->timestamped_line_count = 0;
    timestamps->blank_line_count = 0;

    for (int32 i = 0; i < timestamps->line_count; i += 1) {
        timestamps->lines[i].word_start_index = -1;
        timestamps->lines[i].word_end_index = -1;
        timestamps->lines[i].line_index = -1;
        timestamps->lines[i].start_seconds = 0.0f;
        timestamps->lines[i].end_seconds = 0.0f;
        timestamps->lines[i].score = -INFINITY;
        timestamps->lines[i].kind = LRC_CTC_LINE_TIMESTAMP_KIND_BLANK;
    }

    return true;
}

static bool
lrc_ctc_word_span_valid_for_lines(
    LrcCtcWordSpan *word,
    LrcLyricsNormalized *normalized,
    int32 word_index,
    LrcCtcAlignResult *result
) {
    int32 line_start;
    int32 line_end;

    if ((word->word_index != word_index)
        || (word->token_start_index < 0)
        || (word->token_end_index <= word->token_start_index)
        || (word->span_start_index < 0)
        || (word->span_end_index <= word->span_start_index)
        || (word->line_index < 0)
        || (word->line_index >= normalized->line_count)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word span has invalid indexes",
            -1,
            word_index
        );
        return false;
    }
    if (!lrc_lyrics_normalized_line_range(normalized,
                                          word->line_index,
                                          &line_start,
                                          &line_end)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word span does not belong to an alignable lyric line",
            -1,
            word_index
        );
        return false;
    }
    if ((word->normalized_start < line_start)
        || (word->normalized_end > line_end)
        || (word->normalized_end <= word->normalized_start)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word span normalized range is invalid",
            word->normalized_start,
            word_index
        );
        return false;
    }
    if (!isfinite(word->start_seconds) || !isfinite(word->end_seconds)
        || (word->end_seconds < word->start_seconds)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word span timing is invalid",
            -1,
            word_index
        );
        return false;
    }
    if (!isfinite(word->score)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word span score is invalid",
            -1,
            word_index
        );
        return false;
    }

    return true;
}

static bool
lrc_ctc_line_inputs_ready(
    LrcCtcWordSpans *word_spans,
    LrcLyricsNormalized *normalized,
    LrcCtcLineTimestamps *line_timestamps,
    LrcCtcAlignResult *result
) {
    int32 previous_line;
    float previous_start;

    if ((word_spans == NULL) || (normalized == NULL)
        || (line_timestamps == NULL)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT,
            "CTC line timestamp conversion received invalid arguments",
            -1,
            -1
        );
        return false;
    }
    if ((word_spans->spans == NULL) || (word_spans->span_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word spans are empty",
            -1,
            -1
        );
        return false;
    }
    if ((normalized->lines == NULL) || (normalized->line_count <= 0)) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_NORMALIZED_TEXT,
            "normalized lyric lines are not ready",
            -1,
            -1
        );
        return false;
    }

    previous_line = -1;
    previous_start = -INFINITY;
    for (int32 i = 0; i < word_spans->span_count; i += 1) {
        LrcCtcWordSpan *word = word_spans->spans + i;
        LrcCtcWordSpan *previous;

        if (!lrc_ctc_word_span_valid_for_lines(word,
                                               normalized,
                                               i,
                                               result)) {
            return false;
        }
        if (word->line_index < previous_line) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
                "CTC word spans must be ordered by lyric line",
                -1,
                i
            );
            return false;
        }
        if ((word->line_index == previous_line)
            && ((word->start_seconds + 0.00001f) < previous_start)) {
            lrc_ctc_align_result_set(
                result,
                LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
                "CTC word spans must be time ordered inside each line",
                -1,
                i
            );
            return false;
        }
        if (i > 0) {
            previous = word_spans->spans + i - 1;
            if ((word->token_start_index < previous->token_end_index)
                || (word->span_start_index < previous->span_end_index)) {
                lrc_ctc_align_result_set(
                    result,
                    LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
                    "CTC word spans must be target-token ordered",
                    -1,
                    i
                );
                return false;
            }
        }

        previous_line = word->line_index;
        previous_start = word->start_seconds;
    }

    return true;
}

static bool
lrc_ctc_line_has_words(
    LrcCtcWordSpans *word_spans,
    int32 line_index,
    int32 *first_word_index,
    int32 *end_word_index
) {
    int32 first = -1;
    int32 end = -1;

    for (int32 i = 0; i < word_spans->span_count; i += 1) {
        if (word_spans->spans[i].line_index != line_index) {
            continue;
        }
        if (first < 0) {
            first = i;
        }
        end = i + 1;
    }

    if (first_word_index) {
        *first_word_index = first;
    }
    if (end_word_index) {
        *end_word_index = end;
    }

    return first >= 0;
}

static bool
lrc_ctc_count_line_timestamp_entries(
    LrcCtcWordSpans *word_spans,
    LrcLyricsNormalized *normalized,
    int32 *line_count,
    LrcCtcAlignResult *result
) {
    enum LrcLyricsNormalizedLineKind kind;

    *line_count = 0;
    for (int32 i = 0; i < normalized->line_count; i += 1) {
        kind = lrc_lyrics_normalized_line_kind(normalized, i);
        if (kind == LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK) {
            *line_count += 1;
            continue;
        }
        if (kind == LRC_LYRICS_NORMALIZED_LINE_KIND_ALIGNABLE) {
            if (lrc_ctc_line_has_words(word_spans, i, NULL, NULL)) {
                *line_count += 1;
            }
            continue;
        }
    }
    if (*line_count <= 0) {
        lrc_ctc_align_result_set(
            result,
            LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS,
            "CTC word spans did not map to lyric lines",
            -1,
            -1
        );
        return false;
    }

    return true;
}

static void
lrc_ctc_line_timestamp_set_blank(
    LrcCtcLineTimestamps *timestamps,
    int32 index,
    int32 line_index
) {
    LrcCtcLineTimestamp *line;

    ASSERT(timestamps);
    ASSERT(index >= 0);
    ASSERT(index < timestamps->line_count);

    line = timestamps->lines + index;
    line->word_start_index = -1;
    line->word_end_index = -1;
    line->line_index = line_index;
    line->start_seconds = 0.0f;
    line->end_seconds = 0.0f;
    line->score = -INFINITY;
    line->kind = LRC_CTC_LINE_TIMESTAMP_KIND_BLANK;
    timestamps->blank_line_count += 1;

    return;
}

static void
lrc_ctc_line_timestamp_set_timed(
    LrcCtcLineTimestamps *timestamps,
    int32 index,
    int32 line_index,
    LrcCtcWordSpans *word_spans,
    int32 first_word_index,
    int32 end_word_index
) {
    LrcCtcLineTimestamp *line;
    LrcCtcWordSpan *first;
    LrcCtcWordSpan *last;
    float score_sum;

    ASSERT(timestamps);
    ASSERT(index >= 0);
    ASSERT(index < timestamps->line_count);
    ASSERT(first_word_index >= 0);
    ASSERT(end_word_index > first_word_index);
    ASSERT(end_word_index <= word_spans->span_count);

    first = word_spans->spans + first_word_index;
    last = word_spans->spans + end_word_index - 1;
    score_sum = 0.0f;
    for (int32 i = first_word_index; i < end_word_index; i += 1) {
        score_sum += word_spans->spans[i].score;
    }

    line = timestamps->lines + index;
    line->word_start_index = first_word_index;
    line->word_end_index = end_word_index;
    line->line_index = line_index;
    line->start_seconds = first->start_seconds;
    line->end_seconds = last->end_seconds;
    line->score = score_sum/(float)(end_word_index - first_word_index);
    line->kind = LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED;
    timestamps->timestamped_line_count += 1;

    return;
}

static bool
lrc_ctc_word_spans_to_line_timestamps(
    LrcCtcWordSpans *word_spans,
    LrcLyricsNormalized *normalized,
    LrcCtcLineTimestamps *line_timestamps,
    LrcCtcAlignResult *result
) {
    int32 line_count;
    int32 out_index;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_line_inputs_ready(word_spans,
                                   normalized,
                                   line_timestamps,
                                   result)) {
        return false;
    }
    lrc_ctc_line_timestamps_destroy(line_timestamps);
    if (!lrc_ctc_count_line_timestamp_entries(word_spans,
                                              normalized,
                                              &line_count,
                                              result)) {
        return false;
    }
    if (!lrc_ctc_line_timestamps_allocate(line_timestamps,
                                          line_count,
                                          result)) {
        return false;
    }

    out_index = 0;
    for (int32 i = 0; i < normalized->line_count; i += 1) {
        enum LrcLyricsNormalizedLineKind kind;
        int32 first_word_index;
        int32 end_word_index;

        kind = lrc_lyrics_normalized_line_kind(normalized, i);
        if (kind == LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK) {
            lrc_ctc_line_timestamp_set_blank(line_timestamps, out_index, i);
            out_index += 1;
            continue;
        }
        if (kind != LRC_LYRICS_NORMALIZED_LINE_KIND_ALIGNABLE) {
            continue;
        }
        if (!lrc_ctc_line_has_words(word_spans,
                                    i,
                                    &first_word_index,
                                    &end_word_index)) {
            continue;
        }

        lrc_ctc_line_timestamp_set_timed(line_timestamps,
                                         out_index,
                                         i,
                                         word_spans,
                                         first_word_index,
                                         end_word_index);
        out_index += 1;
    }
    ASSERT(out_index == line_timestamps->line_count);

    return true;
}


#if TESTING_ctc_align
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "lyrics.c"
#include "unicode_norm.c"
#include "ctc_text.c"
#include "ctc_tokenizer.c"
#include "audio.c"
#include "ctc_audio.c"
#include "ctc_model.c"
#include "ctc_inference.c"
#include "lrc.c"

static bool
lrc_ctc_align_graph_state_count(
    int32 target_token_count,
    int32 *state_count,
    LrcCtcAlignResult *result
) {
    return lrc_ctc_align_graph_state_count_for_mode(
        target_token_count,
        LRC_CTC_ALIGN_STAR_MODE_NONE,
        NULL,
        state_count,
        result
    );
}

static int32
lrc_ctc_required_frame_count_for_tokens(
    int32 *target_token_ids,
    int32 target_token_count
) {
    int32 frame_count;

    if ((target_token_ids == NULL) || (target_token_count <= 0)) {
        return -1;
    }

    frame_count = target_token_count;
    for (int32 i = 1; i < target_token_count; i += 1) {
        if (target_token_ids[i] != target_token_ids[i - 1]) {
            continue;
        }
        if (frame_count >= INT32_MAX) {
            return -1;
        }
        frame_count += 1;
    }

    return frame_count;
}

static bool
lrc_ctc_align_graph_build(
    LrcCtcAlignGraph *graph,
    int32 *target_token_ids,
    int32 target_token_count,
    LrcCtcAlignResult *result
) {
    return lrc_ctc_align_graph_build_for_mode(
        graph,
        target_token_ids,
        target_token_count,
        LRC_CTC_ALIGN_STAR_MODE_NONE,
        NULL,
        -1,
        result
    );
}

static bool
lrc_ctc_trellis_allocate(
    LrcCtcTrellis *trellis,
    int32 frame_count,
    int32 target_token_count,
    LrcCtcAlignResult *result
) {
    int32 state_count;

    if (!lrc_ctc_align_graph_state_count(target_token_count,
                                          &state_count,
                                          result)) {
        return false;
    }

    return lrc_ctc_trellis_allocate_for_state_count(
        trellis,
        frame_count,
        target_token_count,
        state_count,
        LRC_CTC_ALIGN_STAR_MODE_NONE,
        -1,
        result
    );
}

static bool
lrc_ctc_trellis_prepare(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 target_token_count,
    int32 blank_token_id,
    LrcCtcAlignResult *result
) {
    int32 state_count;
    float *cell;

    if (result) {
        lrc_ctc_align_result_init(result);
    }
    if (!lrc_ctc_trellis_emissions_ready(emissions,
                                         blank_token_id,
                                         result)) {
        return false;
    }
    if (!lrc_ctc_align_graph_state_count(target_token_count,
                                          &state_count,
                                          result)) {
        return false;
    }
    if (!lrc_ctc_trellis_allocate_for_state_count(
        trellis,
        (int32)emissions->frame_count,
        target_token_count,
        state_count,
        LRC_CTC_ALIGN_STAR_MODE_NONE,
        -1,
        result
    )) {
        return false;
    }

    cell = lrc_ctc_trellis_cell(trellis, 0, 0);
    ASSERT(cell);
    *cell = emissions->values[blank_token_id];
    for (int32 frame = 1; frame < trellis->frame_count; frame += 1) {
        float previous;
        float blank_score;

        cell = lrc_ctc_trellis_cell(trellis, frame - 1, 0);
        ASSERT(cell);
        previous = *cell;
        blank_score = emissions->values[frame*emissions->vocabulary_size
                                        + blank_token_id];

        cell = lrc_ctc_trellis_cell(trellis, frame, 0);
        ASSERT(cell);
        *cell = previous + blank_score;

        *lrc_ctc_trellis_previous_state_cell(trellis, frame, 0) = 0;
    }

    return true;
}

static bool
lrc_ctc_trellis_score_forward(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 blank_token_id,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            NULL,
                            target_token_count,
                            blank_token_id,
                            LRC_CTC_ALIGN_STAR_MODE_NONE,
                            -1);

    return lrc_ctc_trellis_score_forward_with_plan(trellis,
                                                   emissions,
                                                   &plan,
                                                   result);
}

static bool
lrc_ctc_trellis_score_forward_with_edge_stars(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 blank_token_id,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            NULL,
                            target_token_count,
                            blank_token_id,
                            LRC_CTC_ALIGN_STAR_MODE_EDGES,
                            star_token_id);

    return lrc_ctc_trellis_score_forward_with_plan(trellis,
                                                   emissions,
                                                   &plan,
                                                   result);
}

static bool
lrc_ctc_trellis_score_forward_with_segment_stars(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 blank_token_id,
    int32 star_token_id,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            target_segment_starts,
                            target_token_count,
                            blank_token_id,
                            LRC_CTC_ALIGN_STAR_MODE_SEGMENT,
                            star_token_id);

    return lrc_ctc_trellis_score_forward_with_plan(trellis,
                                                   emissions,
                                                   &plan,
                                                   result);
}

static bool
lrc_ctc_trellis_backtrack(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 blank_token_id,
    LrcCtcPath *path,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            NULL,
                            target_token_count,
                            blank_token_id,
                            LRC_CTC_ALIGN_STAR_MODE_NONE,
                            -1);

    return lrc_ctc_trellis_backtrack_with_plan(trellis,
                                               emissions,
                                               &plan,
                                               path,
                                               result);
}

static bool
lrc_ctc_trellis_backtrack_with_edge_stars(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 blank_token_id,
    int32 star_token_id,
    LrcCtcPath *path,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            NULL,
                            target_token_count,
                            blank_token_id,
                            LRC_CTC_ALIGN_STAR_MODE_EDGES,
                            star_token_id);

    return lrc_ctc_trellis_backtrack_with_plan(trellis,
                                               emissions,
                                               &plan,
                                               path,
                                               result);
}

static bool
lrc_ctc_trellis_backtrack_with_segment_stars(
    LrcCtcTrellis *trellis,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 blank_token_id,
    int32 star_token_id,
    LrcCtcPath *path,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            target_segment_starts,
                            target_token_count,
                            blank_token_id,
                            LRC_CTC_ALIGN_STAR_MODE_SEGMENT,
                            star_token_id);

    return lrc_ctc_trellis_backtrack_with_plan(trellis,
                                               emissions,
                                               &plan,
                                               path,
                                               result);
}

static bool
lrc_ctc_path_to_padded_token_spans(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    float frame_duration_seconds,
    LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            NULL,
                            target_token_count,
                            -1,
                            LRC_CTC_ALIGN_STAR_MODE_NONE,
                            -1);

    return lrc_ctc_path_to_padded_token_spans_with_plan(path,
                                                        emissions,
                                                        &plan,
                                                        frame_duration_seconds,
                                                        spans,
                                                        result);
}

static bool
lrc_ctc_path_to_padded_token_spans_with_edge_stars(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    int32 target_token_count,
    int32 star_token_id,
    float frame_duration_seconds,
    LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            NULL,
                            target_token_count,
                            -1,
                            LRC_CTC_ALIGN_STAR_MODE_EDGES,
                            star_token_id);

    return lrc_ctc_path_to_padded_token_spans_with_plan(path,
                                                        emissions,
                                                        &plan,
                                                        frame_duration_seconds,
                                                        spans,
                                                        result);
}

static bool
lrc_ctc_path_to_padded_token_spans_with_segment_stars(
    LrcCtcPath *path,
    LrcCtcEmissions *emissions,
    int32 *target_token_ids,
    bool *target_segment_starts,
    int32 target_token_count,
    int32 star_token_id,
    float frame_duration_seconds,
    LrcCtcTokenSpans *spans,
    LrcCtcAlignResult *result
) {
    LrcCtcAlignPlan plan;

    lrc_ctc_align_plan_init(&plan,
                            target_token_ids,
                            target_segment_starts,
                            target_token_count,
                            -1,
                            LRC_CTC_ALIGN_STAR_MODE_SEGMENT,
                            star_token_id);

    return lrc_ctc_path_to_padded_token_spans_with_plan(path,
                                                        emissions,
                                                        &plan,
                                                        frame_duration_seconds,
                                                        spans,
                                                        result);
}

static int32
ctc_align_test_fail(char *name) {
    error2("CTC align test failed: %s\n", name);

    return 1;
}

static bool
ctc_align_float_close(float a, float b, float max_error) {
    float diff;

    diff = fabsf(a - b);

    return diff <= max_error;
}

static bool
ctc_align_is_negative_infinity(float value) {
    if (!isinf(value)) {
        return false;
    }

    return value < 0.0f;
}

static void
ctc_align_make_emissions(
    LrcCtcEmissions *emissions,
    float *values,
    int32 frame_count,
    int32 vocabulary_size
) {
    memset64(emissions, 0, SIZEOF(*emissions));

    emissions->values = values;
    emissions->value_count = (int64)frame_count*(int64)vocabulary_size;
    emissions->row_count = 1;
    emissions->row_frame_count = frame_count;
    emissions->frame_count = frame_count;
    emissions->vocabulary_size = vocabulary_size;
    emissions->shape_len = 2;
    emissions->shape[0] = frame_count;
    emissions->shape[1] = vocabulary_size;

    return;
}


static void
ctc_align_set_path_segment(
    LrcCtcPathSegments *segments,
    int32 segment_index,
    int32 token_index,
    int32 start_frame,
    int32 end_frame,
    int32 token_id,
    bool is_blank,
    bool is_star
) {
    LrcCtcPathSegment *segment;

    ASSERT(segments);
    ASSERT(segments->segments);
    ASSERT(segment_index >= 0);
    ASSERT(segment_index < segments->segment_count);

    segment = segments->segments + segment_index;
    segment->token_index = token_index;
    segment->start_frame = start_frame;
    segment->end_frame = end_frame;
    segment->start_seconds = (float)start_frame*0.10f;
    segment->end_seconds = (float)end_frame*0.10f;
    segment->score = -0.10f;
    segment->token_id = token_id;
    segment->is_blank = is_blank;
    segment->is_star = is_star;

    return;
}

static bool
ctc_align_load_alphabet_tokenizer_with_options(
    LrcCtcTokenizer *tokenizer,
    bool include_space
) {
    LrcCtcTokenizerResult result;
    StrBuilder builder;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    bool ok;

    sb_init(&builder);
    SB_APPEND(&builder, "<blank>\n");
    if (include_space) {
        SB_APPEND(&builder, "<space>\n");
    }
    for (char ch = 'a'; ch <= 'z'; ch += 1) {
        sb_append(&builder, &ch, 1);
        SB_APPEND(&builder, "\n");
    }

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_align_tokens");
    test_join_path(path, SIZEOF(path), temp_dir, "tokens.txt");
    if (write_entire_file(path, builder.data, builder.len) < 0) {
        test_remove_tree(temp_dir);
        sb_free(&builder);
        return false;
    }

    lrc_ctc_tokenizer_init(tokenizer);
    ok = lrc_ctc_tokenizer_load_file(tokenizer, path, &result);
    test_remove_tree(temp_dir);
    sb_free(&builder);

    return ok;
}

static bool
ctc_align_load_alphabet_tokenizer(LrcCtcTokenizer *tokenizer) {
    return ctc_align_load_alphabet_tokenizer_with_options(tokenizer, true);
}

static bool
ctc_align_load_no_space_alphabet_tokenizer(LrcCtcTokenizer *tokenizer) {
    return ctc_align_load_alphabet_tokenizer_with_options(tokenizer, false);
}

static bool
ctc_align_load_lyrics_text(
    LrcLyrics *lyrics,
    char *text,
    int32 text_len
) {
    LrcLyricsLoadResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    bool ok;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_align_lyrics");
    test_join_path(path, SIZEOF(path), temp_dir, "lyrics.txt");
    if (write_entire_file(path, text, text_len) < 0) {
        test_remove_tree(temp_dir);
        return false;
    }

    memset64(lyrics, 0, SIZEOF(*lyrics));
    ok = lrc_lyrics_load_file(lyrics, path, &result);
    test_remove_tree(temp_dir);

    return ok;
}

static bool
ctc_align_normalize_current_lyrics(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized
) {
    LrcLyricsPreprocessOptions options;

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CURRENT;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF;

    return lrc_lyrics_normalize_with_options(lyrics, normalized, &options);
}

static bool
ctc_align_make_token_spans_from_tokens(
    LrcCtcTokenizedText *tokens,
    float first_start_seconds,
    float token_seconds,
    LrcCtcTokenSpans *spans
) {
    LrcCtcAlignResult result;

    if (!lrc_ctc_token_spans_allocate(spans,
                                      tokens->token_count,
                                      &result)) {
        return false;
    }

    for (int32 i = 0; i < tokens->token_count; i += 1) {
        LrcCtcTokenSpan *span = spans->spans + i;
        float start_seconds = first_start_seconds + (float)i*token_seconds;

        span->token_index = i;
        span->start_frame = i;
        span->end_frame = i + 1;
        span->start_seconds = start_seconds;
        span->end_seconds = start_seconds + token_seconds;
        span->padded_start_frame = span->start_frame;
        span->padded_end_frame = span->end_frame;
        span->padded_start_seconds = span->start_seconds;
        span->padded_end_seconds = span->end_seconds;
        span->score = -0.10f - (float)i*0.01f;
        span->token_id = tokens->tokens[i].token_id;
    }

    return true;
}

static bool
ctc_align_load_tokenized_lyrics(
    char *text,
    int32 text_len,
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcCtcTokenizer *tokenizer,
    LrcCtcTokenizedText *tokens
) {
    LrcCtcTokenizeResult result;

    memset64(normalized, 0, SIZEOF(*normalized));
    lrc_ctc_tokenizer_init(tokenizer);
    memset64(tokens, 0, SIZEOF(*tokens));
    if (!ctc_align_load_lyrics_text(lyrics, text, text_len)) {
        return false;
    }
    if (!ctc_align_normalize_current_lyrics(lyrics, normalized)) {
        return false;
    }
    if (!ctc_align_load_alphabet_tokenizer(tokenizer)) {
        return false;
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(tokenizer,
                                               normalized,
                                               tokens,
                                               &result)) {
        return false;
    }

    return true;
}

static void
ctc_align_assert_word_text(
    LrcLyricsNormalized *normalized,
    LrcCtcWordSpan *word,
    char *text,
    int32 text_len
) {
    ASSERT(word->normalized_start >= 0);
    ASSERT(word->normalized_end > word->normalized_start);
    ASSERT(word->normalized_end <= normalized->text_len);
    ASSERT(STREQUAL(normalized->text + word->normalized_start,
                     word->normalized_end - word->normalized_start,
                     text,
                     text_len));

    return;
}

static void
ctc_align_fill_predictable_values(
    float *values,
    int32 frame_count,
    int32 vocabulary_size,
    int32 blank_token_id,
    int32 *token_ids,
    int32 token_count
) {
    for (int32 i = 0; i < frame_count*vocabulary_size; i += 1) {
        values[i] = -12.0f;
    }

    for (int32 frame = 0; frame < frame_count; frame += 1) {
        values[frame*vocabulary_size + blank_token_id] = -0.05f;
    }
    for (int32 i = 0; i < token_count; i += 1) {
        int32 frame = i + 1;

        values[frame*vocabulary_size + blank_token_id] = -6.0f;
        values[frame*vocabulary_size + token_ids[i]] = -0.05f;
    }

    return;
}

static bool
ctc_align_parse_lrc_file(
    LrcParsedFile *parsed,
    char *path,
    char **file_text,
    int32 *file_text_len
) {
    LrcParseResult result;

    ASSERT(parsed);
    ASSERT(file_text);
    ASSERT(file_text_len);

    *file_text = NULL;
    *file_text_len = 0;
    if (path_missing(path) || !util_file_exists(path)) {
        return false;
    }

    if ((*file_text_len = read_entire_file(path, file_text)) < 0) {
        return false;
    }
    if (!lrc_parse_text(parsed, *file_text, *file_text_len, &result)) {
        free2(*file_text, ((int32)*file_text_len + 1)*SIZEOF(**file_text));
        *file_text = NULL;
        *file_text_len = 0;
        return false;
    }

    return true;
}

static bool
ctc_align_expected_line_timestamp(
    LrcParsedFile *parsed,
    int32 source_line_index,
    float *timestamp_seconds
) {
    ASSERT(parsed);
    ASSERT(timestamp_seconds);

    for (int32 i = 0; i < parsed->line_count; i += 1) {
        LrcParsedLine *line = parsed->lines + i;

        if (line->kind != LRC_PARSED_LINE_KIND_TIMESTAMPED) {
            continue;
        }
        if (line->source_line_index != source_line_index) {
            continue;
        }

        *timestamp_seconds = line->timestamp_seconds;
        return true;
    }

    return false;
}

static int32
ctc_align_seconds_to_frame(float seconds, float frame_duration_seconds) {
    double frame;

    ASSERT(isfinite(seconds));
    ASSERT(seconds >= 0.0f);
    ASSERT(isfinite(frame_duration_seconds));
    ASSERT(frame_duration_seconds > 0.0f);

    frame = (double)seconds/(double)frame_duration_seconds + 0.5;
    if (frame > (double)INT64_MAX) {
        return -1;
    }

    return (int32)frame;
}

static bool
ctc_align_make_line_timed_token_frames(
    LrcParsedFile *expected,
    LrcCtcTokenizedText *tokens,
    float frame_duration_seconds,
    int32 *token_frames,
    int32 *frame_count
) {
    int32 current_line;
    int32 previous_frame;
    int32 line_start_frame;
    int32 line_token_offset;

    if ((expected == NULL) || (tokens == NULL) || (token_frames == NULL)
        || (frame_count == NULL) || (tokens->tokens == NULL)
        || (tokens->token_count <= 0)
        || !isfinite(frame_duration_seconds)
        || (frame_duration_seconds <= 0.0f)) {
        return false;
    }

    current_line = -1;
    previous_frame = -1;
    line_start_frame = -1;
    line_token_offset = 0;
    *frame_count = 0;
    for (int32 i = 0; i < tokens->token_count; i += 1) {
        LrcCtcTextToken *token = tokens->tokens + i;
        int32 frame;

        if (token->line_index != current_line) {
            float timestamp_seconds;

            if (!ctc_align_expected_line_timestamp(expected,
                                                   token->line_index,
                                                   &timestamp_seconds)) {
                return false;
            }
            line_start_frame = ctc_align_seconds_to_frame(
                timestamp_seconds,
                frame_duration_seconds
            );
            if (line_start_frame < 0) {
                return false;
            }
            if ((i > 0) && (line_start_frame > 0)) {
                line_start_frame -= 1;
            }
            if (line_start_frame <= previous_frame) {
                line_start_frame = previous_frame + 1;
            }

            current_line = token->line_index;
            line_token_offset = 0;
        }

        frame = line_start_frame + line_token_offset;
        if (frame <= previous_frame) {
            return false;
        }
        token_frames[i] = frame;
        previous_frame = frame;
        line_token_offset += 1;
    }

    if (previous_frame > INT32_MAX - 2) {
        return false;
    }
    *frame_count = previous_frame + 2;

    return true;
}

static void
ctc_align_fill_token_frame_values(
    float *values,
    int32 frame_count,
    int32 vocabulary_size,
    int32 blank_token_id,
    int32 *token_ids,
    int32 *token_frames,
    int32 token_count
) {
    for (int32 i = 0; i < frame_count*vocabulary_size; i += 1) {
        values[i] = -12.0f;
    }
    for (int32 frame = 0; frame < frame_count; frame += 1) {
        values[frame*vocabulary_size + blank_token_id] = -0.05f;
    }
    for (int32 i = 0; i < token_count; i += 1) {
        int32 frame = token_frames[i];

        ASSERT(frame >= 0);
        ASSERT(frame < frame_count);
        values[frame*vocabulary_size + blank_token_id] = -6.0f;
        values[frame*vocabulary_size + token_ids[i]] = -0.05f;
    }

    return;
}

static bool
ctc_align_parsed_files_close(
    LrcParsedFile *actual,
    LrcParsedFile *expected,
    float max_error_seconds
) {
    if ((actual == NULL) || (expected == NULL)) {
        return false;
    }
    if (actual->line_count != expected->line_count) {
        error2("LRC line count mismatch: actual=%d expected=%d\n",
               actual->line_count, expected->line_count);
        return false;
    }

    for (int32 i = 0; i < expected->line_count; i += 1) {
        LrcParsedLine *actual_line = actual->lines + i;
        LrcParsedLine *expected_line = expected->lines + i;
        float diff;

        if (actual_line->kind != expected_line->kind) {
            error2("LRC line %d kind mismatch\n", i);
            return false;
        }
        if (!STREQUAL(actual_line->text, actual_line->text_len,
                      expected_line->text, expected_line->text_len)) {
            error2("LRC line %d text mismatch\n", i);
            return false;
        }
        if (expected_line->kind != LRC_PARSED_LINE_KIND_TIMESTAMPED) {
            continue;
        }

        diff = fabsf(actual_line->timestamp_seconds
                     - expected_line->timestamp_seconds);
        if (diff > max_error_seconds) {
            error2(
                "LRC line %d timestamp diff %.3f actual %.3f expected %.3f\n",
                i,
                (double)diff,
                (double)actual_line->timestamp_seconds,
                (double)expected_line->timestamp_seconds
            );
            return false;
        }
    }

    return true;
}


static bool
ctc_align_output_lines_from_timestamps(
    LrcLyrics *lyrics,
    LrcCtcLineTimestamps *timestamps,
    LrcOutputLine *lines
) {
    if ((lyrics == NULL) || (timestamps == NULL) || (lines == NULL)) {
        return false;
    }
    if ((timestamps->line_count < 0)
        || (timestamps->line_count > INT32_MAX)) {
        return false;
    }

    for (int32 i = 0; i < timestamps->line_count; i += 1) {
        LrcCtcLineTimestamp *timestamp = timestamps->lines + i;
        LrcLyricsLine *lyrics_line;
        LrcFormatResult result;
        int32 hundredths;

        if ((timestamp->line_index < 0)
            || (timestamp->line_index >= lyrics->line_count)) {
            return false;
        }

        lyrics_line = lyrics->lines + timestamp->line_index;
        lines[i].text = lyrics_line->text;
        lines[i].text_len = lyrics_line->text_len;
        lines[i].timestamp_hundredths = -1;

        switch (timestamp->kind) {
        case LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED:
            if (!lrc_timestamp_hundredths_from_seconds(
                timestamp->start_seconds,
                &hundredths,
                &result
            )) {
                return false;
            }
            lines[i].kind = LRC_OUTPUT_LINE_KIND_TIMESTAMPED;
            lines[i].timestamp_hundredths = hundredths;
            break;
        case LRC_CTC_LINE_TIMESTAMP_KIND_BLANK:
            lines[i].kind = LRC_OUTPUT_LINE_KIND_BLANK;
            break;
        default:
            return false;
        }
    }

    return true;
}

static void
ctc_align_test_empty_initializers(void) {
    LrcCtcAlignResult result;
    LrcCtcAlignGraph graph = {0};
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcPathSegments path_segments = {0};
    LrcCtcTokenSpans spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};

    lrc_ctc_align_result_init(&result);

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.header.message, "ok"));
    ASSERT(result.frame_index == -1);
    ASSERT(result.token_index == -1);

    ASSERT(graph.states == NULL);
    ASSERT(graph.state_count == 0);
    ASSERT(graph.target_token_count == 0);

    ASSERT(trellis.scores == NULL);
    ASSERT(trellis.frame_count == 0);
    ASSERT(trellis.target_token_count == 0);
    ASSERT(trellis.state_count == 0);
    ASSERT(trellis.cell_count == 0);

    ASSERT(path.steps == NULL);
    ASSERT(path.step_count == 0);

    ASSERT(path_segments.segments == NULL);
    ASSERT(path_segments.segment_count == 0);

    ASSERT(spans.spans == NULL);
    ASSERT(spans.span_count == 0);

    ASSERT(word_spans.spans == NULL);
    ASSERT(word_spans.span_count == 0);

    ASSERT(line_timestamps.lines == NULL);
    ASSERT(line_timestamps.line_count == 0);
    ASSERT(line_timestamps.timestamped_line_count == 0);
    ASSERT(line_timestamps.blank_line_count == 0);

    return;
}

static void
ctc_align_test_graph_build_layout(void) {
    LrcCtcAlignResult result;
    LrcCtcAlignGraph graph = {0};
    int32 one_token[] = {7};
    int32 two_tokens[] = {4, 8};
    int32 repeated_tokens[] = {3, 3};

    if (!lrc_ctc_align_graph_build(&graph, one_token, 1, &result)) {
        fatal(ctc_align_test_fail("build one-token CTC graph"));
    }
    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(graph.target_token_count == 1);
    ASSERT(graph.state_count == 3);
    ASSERT(graph.states[0].kind == LRC_CTC_ALIGN_STATE_BLANK);
    ASSERT(graph.states[0].token_index == -1);
    ASSERT(graph.states[1].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[1].token_index == 0);
    ASSERT(graph.states[1].token_id == 7);
    ASSERT(graph.states[2].kind == LRC_CTC_ALIGN_STATE_BLANK);

    if (!lrc_ctc_align_graph_build(&graph, two_tokens, 2, &result)) {
        fatal(ctc_align_test_fail("build two-token CTC graph"));
    }
    ASSERT(graph.target_token_count == 2);
    ASSERT(graph.state_count == 5);
    ASSERT(graph.states[0].kind == LRC_CTC_ALIGN_STATE_BLANK);
    ASSERT(graph.states[1].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[1].token_index == 0);
    ASSERT(graph.states[1].token_id == 4);
    ASSERT(graph.states[2].kind == LRC_CTC_ALIGN_STATE_BLANK);
    ASSERT(graph.states[3].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[3].token_index == 1);
    ASSERT(graph.states[3].token_id == 8);
    ASSERT(graph.states[4].kind == LRC_CTC_ALIGN_STATE_BLANK);

    if (!lrc_ctc_align_graph_build(&graph, repeated_tokens, 2, &result)) {
        fatal(ctc_align_test_fail("build repeated-token CTC graph"));
    }
    ASSERT(graph.state_count == 5);
    ASSERT(graph.states[1].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[1].token_index == 0);
    ASSERT(graph.states[1].token_id == 3);
    ASSERT(graph.states[3].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[3].token_index == 1);
    ASSERT(graph.states[3].token_id == 3);

    lrc_ctc_align_graph_destroy(&graph);

    return;
}

static void
ctc_align_test_graph_build_edge_stars(void) {
    LrcCtcAlignResult result;
    LrcCtcAlignGraph graph = {0};
    int32 tokens[] = {4, 8};
    int32 repeated_tokens[] = {4, 4};

    if (!lrc_ctc_align_graph_build_for_mode(&graph,
                                            tokens,
                                            2,
                                            LRC_CTC_ALIGN_STAR_MODE_EDGES,
                                            NULL,
                                            9,
                                            &result)) {
        fatal(ctc_align_test_fail("build edge-star graph"));
    }
    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(graph.target_token_count == 2);
    ASSERT(graph.state_count == 9);
    ASSERT(graph.states[0].kind == LRC_CTC_ALIGN_STATE_BLANK);
    ASSERT(graph.states[1].kind == LRC_CTC_ALIGN_STATE_STAR);
    ASSERT(graph.states[1].token_index == -1);
    ASSERT(graph.states[1].token_id == 9);
    ASSERT(graph.states[3].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[3].token_index == 0);
    ASSERT(graph.states[3].token_id == 4);
    ASSERT(graph.states[5].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[5].token_index == 1);
    ASSERT(graph.states[5].token_id == 8);
    ASSERT(graph.states[7].kind == LRC_CTC_ALIGN_STATE_STAR);
    ASSERT(graph.states[7].token_index == -1);
    ASSERT(graph.states[7].token_id == 9);
    ASSERT(graph.states[8].kind == LRC_CTC_ALIGN_STATE_BLANK);
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 1, 3));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 3, 5));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 5, 7));

    if (!lrc_ctc_align_graph_build_for_mode(&graph,
                                            repeated_tokens,
                                            2,
                                            LRC_CTC_ALIGN_STAR_MODE_EDGES,
                                            NULL,
                                            9,
                                            &result)) {
        fatal(ctc_align_test_fail("build repeated edge-star graph"));
    }
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 1, 3));
    ASSERT(!lrc_ctc_align_state_can_skip(&graph, 3, 5));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 5, 7));
    ASSERT(lrc_ctc_required_frame_count_for_graph(&graph) == 5);

    lrc_ctc_align_graph_destroy(&graph);

    return;
}

static void
ctc_align_test_graph_build_segment_stars(void) {
    LrcCtcAlignResult result;
    LrcCtcAlignGraph graph = {0};
    int32 tokens[] = {4, 8, 6};
    bool segment_starts[] = {true, false, true};

    if (!lrc_ctc_align_graph_build_for_mode(
        &graph,
        tokens,
        LENGTH(tokens),
        LRC_CTC_ALIGN_STAR_MODE_SEGMENT,
        segment_starts,
        9,
        &result
    )) {
        fatal(ctc_align_test_fail("build segment-star graph"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(graph.target_token_count == 3);
    ASSERT(graph.state_count == 11);
    ASSERT(graph.states[1].kind == LRC_CTC_ALIGN_STATE_STAR);
    ASSERT(graph.states[1].token_index == -1);
    ASSERT(graph.states[1].token_id == 9);
    ASSERT(graph.states[3].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[3].token_index == 0);
    ASSERT(graph.states[3].token_id == 4);
    ASSERT(graph.states[5].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[5].token_index == 1);
    ASSERT(graph.states[5].token_id == 8);
    ASSERT(graph.states[7].kind == LRC_CTC_ALIGN_STATE_STAR);
    ASSERT(graph.states[7].token_index == -1);
    ASSERT(graph.states[7].token_id == 9);
    ASSERT(graph.states[9].kind == LRC_CTC_ALIGN_STATE_TOKEN);
    ASSERT(graph.states[9].token_index == 2);
    ASSERT(graph.states[9].token_id == 6);
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 1, 3));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 3, 5));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 5, 7));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 7, 9));

    lrc_ctc_align_graph_destroy(&graph);

    return;
}

static void
ctc_align_test_graph_rejects_bad_inputs(void) {
    LrcCtcAlignResult result;
    LrcCtcAlignGraph graph = {0};
    int32 tokens[] = {1};
    int32 state_count;

    if (lrc_ctc_align_graph_build(NULL, tokens, 1, &result)) {
        fatal(ctc_align_test_fail("missing graph accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (lrc_ctc_align_graph_build(&graph, NULL, 1, &result)) {
        fatal(ctc_align_test_fail("missing graph tokens accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (lrc_ctc_align_graph_build(&graph, tokens, 0, &result)) {
        fatal(ctc_align_test_fail("zero graph tokens accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS);

    if (lrc_ctc_align_graph_state_count(1, NULL, &result)) {
        fatal(ctc_align_test_fail("missing state-count destination accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (lrc_ctc_align_graph_state_count(INT32_MAX/2 + 1,
                                        &state_count,
                                        &result)) {
        fatal(ctc_align_test_fail("huge graph state count accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_TOO_LARGE);
    ASSERT(state_count == 0);
    ASSERT(graph.states == NULL);

    return;
}

static void
ctc_align_test_graph_transition_rules(void) {
    LrcCtcAlignResult result;
    LrcCtcAlignGraph graph = {0};
    int32 different_tokens[] = {1, 2};
    int32 repeated_tokens[] = {1, 1};

    if (!lrc_ctc_align_graph_build(&graph, different_tokens, 2, &result)) {
        fatal(ctc_align_test_fail("build transition graph"));
    }

    ASSERT(lrc_ctc_align_graph_transition_allowed(&graph, 0, 0));
    ASSERT(lrc_ctc_align_graph_transition_allowed(&graph, 0, 1));
    ASSERT(!lrc_ctc_align_graph_transition_allowed(&graph, 0, 2));
    ASSERT(lrc_ctc_align_state_can_skip(&graph, 1, 3));
    ASSERT(lrc_ctc_align_graph_transition_allowed(&graph, 1, 3));
    ASSERT(!lrc_ctc_align_graph_transition_allowed(&graph, 1, 4));
    ASSERT(!lrc_ctc_align_graph_transition_allowed(&graph, 3, 1));
    ASSERT(!lrc_ctc_align_graph_transition_allowed(&graph, -1, 0));
    ASSERT(!lrc_ctc_align_graph_transition_allowed(&graph, 0, 5));

    if (!lrc_ctc_align_graph_build(&graph, repeated_tokens, 2, &result)) {
        fatal(ctc_align_test_fail("build repeated transition graph"));
    }
    ASSERT(!lrc_ctc_align_state_can_skip(&graph, 1, 3));
    ASSERT(!lrc_ctc_align_graph_transition_allowed(&graph, 1, 3));
    ASSERT(lrc_ctc_align_graph_transition_allowed(&graph, 1, 2));
    ASSERT(lrc_ctc_align_graph_transition_allowed(&graph, 2, 3));

    lrc_ctc_align_graph_destroy(&graph);

    return;
}

static void
ctc_align_test_required_frame_count_for_tokens(void) {
    int32 one_token[] = {1};
    int32 different_tokens[] = {1, 2};
    int32 repeated_tokens[] = {1, 1};
    int32 mixed_tokens[] = {1, 1, 2, 2};
    int32 separated_repeat_tokens[] = {1, 2, 1};

    ASSERT(lrc_ctc_required_frame_count_for_tokens(one_token, 1) == 1);
    ASSERT(lrc_ctc_required_frame_count_for_tokens(different_tokens, 2) == 2);
    ASSERT(lrc_ctc_required_frame_count_for_tokens(repeated_tokens, 2) == 3);
    ASSERT(lrc_ctc_required_frame_count_for_tokens(mixed_tokens, 4) == 6);
    ASSERT(lrc_ctc_required_frame_count_for_tokens(separated_repeat_tokens, 3)
           == 3);
    ASSERT(lrc_ctc_required_frame_count_for_tokens(NULL, 1) == -1);
    ASSERT(lrc_ctc_required_frame_count_for_tokens(one_token, 0) == -1);

    return;
}

static void
ctc_align_test_score_rejects_too_few_repeated_frames(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 target_token_ids[] = {1, 1};
    float values[] = {
        -0.10f, -0.20f,
        -0.20f, -0.10f,
    };

    ctc_align_make_emissions(&emissions, values, 2, 2);
    if (lrc_ctc_trellis_score_forward(&trellis,
                                       &emissions,
                                       target_token_ids,
                                       2,
                                       0,
                                       &result)) {
        fatal(ctc_align_test_fail("too-few repeated frames accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_IMPOSSIBLE_ALIGNMENT);
    ASSERT(result.frame_index == 2);
    ASSERT(result.token_index == 2);
    ASSERT(trellis.scores == NULL);

    return;
}

static void
ctc_align_test_allocate_initializes_to_negative_infinity(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};

    if (!lrc_ctc_trellis_allocate(&trellis, 3, 2, &result)) {
        fatal(ctc_align_test_fail("allocate 3x3 trellis"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(trellis.frame_count == 3);
    ASSERT(trellis.target_token_count == 2);
    ASSERT(trellis.state_count == 5);
    ASSERT(trellis.cell_count == 15);
    for (int64 i = 0; i < trellis.cell_count; i += 1) {
        ASSERT(ctc_align_is_negative_infinity(trellis.scores[i]));
    }
    ASSERT(lrc_ctc_trellis_cell(&trellis, 0, 0) == trellis.scores);
    ASSERT(lrc_ctc_trellis_cell(&trellis, 2, 4)
           == trellis.scores + 14);
    ASSERT(lrc_ctc_trellis_cell(&trellis, -1, 0) == NULL);
    ASSERT(lrc_ctc_trellis_cell(&trellis, 0, -1) == NULL);
    ASSERT(lrc_ctc_trellis_cell(&trellis, 3, 0) == NULL);
    ASSERT(lrc_ctc_trellis_cell(&trellis, 0, 5) == NULL);

    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_rejects_invalid_dimensions(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};

    if (lrc_ctc_trellis_allocate(NULL, 1, 1, &result)) {
        fatal(ctc_align_test_fail("missing trellis accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (lrc_ctc_trellis_allocate(&trellis, 0, 1, &result)) {
        fatal(ctc_align_test_fail("zero frames accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS);
    ASSERT(result.frame_index == 0);
    ASSERT(result.token_index == 1);

    if (lrc_ctc_trellis_allocate(&trellis, 1, 0, &result)) {
        fatal(ctc_align_test_fail("zero target tokens accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_DIMENSIONS);
    ASSERT(result.frame_index == -1);
    ASSERT(result.token_index == 0);

    if (lrc_ctc_trellis_allocate(&trellis, 1, INT32_MAX/2 + 1, &result)) {
        fatal(ctc_align_test_fail("huge trellis accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_TOO_LARGE);

    ASSERT(trellis.scores == NULL);

    return;
}

static void
ctc_align_test_prepare_initializes_start_state(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    float values[] = {
        -0.10f, -2.00f, -3.00f,
        -0.20f, -2.10f, -3.10f,
        -0.30f, -2.20f, -3.20f,
    };

    ctc_align_make_emissions(&emissions, values, 3, 3);
    if (!lrc_ctc_trellis_prepare(&trellis,
                                 &emissions,
                                 2,
                                 0,
                                 &result)) {
        fatal(ctc_align_test_fail("prepare state trellis"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(trellis.frame_count == 3);
    ASSERT(trellis.target_token_count == 2);
    ASSERT(trellis.state_count == 5);
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 0, 0),
                                 -0.10f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 1, 0),
                                 -0.30f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 2, 0),
                                 -0.60f,
                                 0.00001f));
    ASSERT(ctc_align_is_negative_infinity(
               *lrc_ctc_trellis_cell(&trellis, 0, 1)));
    ASSERT(ctc_align_is_negative_infinity(
               *lrc_ctc_trellis_cell(&trellis, 0, 2)));
    ASSERT(ctc_align_is_negative_infinity(
               *lrc_ctc_trellis_cell(&trellis, 2, 1)));
    ASSERT(ctc_align_is_negative_infinity(
               *lrc_ctc_trellis_cell(&trellis, 2, 2)));

    lrc_ctc_trellis_destroy(&trellis);

    return;
}


static void
ctc_align_test_forward_scores_simple_path(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 target_token_ids[] = {1, 2};
    float values[] = {
        -0.10f, -5.00f, -5.00f,
        -5.00f, -0.10f, -5.00f,
        -5.00f, -5.00f, -0.10f,
        -0.10f, -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 3);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score simple forward path"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(trellis.state_count == 5);
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 1, 1),
                                 -0.20f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 2, 3),
                                 -0.30f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 3, 4),
                                 -0.40f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 3, 0),
                                 -10.20f,
                                 0.00001f));
    ASSERT(*lrc_ctc_trellis_previous_state_cell(&trellis, 2, 3) == 1);
    ASSERT(*lrc_ctc_trellis_previous_state_cell(&trellis, 3, 4) == 3);

    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_forward_prefers_blank_stay(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 target_token_ids[] = {1};
    float values[] = {
        -0.10f, -5.00f,
        -5.00f, -0.20f,
        -0.10f, -3.00f,
    };

    ctc_align_make_emissions(&emissions, values, 3, 2);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        1,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score blank stay path"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(trellis.state_count == 3);
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 1, 1),
                                 -0.30f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 2, 2),
                                 -0.40f,
                                 0.00001f));
    ASSERT(*lrc_ctc_trellis_previous_state_cell(&trellis, 2, 2) == 1);

    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_trellis_uses_graph_states(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};

    if (!lrc_ctc_trellis_allocate(&trellis, 2, 1, &result)) {
        fatal(ctc_align_test_fail("allocate one-token state trellis"));
    }
    ASSERT(trellis.state_count == 3);
    ASSERT(trellis.cell_count == 6);
    lrc_ctc_trellis_destroy(&trellis);

    if (!lrc_ctc_trellis_allocate(&trellis, 2, 2, &result)) {
        fatal(ctc_align_test_fail("allocate two-token state trellis"));
    }
    ASSERT(trellis.state_count == 5);
    ASSERT(trellis.cell_count == 10);
    lrc_ctc_trellis_destroy(&trellis);

    if (!lrc_ctc_trellis_allocate(&trellis, 2, 3, &result)) {
        fatal(ctc_align_test_fail("allocate three-token state trellis"));
    }
    ASSERT(trellis.state_count == 7);
    ASSERT(trellis.cell_count == 14);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_forward_scores_ctc_skip_transition(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 target_token_ids[] = {1, 2};
    float values[] = {
        -5.00f, -0.10f, -5.00f,
        -5.00f, -5.00f, -0.20f,
    };

    ctc_align_make_emissions(&emissions, values, 2, 3);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score CTC skip transition"));
    }

    ASSERT(ctc_align_float_close(*lrc_ctc_trellis_cell(&trellis, 1, 3),
                                 -0.30f,
                                 0.00001f));
    ASSERT(*lrc_ctc_trellis_previous_state_cell(&trellis, 1, 3) == 1);

    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_best_final_state_selection(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 final_state;
    int32 target_token_ids[] = {1};
    float token_values[] = {
        -5.00f, -0.10f,
    };
    float blank_values[] = {
        -5.00f, -0.10f,
        -0.20f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, token_values, 1, 2);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        1,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score final token state"));
    }
    if (!lrc_ctc_trellis_best_final_state(&trellis, &final_state, &result)) {
        fatal(ctc_align_test_fail("select final token state"));
    }
    ASSERT(final_state == 1);
    lrc_ctc_trellis_destroy(&trellis);

    ctc_align_make_emissions(&emissions, blank_values, 2, 2);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        1,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score final blank state"));
    }
    if (!lrc_ctc_trellis_best_final_state(&trellis, &final_state, &result)) {
        fatal(ctc_align_test_fail("select final blank state"));
    }
    ASSERT(final_state == 2);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_forward_rejects_bad_targets(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 target_token_ids[] = {1, 3};
    float values[] = {
        -0.10f, -0.20f,
        -0.30f, -0.40f,
    };

    ctc_align_make_emissions(&emissions, values, 2, 2);
    if (lrc_ctc_trellis_score_forward(&trellis,
                                      &emissions,
                                      NULL,
                                      1,
                                      0,
                                      &result)) {
        fatal(ctc_align_test_fail("missing target ids accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (lrc_ctc_trellis_score_forward(&trellis,
                                      &emissions,
                                      target_token_ids,
                                      2,
                                      0,
                                      &result)) {
        fatal(ctc_align_test_fail("bad target id accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN);
    ASSERT(result.token_index == 1);
    ASSERT(trellis.scores == NULL);

    return;
}


static void
ctc_align_test_backtracks_simple_path(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    int32 target_token_ids[] = {1, 2};
    float values[] = {
        -0.10f, -5.00f, -5.00f,
        -5.00f, -0.10f, -5.00f,
        -5.00f, -5.00f, -0.10f,
        -0.10f, -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 3);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score simple backtrack path"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   2,
                                   0,
                                   &path,
                                   &result)) {
        fatal(ctc_align_test_fail("backtrack simple path"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(path.step_count == 4);
    ASSERT(path.steps[0].frame_index == 0);
    ASSERT(path.steps[0].state_index == 0);
    ASSERT(path.steps[0].is_blank);
    ASSERT(path.steps[0].token_id == 0);
    ASSERT(path.steps[1].frame_index == 1);
    ASSERT(path.steps[1].state_index == 1);
    ASSERT(!path.steps[1].is_blank);
    ASSERT(path.steps[1].token_index == 0);
    ASSERT(path.steps[1].token_id == 1);
    ASSERT(path.steps[2].frame_index == 2);
    ASSERT(path.steps[2].state_index == 3);
    ASSERT(!path.steps[2].is_blank);
    ASSERT(path.steps[2].token_index == 1);
    ASSERT(path.steps[2].token_id == 2);
    ASSERT(path.steps[3].frame_index == 3);
    ASSERT(path.steps[3].state_index == 4);
    ASSERT(path.steps[3].is_blank);
    ASSERT(path.steps[3].token_id == 0);

    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_backtracks_repeated_tokens(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    int32 target_token_ids[] = {1, 1};
    float values[] = {
        -0.10f, -5.00f,
        -5.00f, -0.10f,
        -0.10f, -5.00f,
        -5.00f, -0.20f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 2);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score repeated-token path"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   2,
                                   0,
                                   &path,
                                   &result)) {
        fatal(ctc_align_test_fail("backtrack repeated-token path"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(path.step_count == 4);
    ASSERT(path.steps[1].frame_index == 1);
    ASSERT(path.steps[1].state_index == 1);
    ASSERT(!path.steps[1].is_blank);
    ASSERT(path.steps[1].token_index == 0);
    ASSERT(path.steps[1].token_id == 1);
    ASSERT(path.steps[2].frame_index == 2);
    ASSERT(path.steps[2].state_index == 2);
    ASSERT(path.steps[2].is_blank);
    ASSERT(path.steps[2].token_id == 0);
    ASSERT(path.steps[3].frame_index == 3);
    ASSERT(path.steps[3].state_index == 3);
    ASSERT(!path.steps[3].is_blank);
    ASSERT(path.steps[3].token_index == 1);
    ASSERT(path.steps[3].token_id == 1);

    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_backtracks_edge_stars(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    LrcCtcTokenSpans spans = {0};
    int32 target_token_ids[] = {1, 2};
    float values[] = {
        -5.00f, -5.00f, -5.00f,
        -5.00f, -0.10f, -5.00f,
        -5.00f, -5.00f, -0.20f,
        -5.00f, -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 3);
    if (!lrc_ctc_trellis_score_forward_with_edge_stars(&trellis,
                                                       &emissions,
                                                       target_token_ids,
                                                       2,
                                                       0,
                                                       3,
                                                       &result)) {
        fatal(ctc_align_test_fail("score edge-star path"));
    }
    ASSERT(trellis.state_count == 9);
    ASSERT(trellis.has_edge_stars);
    ASSERT(trellis.star_token_id == 3);

    if (!lrc_ctc_trellis_backtrack_with_edge_stars(&trellis,
                                                   &emissions,
                                                   target_token_ids,
                                                   2,
                                                   0,
                                                   3,
                                                   &path,
                                                   &result)) {
        fatal(ctc_align_test_fail("backtrack edge-star path"));
    }
    ASSERT(path.step_count == 4);
    ASSERT(path.steps[0].state_index == 1);
    ASSERT(path.steps[0].is_star);
    ASSERT(!path.steps[0].is_blank);
    ASSERT(path.steps[0].token_index == -1);
    ASSERT(path.steps[0].token_id == 3);
    ASSERT(path.steps[1].state_index == 3);
    ASSERT(!path.steps[1].is_blank);
    ASSERT(!path.steps[1].is_star);
    ASSERT(path.steps[1].token_index == 0);
    ASSERT(path.steps[1].token_id == 1);
    ASSERT(path.steps[2].state_index == 5);
    ASSERT(!path.steps[2].is_blank);
    ASSERT(!path.steps[2].is_star);
    ASSERT(path.steps[2].token_index == 1);
    ASSERT(path.steps[2].token_id == 2);
    ASSERT(path.steps[3].state_index == 7);
    ASSERT(path.steps[3].is_star);
    ASSERT(path.steps[3].token_index == -1);
    ASSERT(path.steps[3].token_id == 3);

    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     0.25f,
                                     &spans,
                                     &result)) {
        fatal(ctc_align_test_fail("edge-star path to spans"));
    }
    ASSERT(spans.span_count == 2);
    ASSERT(spans.spans[0].token_index == 0);
    ASSERT(spans.spans[0].token_id == 1);
    ASSERT(spans.spans[0].start_frame == 1);
    ASSERT(spans.spans[0].end_frame == 2);
    ASSERT(spans.spans[1].token_index == 1);
    ASSERT(spans.spans[1].token_id == 2);
    ASSERT(spans.spans[1].start_frame == 2);
    ASSERT(spans.spans[1].end_frame == 3);

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_backtracks_segment_stars(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    LrcCtcTokenSpans token_spans = {0};
    int32 target_token_ids[] = {1, 2};
    bool segment_starts[] = {true, true};
    int32 star_token_id;
    bool saw_star;
    float values[] = {
        -5.00f, -5.00f, -5.00f,
        -5.00f, -0.10f, -5.00f,
        -5.00f, -5.00f, -0.10f,
        -5.00f, -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 3);
    star_token_id = (int32)emissions.vocabulary_size;

    if (!lrc_ctc_trellis_score_forward_with_segment_stars(
        &trellis,
        &emissions,
        target_token_ids,
        segment_starts,
        LENGTH(target_token_ids),
        0,
        star_token_id,
        &result
    )) {
        fatal(ctc_align_test_fail("score segment-star path"));
    }
    ASSERT(trellis.has_segment_stars);
    ASSERT(!trellis.has_edge_stars);

    if (!lrc_ctc_trellis_backtrack_with_segment_stars(
        &trellis,
        &emissions,
        target_token_ids,
        segment_starts,
        LENGTH(target_token_ids),
        0,
        star_token_id,
        &path,
        &result
    )) {
        fatal(ctc_align_test_fail("backtrack segment-star path"));
    }

    ASSERT(path.step_count == 4);
    saw_star = false;
    for (int32 i = 0; i < path.step_count; i += 1) {
        if (path.steps[i].is_star) {
            saw_star = true;
            ASSERT(path.steps[i].token_id == star_token_id);
        }
    }
    ASSERT(saw_star);

    if (!lrc_ctc_path_to_token_spans(&path,
                                      &emissions,
                                      0.02f,
                                      &token_spans,
                                      &result)) {
        fatal(ctc_align_test_fail("segment-star token spans"));
    }
    ASSERT(token_spans.span_count == 2);
    ASSERT(token_spans.spans[0].token_index == 0);
    ASSERT(token_spans.spans[1].token_index == 1);

    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_edge_stars_reject_bad_star_token(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    int32 target_token_ids[] = {1};
    float values[] = {
        -5.00f, -5.00f,
        -5.00f, -0.10f,
        -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 3, 2);
    if (lrc_ctc_trellis_score_forward_with_edge_stars(&trellis,
                                                       &emissions,
                                                       target_token_ids,
                                                       1,
                                                       0,
                                                       1,
                                                       &result)) {
        fatal(ctc_align_test_fail("target token accepted as star"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN);

    if (lrc_ctc_trellis_score_forward_with_edge_stars(&trellis,
                                                       &emissions,
                                                       target_token_ids,
                                                       1,
                                                       0,
                                                       3,
                                                       &result)) {
        fatal(ctc_align_test_fail("out-of-range star token accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TARGET_TOKEN);

    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_backtrack_rejects_impossible_alignment(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    int32 target_token_ids[] = {1, 2};
    float values[] = {
        -0.10f, -5.00f, -5.00f,
        -5.00f, -0.10f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 2, 3);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score impossible path"));
    }
    *lrc_ctc_trellis_cell(&trellis, trellis.frame_count - 1, 3) = -INFINITY;
    *lrc_ctc_trellis_cell(&trellis, trellis.frame_count - 1, 4) = -INFINITY;
    if (lrc_ctc_trellis_backtrack(&trellis,
                                  &emissions,
                                  target_token_ids,
                                  2,
                                  0,
                                  &path,
                                  &result)) {
        fatal(ctc_align_test_fail("impossible path accepted"));
    }

    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_IMPOSSIBLE_ALIGNMENT);
    ASSERT(path.steps == NULL);
    ASSERT(path.step_count == 0);

    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_backtrack_rejects_invalid_trellis(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    int32 target_token_ids[] = {1};
    float values[] = {
        -0.10f, -5.00f,
        -5.00f, -0.10f,
    };

    ctc_align_make_emissions(&emissions, values, 2, 2);
    if (lrc_ctc_trellis_backtrack(&trellis,
                                  &emissions,
                                  target_token_ids,
                                  1,
                                  0,
                                  &path,
                                  &result)) {
        fatal(ctc_align_test_fail("unscored trellis accepted"));
    }

    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TRELLIS);
    ASSERT(path.steps == NULL);

    return;
}

static void
ctc_align_test_path_segments_merge_blanks_and_tokens(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcPathSegments segments = {0};
    float values[] = {
        -0.10f, -5.00f, -5.00f,
        -0.20f, -5.00f, -5.00f,
        -5.00f, -0.30f, -5.00f,
        -5.00f, -0.50f, -5.00f,
        -0.40f, -5.00f, -5.00f,
        -5.00f, -5.00f, -0.60f,
        -0.70f, -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 7, 3);
    if (!lrc_ctc_path_allocate(&path, 7, &result)) {
        fatal(ctc_align_test_fail("allocate path segment path"));
    }

    lrc_ctc_path_set_blank_step(&path, 0, 0, 0);
    lrc_ctc_path_set_blank_step(&path, 1, 0, 0);
    lrc_ctc_path_set_token_step(&path, 2, 1, 0, 1);
    lrc_ctc_path_set_token_step(&path, 3, 1, 0, 1);
    lrc_ctc_path_set_blank_step(&path, 4, 2, 0);
    lrc_ctc_path_set_token_step(&path, 5, 3, 1, 2);
    lrc_ctc_path_set_blank_step(&path, 6, 4, 0);
    if (!lrc_ctc_path_to_segments(&path,
                                  &emissions,
                                  0.10f,
                                  &segments,
                                  &result)) {
        fatal(ctc_align_test_fail("merge blank/token path segments"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(segments.segment_count == 5);
    ASSERT(segments.segments[0].is_blank);
    ASSERT(!segments.segments[0].is_star);
    ASSERT(segments.segments[0].token_id == 0);
    ASSERT(segments.segments[0].token_index == -1);
    ASSERT(segments.segments[0].start_frame == 0);
    ASSERT(segments.segments[0].end_frame == 2);
    ASSERT(ctc_align_float_close(segments.segments[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(segments.segments[0].end_seconds,
                                 0.2f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(segments.segments[0].score,
                                 -0.15f,
                                 0.00001f));
    ASSERT(!segments.segments[1].is_blank);
    ASSERT(!segments.segments[1].is_star);
    ASSERT(segments.segments[1].token_id == 1);
    ASSERT(segments.segments[1].token_index == 0);
    ASSERT(segments.segments[1].start_frame == 2);
    ASSERT(segments.segments[1].end_frame == 4);
    ASSERT(ctc_align_float_close(segments.segments[1].score,
                                 -0.40f,
                                 0.00001f));
    ASSERT(segments.segments[2].is_blank);
    ASSERT(segments.segments[2].start_frame == 4);
    ASSERT(segments.segments[2].end_frame == 5);
    ASSERT(!segments.segments[3].is_blank);
    ASSERT(segments.segments[3].token_id == 2);
    ASSERT(segments.segments[3].token_index == 1);
    ASSERT(segments.segments[3].start_frame == 5);
    ASSERT(segments.segments[3].end_frame == 6);
    ASSERT(segments.segments[4].is_blank);
    ASSERT(segments.segments[4].start_frame == 6);
    ASSERT(segments.segments[4].end_frame == 7);

    lrc_ctc_path_segments_destroy(&segments);
    lrc_ctc_path_destroy(&path);

    return;
}

static void
ctc_align_test_path_segments_split_repeated_token_after_blank(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcPathSegments segments = {0};
    float values[] = {
        -5.00f, -0.10f,
        -5.00f, -0.20f,
        -0.30f, -5.00f,
        -5.00f, -0.40f,
        -5.00f, -0.50f,
    };

    ctc_align_make_emissions(&emissions, values, 5, 2);
    if (!lrc_ctc_path_allocate(&path, 5, &result)) {
        fatal(ctc_align_test_fail("allocate repeated segment path"));
    }

    lrc_ctc_path_set_token_step(&path, 0, 1, 0, 1);
    lrc_ctc_path_set_token_step(&path, 1, 1, 0, 1);
    lrc_ctc_path_set_blank_step(&path, 2, 2, 0);
    lrc_ctc_path_set_token_step(&path, 3, 3, 1, 1);
    lrc_ctc_path_set_token_step(&path, 4, 3, 1, 1);
    if (!lrc_ctc_path_to_segments(&path,
                                  &emissions,
                                  0.25f,
                                  &segments,
                                  &result)) {
        fatal(ctc_align_test_fail("split repeated token segments"));
    }

    ASSERT(segments.segment_count == 3);
    ASSERT(!segments.segments[0].is_blank);
    ASSERT(segments.segments[0].token_id == 1);
    ASSERT(segments.segments[0].token_index == 0);
    ASSERT(segments.segments[0].start_frame == 0);
    ASSERT(segments.segments[0].end_frame == 2);
    ASSERT(segments.segments[1].is_blank);
    ASSERT(segments.segments[1].start_frame == 2);
    ASSERT(segments.segments[1].end_frame == 3);
    ASSERT(!segments.segments[2].is_blank);
    ASSERT(segments.segments[2].token_id == 1);
    ASSERT(segments.segments[2].token_index == 1);
    ASSERT(segments.segments[2].start_frame == 3);
    ASSERT(segments.segments[2].end_frame == 5);
    ASSERT(ctc_align_float_close(segments.segments[2].end_seconds,
                                 1.25f,
                                 0.00001f));

    lrc_ctc_path_segments_destroy(&segments);
    lrc_ctc_path_destroy(&path);

    return;
}

static void
ctc_align_test_path_segments_keep_stars(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcPathSegments segments = {0};
    int32 star_token_id;
    float values[] = {
        -5.00f, -5.00f,
        -5.00f, -5.00f,
        -5.00f, -0.10f,
        -0.20f, -5.00f,
        -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 5, 2);
    star_token_id = (int32)emissions.vocabulary_size;
    if (!lrc_ctc_path_allocate(&path, 5, &result)) {
        fatal(ctc_align_test_fail("allocate star segment path"));
    }

    lrc_ctc_path_set_star_step(&path, 0, 1, star_token_id);
    lrc_ctc_path_set_star_step(&path, 1, 1, star_token_id);
    lrc_ctc_path_set_token_step(&path, 2, 3, 0, 1);
    lrc_ctc_path_set_blank_step(&path, 3, 4, 0);
    lrc_ctc_path_set_star_step(&path, 4, 5, star_token_id);
    if (!lrc_ctc_path_to_segments(&path,
                                  &emissions,
                                  0.10f,
                                  &segments,
                                  &result)) {
        fatal(ctc_align_test_fail("keep star path segments"));
    }

    ASSERT(segments.segment_count == 4);
    ASSERT(!segments.segments[0].is_blank);
    ASSERT(segments.segments[0].is_star);
    ASSERT(segments.segments[0].token_id == star_token_id);
    ASSERT(segments.segments[0].token_index == -1);
    ASSERT(segments.segments[0].start_frame == 0);
    ASSERT(segments.segments[0].end_frame == 2);
    ASSERT(ctc_align_float_close(segments.segments[0].score,
                                 0.0f,
                                 0.00001f));
    ASSERT(!segments.segments[1].is_blank);
    ASSERT(!segments.segments[1].is_star);
    ASSERT(segments.segments[1].token_id == 1);
    ASSERT(segments.segments[1].start_frame == 2);
    ASSERT(segments.segments[1].end_frame == 3);
    ASSERT(segments.segments[2].is_blank);
    ASSERT(segments.segments[2].start_frame == 3);
    ASSERT(segments.segments[2].end_frame == 4);
    ASSERT(segments.segments[3].is_star);
    ASSERT(segments.segments[3].start_frame == 4);
    ASSERT(segments.segments[3].end_frame == 5);

    lrc_ctc_path_segments_destroy(&segments);
    lrc_ctc_path_destroy(&path);

    return;
}

static void
ctc_align_test_aligned_intervals_keep_edge_star_order(void) {
    LrcCtcAlignResult result;
    LrcCtcPathSegments segments = {0};
    LrcCtcAlignedTokenIntervals intervals = {0};
    int32 target_token_ids[] = {1, 2};
    int32 star_token_id = 3;

    if (!lrc_ctc_path_segments_allocate(&segments, 5, &result)) {
        fatal(ctc_align_test_fail("allocate edge-star interval segments"));
    }

    ctc_align_set_path_segment(&segments, 0, -1, 0, 2, star_token_id,
                               false, true);
    ctc_align_set_path_segment(&segments, 1, 0, 2, 3, 1, false, false);
    ctc_align_set_path_segment(&segments, 2, -1, 3, 4, 0, true, false);
    ctc_align_set_path_segment(&segments, 3, 1, 4, 5, 2, false, false);
    ctc_align_set_path_segment(&segments, 4, -1, 5, 6, star_token_id,
                               false, true);
    if (!lrc_ctc_path_segments_to_aligned_token_intervals(
        &segments,
        target_token_ids,
        NULL,
        2,
        LRC_CTC_ALIGN_STAR_MODE_EDGES,
        star_token_id,
        &intervals,
        &result
    )) {
        fatal(ctc_align_test_fail("edge-star aligned intervals"));
    }

    ASSERT(intervals.interval_count == 4);
    ASSERT(intervals.intervals[0].is_star);
    ASSERT(intervals.intervals[0].target_token_index == -1);
    ASSERT(intervals.intervals[0].segment_start_index == 0);
    ASSERT(intervals.intervals[0].segment_end_index == 1);
    ASSERT(intervals.intervals[0].token_start_frame == 0);
    ASSERT(intervals.intervals[0].token_end_frame == 2);
    ASSERT(!intervals.intervals[1].is_star);
    ASSERT(intervals.intervals[1].target_token_index == 0);
    ASSERT(intervals.intervals[1].segment_start_index == 1);
    ASSERT(intervals.intervals[1].segment_end_index == 2);
    ASSERT(intervals.intervals[1].token_start_frame == 2);
    ASSERT(intervals.intervals[1].token_end_frame == 3);
    ASSERT(!intervals.intervals[2].is_star);
    ASSERT(intervals.intervals[2].target_token_index == 1);
    ASSERT(intervals.intervals[2].segment_start_index == 3);
    ASSERT(intervals.intervals[2].segment_end_index == 4);
    ASSERT(intervals.intervals[3].is_star);
    ASSERT(intervals.intervals[3].target_token_index == -1);
    ASSERT(intervals.intervals[3].segment_start_index == 4);
    ASSERT(intervals.intervals[3].segment_end_index == 5);

    lrc_ctc_aligned_token_intervals_destroy(&intervals);
    lrc_ctc_path_segments_destroy(&segments);

    return;
}

static void
ctc_align_test_aligned_intervals_keep_segment_star_order(void) {
    LrcCtcAlignResult result;
    LrcCtcPathSegments segments = {0};
    LrcCtcAlignedTokenIntervals intervals = {0};
    int32 target_token_ids[] = {1, 2, 3};
    bool target_segment_starts[] = {true, false, true};
    int32 star_token_id = 4;

    if (!lrc_ctc_path_segments_allocate(&segments, 6, &result)) {
        fatal(ctc_align_test_fail("allocate segment-star intervals"));
    }

    ctc_align_set_path_segment(&segments, 0, -1, 0, 1, star_token_id,
                               false, true);
    ctc_align_set_path_segment(&segments, 1, 0, 1, 2, 1, false, false);
    ctc_align_set_path_segment(&segments, 2, -1, 2, 3, 0, true, false);
    ctc_align_set_path_segment(&segments, 3, 1, 3, 4, 2, false, false);
    ctc_align_set_path_segment(&segments, 4, -1, 4, 5, star_token_id,
                               false, true);
    ctc_align_set_path_segment(&segments, 5, 2, 5, 6, 3, false, false);
    if (!lrc_ctc_path_segments_to_aligned_token_intervals(
        &segments,
        target_token_ids,
        target_segment_starts,
        3,
        LRC_CTC_ALIGN_STAR_MODE_SEGMENT,
        star_token_id,
        &intervals,
        &result
    )) {
        fatal(ctc_align_test_fail("segment-star aligned intervals"));
    }

    ASSERT(intervals.interval_count == 5);
    ASSERT(intervals.intervals[0].is_star);
    ASSERT(intervals.intervals[0].target_token_index == -1);
    ASSERT(intervals.intervals[1].target_token_index == 0);
    ASSERT(intervals.intervals[1].segment_start_index == 1);
    ASSERT(intervals.intervals[2].target_token_index == 1);
    ASSERT(intervals.intervals[2].segment_start_index == 3);
    ASSERT(intervals.intervals[3].is_star);
    ASSERT(intervals.intervals[3].target_token_index == -1);
    ASSERT(intervals.intervals[3].segment_start_index == 4);
    ASSERT(intervals.intervals[4].target_token_index == 2);
    ASSERT(intervals.intervals[4].segment_start_index == 5);
    ASSERT(intervals.intervals[4].token_start_frame == 5);
    ASSERT(intervals.intervals[4].token_end_frame == 6);

    lrc_ctc_aligned_token_intervals_destroy(&intervals);
    lrc_ctc_path_segments_destroy(&segments);

    return;
}


static void
ctc_align_test_pad_intervals_distributes_blank_frames(void) {
    LrcCtcAlignResult result;
    LrcCtcPathSegments segments = {0};
    LrcCtcAlignedTokenIntervals intervals = {0};
    int32 target_token_ids[] = {1, 2};

    if (!lrc_ctc_path_segments_allocate(&segments, 5, &result)) {
        fatal(ctc_align_test_fail("allocate padded intervals segments"));
    }

    ctc_align_set_path_segment(&segments, 0, -1, 0, 10, 0, true, false);
    ctc_align_set_path_segment(&segments, 1, 0, 10, 12, 1, false, false);
    ctc_align_set_path_segment(&segments, 2, -1, 12, 20, 0, true, false);
    ctc_align_set_path_segment(&segments, 3, 1, 20, 22, 2, false, false);
    ctc_align_set_path_segment(&segments, 4, -1, 22, 30, 0, true, false);
    if (!lrc_ctc_path_segments_to_aligned_token_intervals(
        &segments,
        target_token_ids,
        NULL,
        2,
        LRC_CTC_ALIGN_STAR_MODE_NONE,
        -1,
        &intervals,
        &result
    )) {
        fatal(ctc_align_test_fail("build padded intervals"));
    }
    if (!lrc_ctc_pad_token_intervals_with_blanks(&segments,
                                                 0.10f,
                                                 &intervals,
                                                 &result)) {
        fatal(ctc_align_test_fail("pad intervals with blanks"));
    }

    ASSERT(intervals.interval_count == 2);
    ASSERT(intervals.intervals[0].token_start_frame == 10);
    ASSERT(intervals.intervals[0].token_end_frame == 12);
    ASSERT(intervals.intervals[0].padded_start_frame == 0);
    ASSERT(intervals.intervals[0].padded_end_frame == 16);
    ASSERT(ctc_align_float_close(intervals.intervals[0].padded_start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(intervals.intervals[0].padded_end_seconds,
                                 1.6f,
                                 0.00001f));
    ASSERT(intervals.intervals[1].token_start_frame == 20);
    ASSERT(intervals.intervals[1].token_end_frame == 22);
    ASSERT(intervals.intervals[1].padded_start_frame == 16);
    ASSERT(intervals.intervals[1].padded_end_frame == 30);
    ASSERT(ctc_align_float_close(intervals.intervals[1].padded_start_seconds,
                                 1.6f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(intervals.intervals[1].padded_end_seconds,
                                 3.0f,
                                 0.00001f));

    lrc_ctc_aligned_token_intervals_destroy(&intervals);
    lrc_ctc_path_segments_destroy(&segments);

    return;
}

static void
ctc_align_test_pad_intervals_counts_initial_star(void) {
    LrcCtcAlignResult result;
    LrcCtcPathSegments segments = {0};
    LrcCtcAlignedTokenIntervals intervals = {0};
    int32 target_token_ids[] = {1};
    int32 star_token_id = 2;

    if (!lrc_ctc_path_segments_allocate(&segments, 6, &result)) {
        fatal(ctc_align_test_fail("allocate star padding segments"));
    }

    ctc_align_set_path_segment(&segments, 0, -1, 0, 10, 0, true, false);
    ctc_align_set_path_segment(&segments, 1, -1, 10, 12, star_token_id,
                               false, true);
    ctc_align_set_path_segment(&segments, 2, -1, 12, 20, 0, true, false);
    ctc_align_set_path_segment(&segments, 3, 0, 20, 22, 1, false, false);
    ctc_align_set_path_segment(&segments, 4, -1, 22, 30, 0, true, false);
    ctc_align_set_path_segment(&segments, 5, -1, 30, 32, star_token_id,
                               false, true);
    if (!lrc_ctc_path_segments_to_aligned_token_intervals(
        &segments,
        target_token_ids,
        NULL,
        1,
        LRC_CTC_ALIGN_STAR_MODE_EDGES,
        star_token_id,
        &intervals,
        &result
    )) {
        fatal(ctc_align_test_fail("build star padded intervals"));
    }
    if (!lrc_ctc_pad_token_intervals_with_blanks(&segments,
                                                 0.10f,
                                                 &intervals,
                                                 &result)) {
        fatal(ctc_align_test_fail("pad star intervals with blanks"));
    }

    ASSERT(intervals.interval_count == 3);
    ASSERT(intervals.intervals[0].is_star);
    ASSERT(intervals.intervals[0].padded_start_frame == 0);
    ASSERT(intervals.intervals[0].padded_end_frame == 16);
    ASSERT(!intervals.intervals[1].is_star);
    ASSERT(intervals.intervals[1].target_token_index == 0);
    ASSERT(intervals.intervals[1].token_start_frame == 20);
    ASSERT(intervals.intervals[1].token_end_frame == 22);
    ASSERT(intervals.intervals[1].padded_start_frame == 16);
    ASSERT(intervals.intervals[1].padded_end_frame == 26);
    ASSERT(ctc_align_float_close(intervals.intervals[1].padded_start_seconds,
                                 1.6f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(intervals.intervals[1].padded_end_seconds,
                                 2.6f,
                                 0.00001f));
    ASSERT(intervals.intervals[2].is_star);
    ASSERT(intervals.intervals[2].padded_start_frame == 26);
    ASSERT(intervals.intervals[2].padded_end_frame == 32);

    lrc_ctc_aligned_token_intervals_destroy(&intervals);
    lrc_ctc_path_segments_destroy(&segments);

    return;
}

static void
ctc_align_test_token_spans_from_backtracked_path(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    LrcCtcTokenSpans spans = {0};
    int32 target_token_ids[] = {1, 2};
    float values[] = {
        -0.10f, -5.00f, -5.00f,
        -5.00f, -0.10f, -5.00f,
        -5.00f, -5.00f, -0.20f,
        -0.10f, -5.00f, -5.00f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 3);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score span path"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   2,
                                   0,
                                   &path,
                                   &result)) {
        fatal(ctc_align_test_fail("backtrack span path"));
    }
    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     0.5f,
                                     &spans,
                                     &result)) {
        fatal(ctc_align_test_fail("path to token spans"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(spans.span_count == 2);
    ASSERT(spans.spans[0].token_index == 0);
    ASSERT(spans.spans[0].token_id == 1);
    ASSERT(spans.spans[0].start_frame == 1);
    ASSERT(spans.spans[0].end_frame == 2);
    ASSERT(ctc_align_float_close(spans.spans[0].start_seconds,
                                 0.5f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[0].end_seconds,
                                 1.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[0].score,
                                 -0.10f,
                                 0.00001f));
    ASSERT(spans.spans[1].token_index == 1);
    ASSERT(spans.spans[1].token_id == 2);
    ASSERT(spans.spans[1].start_frame == 2);
    ASSERT(spans.spans[1].end_frame == 3);
    ASSERT(ctc_align_float_close(spans.spans[1].start_seconds,
                                 1.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[1].end_seconds,
                                 1.5f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[1].score,
                                 -0.20f,
                                 0.00001f));

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_token_spans_preserve_repeated_tokens(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    LrcCtcPath path = {0};
    LrcCtcTokenSpans spans = {0};
    int32 target_token_ids[] = {1, 1};
    float values[] = {
        -0.10f, -5.00f,
        -5.00f, -0.10f,
        -0.10f, -5.00f,
        -5.00f, -0.20f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 2);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        2,
                                        0,
                                        &result)) {
        fatal(ctc_align_test_fail("score repeated spans"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   2,
                                   0,
                                   &path,
                                   &result)) {
        fatal(ctc_align_test_fail("backtrack repeated spans"));
    }
    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     0.25f,
                                     &spans,
                                     &result)) {
        fatal(ctc_align_test_fail("repeated path to spans"));
    }

    ASSERT(spans.span_count == 2);
    ASSERT(spans.spans[0].token_id == 1);
    ASSERT(spans.spans[1].token_id == 1);
    ASSERT(spans.spans[0].token_index == 0);
    ASSERT(spans.spans[1].token_index == 1);
    ASSERT(spans.spans[0].start_frame == 1);
    ASSERT(spans.spans[1].start_frame == 3);
    ASSERT(ctc_align_float_close(spans.spans[0].start_seconds,
                                 0.25f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[1].start_seconds,
                                 0.75f,
                                 0.00001f));

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);

    return;
}

static void
ctc_align_test_token_spans_collapse_contiguous_steps(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcTokenSpans spans = {0};
    float values[] = {
        -5.00f, -0.20f, -5.00f,
        -5.00f, -0.40f, -5.00f,
        -0.10f, -5.00f, -5.00f,
        -5.00f, -5.00f, -0.30f,
    };

    ctc_align_make_emissions(&emissions, values, 4, 3);
    if (!lrc_ctc_path_allocate(&path, 4, &result)) {
        fatal(ctc_align_test_fail("allocate manual span path"));
    }

    lrc_ctc_path_set_token_step(&path, 0, 1, 0, 1);
    lrc_ctc_path_set_token_step(&path, 1, 1, 0, 1);
    lrc_ctc_path_set_blank_step(&path, 2, 2, 0);
    lrc_ctc_path_set_token_step(&path, 3, 3, 1, 2);
    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     0.1f,
                                     &spans,
                                     &result)) {
        fatal(ctc_align_test_fail("collapse contiguous spans"));
    }

    ASSERT(spans.span_count == 2);
    ASSERT(spans.spans[0].token_index == 0);
    ASSERT(spans.spans[0].token_id == 1);
    ASSERT(spans.spans[0].start_frame == 0);
    ASSERT(spans.spans[0].end_frame == 2);
    ASSERT(ctc_align_float_close(spans.spans[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[0].end_seconds,
                                 0.2f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[0].score,
                                 -0.30f,
                                 0.00001f));
    ASSERT(spans.spans[1].token_index == 1);
    ASSERT(spans.spans[1].token_id == 2);
    ASSERT(spans.spans[1].start_frame == 3);
    ASSERT(spans.spans[1].end_frame == 4);

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);

    return;
}

static void
ctc_align_test_token_spans_reject_bad_inputs(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcTokenSpans spans = {0};
    float values[] = {
        -0.10f, -5.00f,
        -5.00f, -0.20f,
    };

    ctc_align_make_emissions(&emissions, values, 2, 2);
    if (lrc_ctc_path_to_token_spans(NULL,
                                    &emissions,
                                    0.1f,
                                    &spans,
                                    &result)) {
        fatal(ctc_align_test_fail("missing path accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (lrc_ctc_path_to_token_spans(&path,
                                    &emissions,
                                    0.1f,
                                    &spans,
                                    &result)) {
        fatal(ctc_align_test_fail("empty path accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_PATH);

    if (!lrc_ctc_path_allocate(&path, 2, &result)) {
        fatal(ctc_align_test_fail("allocate invalid span path"));
    }
    lrc_ctc_path_set_blank_step(&path, 0, 0, 0);
    lrc_ctc_path_set_token_step(&path, 1, 1, 0, 1);
    if (lrc_ctc_path_to_token_spans(&path,
                                    &emissions,
                                    0.0f,
                                    &spans,
                                    &result)) {
        fatal(ctc_align_test_fail("zero frame duration accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_FRAME_DURATION);

    path.steps[1].token_id = 2;
    if (lrc_ctc_path_to_token_spans(&path,
                                    &emissions,
                                    0.1f,
                                    &spans,
                                    &result)) {
        fatal(ctc_align_test_fail("bad path token id accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_PATH);
    ASSERT(spans.spans == NULL);

    lrc_ctc_path_destroy(&path);

    return;
}


static void
ctc_align_test_token_spans_reject_out_of_order_targets(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcTokenSpans spans = {0};
    float values[] = {
        -5.00f, -0.10f,
    };

    ctc_align_make_emissions(&emissions, values, 1, 2);
    if (!lrc_ctc_path_allocate(&path, 1, &result)) {
        fatal(ctc_align_test_fail("allocate out-of-order span path"));
    }

    lrc_ctc_path_set_token_step(&path, 0, 3, 1, 1);
    if (lrc_ctc_path_to_token_spans(&path,
                                    &emissions,
                                    0.1f,
                                    &spans,
                                    &result)) {
        fatal(ctc_align_test_fail("out-of-order path target accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_PATH);
    ASSERT(spans.spans == NULL);

    lrc_ctc_path_destroy(&path);

    return;
}




static void
ctc_align_test_padded_token_spans_use_blank_boundaries(void) {
    LrcCtcAlignResult result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcTokenSpans spans = {0};
    int32 target_token_ids[] = {1, 2};
    float values[30*3];

    for (int32 i = 0; i < LENGTH(values); i += 1) {
        values[i] = -0.10f;
    }

    ctc_align_make_emissions(&emissions, values, 30, 3);
    if (!lrc_ctc_path_allocate(&path, 30, &result)) {
        fatal(ctc_align_test_fail("allocate padded token path"));
    }

    for (int32 i = 0; i < path.step_count; i += 1) {
        if ((i >= 10) && (i < 12)) {
            lrc_ctc_path_set_token_step(&path, i, 1, 0, 1);
        } else if ((i >= 20) && (i < 22)) {
            lrc_ctc_path_set_token_step(&path, i, 3, 1, 2);
        } else {
            lrc_ctc_path_set_blank_step(&path, i, 0, 0);
        }
    }

    if (!lrc_ctc_path_to_padded_token_spans(&path,
                                            &emissions,
                                            target_token_ids,
                                            LENGTH(target_token_ids),
                                            0.01f,
                                            &spans,
                                            &result)) {
        fatal(ctc_align_test_fail("convert padded token spans"));
    }

    ASSERT(spans.span_count == 2);
    ASSERT(spans.spans[0].start_frame == 10);
    ASSERT(spans.spans[0].end_frame == 12);
    ASSERT(spans.spans[0].padded_start_frame == 0);
    ASSERT(spans.spans[0].padded_end_frame == 16);
    ASSERT(ctc_align_float_close(spans.spans[0].padded_start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[0].padded_end_seconds,
                                 0.16f,
                                 0.00001f));
    ASSERT(spans.spans[1].start_frame == 20);
    ASSERT(spans.spans[1].end_frame == 22);
    ASSERT(spans.spans[1].padded_start_frame == 16);
    ASSERT(spans.spans[1].padded_end_frame == 30);
    ASSERT(ctc_align_float_close(spans.spans[1].padded_start_seconds,
                                 0.16f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(spans.spans[1].padded_end_seconds,
                                 0.30f,
                                 0.00001f));

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);

    return;
}


static void
ctc_align_test_synthetic_lrc_uses_active_token_boundaries(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult align_result;
    LrcCtcPath path = {0};
    LrcCtcEmissions emissions;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcOutputLine output_lines[3];
    LrcWriteResult write_result;
    char temp_dir[PATH_MAX];
    char lrc_path[PATH_MAX];
    char text[] = "a\nb\nc\n";
    char expected_lrc[] = "[00:00.10]a\n[00:00.90]b\n[00:01.00]c\n";
    char *written_lrc;
    int32 written_lrc_len;
    int32 target_token_ids[3];
    float *values;
    int32 first_start;
    int32 first_end;
    int32 second_start;
    int32 second_end;
    int32 third_start;
    int32 third_end;
    int32 frame_count;
    int32 vocabulary_size;
    int64 value_count;
    bool ok;

    lrc_ctc_tokenizer_init(&tokenizer);

    values = NULL;
    written_lrc = NULL;
    written_lrc_len = 0;
    first_start = 10;
    first_end = 11;
    second_start = 90;
    second_end = 91;
    third_start = 100;
    third_end = 101;
    frame_count = 110;
    value_count = 0;
    ok = true;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_padded_lrc");
    test_join_path(lrc_path, SIZEOF(lrc_path), temp_dir, "out.lrc");

    if (!ctc_align_load_lyrics_text(&lyrics, text, strlen32(text))) {
        ok = false;
    }
    if (ok && !ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        ok = false;
    }
    if (ok && !ctc_align_load_no_space_alphabet_tokenizer(&tokenizer)) {
        ok = false;
    }
    if (ok && !lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                                     &normalized,
                                                     &tokens,
                                                     &tokenize_result)) {
        ok = false;
    }
    if (ok) {
        ASSERT(tokens.token_count == LENGTH(target_token_ids));
        ASSERT(tokens.tokens[0].line_index == 0);
        ASSERT(tokens.tokens[1].line_index == 1);
        ASSERT(tokens.tokens[2].line_index == 2);

        for (int32 i = 0; i < tokens.token_count; i += 1) {
            target_token_ids[i] = tokens.tokens[i].token_id;
        }
    }

    vocabulary_size = tokenizer.token_count;
    if (ok && ((int64)frame_count > INT64_MAX/vocabulary_size)) {
        ok = false;
    }
    if (ok) {
        value_count = (int64)frame_count*(int64)vocabulary_size;
        values = malloc2(value_count*SIZEOF(*values));
        for (int32 i = 0; i < value_count; i += 1) {
            values[i] = -0.10f;
        }
        ctc_align_make_emissions(&emissions,
                                 values,
                                 frame_count,
                                 vocabulary_size);
    }

    if (ok && !lrc_ctc_path_allocate(&path, frame_count, &align_result)) {
        ok = false;
    }
    if (ok) {
        for (int32 i = 0; i < path.step_count; i += 1) {
            if ((i >= first_start) && (i < first_end)) {
                lrc_ctc_path_set_token_step(&path,
                                            i,
                                            1,
                                            0,
                                            target_token_ids[0]);
                continue;
            }
            if ((i >= second_start) && (i < second_end)) {
                lrc_ctc_path_set_token_step(&path,
                                            i,
                                            3,
                                            1,
                                            target_token_ids[1]);
                continue;
            }
            if ((i >= third_start) && (i < third_end)) {
                lrc_ctc_path_set_token_step(&path,
                                            i,
                                            5,
                                            2,
                                            target_token_ids[2]);
                continue;
            }
            lrc_ctc_path_set_blank_step(&path, i, 0, tokenizer.blank_id);
        }
    }

    if (ok && !lrc_ctc_path_to_padded_token_spans(&path,
                                                  &emissions,
                                                  target_token_ids,
                                                  LENGTH(target_token_ids),
                                                  0.01f,
                                                  &token_spans,
                                                  &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_token_spans_to_word_spans(&token_spans,
                                                 &tokens,
                                                 &normalized,
                                                 &word_spans,
                                                 &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                                     &normalized,
                                                     &line_timestamps,
                                                     &align_result)) {
        ok = false;
    }
    if (ok) {
        ASSERT(token_spans.span_count == 3);
        ASSERT(token_spans.spans[0].start_frame == first_start);
        ASSERT(token_spans.spans[1].start_frame == second_start);
        ASSERT(token_spans.spans[2].start_frame == third_start);
        ASSERT(token_spans.spans[0].padded_start_frame == 0);
        ASSERT(token_spans.spans[1].padded_start_frame == 50);
        ASSERT(token_spans.spans[2].padded_start_frame == 95);

        ASSERT(word_spans.span_count == 3);
        ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                     0.10f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                     0.90f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(word_spans.spans[2].start_seconds,
                                     1.00f,
                                     0.00001f));

        ASSERT(line_timestamps.line_count == 3);
        ASSERT(ctc_align_float_close(line_timestamps.lines[0].start_seconds,
                                     0.10f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(line_timestamps.lines[0].end_seconds,
                                     0.11f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(line_timestamps.lines[1].start_seconds,
                                     0.90f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(line_timestamps.lines[1].end_seconds,
                                     0.91f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(line_timestamps.lines[2].start_seconds,
                                     1.00f,
                                     0.00001f));
        ASSERT(ctc_align_float_close(line_timestamps.lines[2].end_seconds,
                                     1.01f,
                                     0.00001f));
    }

    if (ok && !ctc_align_output_lines_from_timestamps(&lyrics,
                                                      &line_timestamps,
                                                      output_lines)) {
        ok = false;
    }
    if (ok && !lrc_write_output_file(lrc_path,
                                     output_lines,
                                     LENGTH(output_lines),
                                     &write_result)) {
        ok = false;
    }
    if (ok) {
        if ((written_lrc_len = read_entire_file(lrc_path, &written_lrc)) < 0) {
            ok = false;
        } else if (!STREQUAL(written_lrc, written_lrc_len, expected_lrc)) {
            ok = false;
        }
    }

    free2(written_lrc, ((int32)written_lrc_len + 1)*SIZEOF(*written_lrc));
    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_path_destroy(&path);
    free2(values, value_count*SIZEOF(*values));
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);
    test_remove_tree(temp_dir);

    if (!ok) {
        fatal(ctc_align_test_fail("synthetic active lrc timing"));
    }

    return;
}

static void
ctc_align_test_word_spans_use_active_token_boundaries(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcAlignResult result;
    char text[] = "ab cd\n";

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load active word lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make active word token spans"));
    }

    ASSERT(tokens.token_count == 5);
    token_spans.spans[0].padded_start_seconds = 0.03f;
    token_spans.spans[1].padded_end_seconds = 0.33f;
    token_spans.spans[3].padded_start_seconds = 0.44f;
    token_spans.spans[3].padded_end_seconds = 0.55f;
    token_spans.spans[4].padded_start_seconds = 0.55f;
    token_spans.spans[4].padded_end_seconds = 0.78f;

    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert active word spans"));
    }

    ASSERT(word_spans.span_count == 2);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("ab"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 1,
                               STRLIT("cd"));
    ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].end_seconds,
                                 0.20f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                 0.30f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].end_seconds,
                                 0.50f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_segment_word_spans_use_active_token_boundaries(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    LrcLyricsPreprocessOptions options;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult result;
    char text[] = "Hi.world stop\n";

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!ctc_align_load_lyrics_text(&lyrics, text, strlen32(text))) {
        fatal(ctc_align_test_fail("load active segment lyrics"));
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        fatal(ctc_align_test_fail("normalize active segment lyrics"));
    }
    if (!ctc_align_load_no_space_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load active segment tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize active segment lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make active segment token spans"));
    }

    ASSERT(tokens.token_count == 11);
    token_spans.spans[0].padded_start_seconds = 0.02f;
    token_spans.spans[6].padded_end_seconds = 0.88f;
    token_spans.spans[7].padded_start_seconds = 0.91f;
    token_spans.spans[7].padded_end_seconds = 1.00f;
    token_spans.spans[8].padded_start_seconds = 1.00f;
    token_spans.spans[8].padded_end_seconds = 1.10f;
    token_spans.spans[9].padded_start_seconds = 1.10f;
    token_spans.spans[9].padded_end_seconds = 1.20f;
    token_spans.spans[10].padded_start_seconds = 1.20f;
    token_spans.spans[10].padded_end_seconds = 1.42f;

    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert active segment word spans"));
    }

    ASSERT(word_spans.span_count == 2);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("hi world"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 1,
                               STRLIT("stop"));
    ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].end_seconds,
                                 0.70f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                 0.70f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].end_seconds,
                                 1.10f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_word_spans_group_generated_words(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcAlignResult result;
    char text[] = "Hi, Bob!\nNext line\n";

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load generated word lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make generated token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert generated word spans"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(word_spans.span_count == 4);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("hi"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 1,
                               STRLIT("bob"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 2,
                               STRLIT("next"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 3,
                               STRLIT("line"));
    ASSERT(word_spans.spans[0].line_index == 0);
    ASSERT(word_spans.spans[1].line_index == 0);
    ASSERT(word_spans.spans[2].line_index == 1);
    ASSERT(word_spans.spans[3].line_index == 1);
    ASSERT(word_spans.spans[0].token_start_index == 0);
    ASSERT(word_spans.spans[0].token_end_index == 2);
    ASSERT(word_spans.spans[1].token_start_index == 3);
    ASSERT(word_spans.spans[1].token_end_index == 6);
    ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].end_seconds,
                                 0.2f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                 0.3f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[2].start_seconds,
                                 0.7f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].score,
                                 -0.105f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}


static void
ctc_align_test_word_spans_use_skipped_space_gaps(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult result;
    char text[] = "Hi Bob\n";

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!ctc_align_load_lyrics_text(&lyrics, text, strlen32(text))) {
        fatal(ctc_align_test_fail("load skipped-space lyrics"));
    }
    if (!ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        fatal(ctc_align_test_fail("normalize skipped-space lyrics"));
    }
    if (!ctc_align_load_no_space_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load no-space word tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize skipped-space lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make skipped-space token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert skipped-space word spans"));
    }

    ASSERT_EQUAL(normalized.text, "hi bob");
    ASSERT(tokens.token_count == 5);
    ASSERT(tokens.tokens[0].normalized_start == 0);
    ASSERT(tokens.tokens[1].normalized_start == 1);
    ASSERT(tokens.tokens[2].normalized_start == 3);
    ASSERT(word_spans.span_count == 2);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("hi"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 1,
                               STRLIT("bob"));
    ASSERT(word_spans.spans[0].token_start_index == 0);
    ASSERT(word_spans.spans[0].token_end_index == 2);
    ASSERT(word_spans.spans[1].token_start_index == 2);
    ASSERT(word_spans.spans[1].token_end_index == 5);
    ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                 0.2f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_word_spans_handle_removed_punctuation(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcAlignResult result;
    char text[] = "A---B   C\n";

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load punctuation word lyrics"));
    }
    ASSERT_EQUAL(normalized.text, "ab c");
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.5f,
                                                0.25f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make punctuation token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert punctuation word spans"));
    }

    ASSERT(word_spans.span_count == 2);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("ab"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 1,
                               STRLIT("c"));
    ASSERT(word_spans.spans[0].line_index == 0);
    ASSERT(word_spans.spans[1].line_index == 0);
    ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                 0.5f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].end_seconds,
                                 1.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                 1.25f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}


static void
ctc_align_test_word_spans_follow_reference_segments(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    LrcLyricsPreprocessOptions options;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult result;
    char text[] = "Hi.world stop\n";

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!ctc_align_load_lyrics_text(&lyrics, text, strlen32(text))) {
        fatal(ctc_align_test_fail("load reference segment lyrics"));
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        fatal(ctc_align_test_fail("normalize reference segment lyrics"));
    }
    if (!ctc_align_load_no_space_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load reference segment tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize reference segment lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make reference segment token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert reference segment word spans"));
    }

    ASSERT_EQUAL(normalized.text, "hi world stop");
    ASSERT(tokens.token_count == 11);
    ASSERT(normalized.segment_count == 2);
    ASSERT(word_spans.span_count == 2);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("hi world"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 1,
                               STRLIT("stop"));
    ASSERT(word_spans.spans[0].token_start_index == 0);
    ASSERT(word_spans.spans[0].token_end_index == 7);
    ASSERT(word_spans.spans[1].token_start_index == 7);
    ASSERT(word_spans.spans[1].token_end_index == 11);
    ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].end_seconds,
                                 0.7f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[1].start_seconds,
                                 0.7f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_word_spans_keep_repeated_token_positions(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcAlignResult result;
    char text[] = "aa\n";

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load repeated word lyrics"));
    }
    ASSERT(tokens.token_count == 2);
    ASSERT(tokens.tokens[0].token_id == tokens.tokens[1].token_id);
    if (!lrc_ctc_token_spans_allocate(&token_spans,
                                      tokens.token_count,
                                      &result)) {
        fatal(ctc_align_test_fail("allocate repeated word token spans"));
    }

    token_spans.spans[0].token_index = 0;
    token_spans.spans[0].start_frame = 1;
    token_spans.spans[0].end_frame = 2;
    token_spans.spans[0].start_seconds = 0.10f;
    token_spans.spans[0].end_seconds = 0.20f;
    token_spans.spans[0].score = -0.10f;
    token_spans.spans[0].token_id = tokens.tokens[0].token_id;

    token_spans.spans[1].token_index = 1;
    token_spans.spans[1].start_frame = 3;
    token_spans.spans[1].end_frame = 4;
    token_spans.spans[1].start_seconds = 0.30f;
    token_spans.spans[1].end_seconds = 0.40f;
    token_spans.spans[1].score = -0.30f;
    token_spans.spans[1].token_id = tokens.tokens[1].token_id;

    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert repeated word spans"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(word_spans.span_count == 1);
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("aa"));
    ASSERT(word_spans.spans[0].token_start_index == 0);
    ASSERT(word_spans.spans[0].token_end_index == 2);
    ASSERT(word_spans.spans[0].span_start_index == 0);
    ASSERT(word_spans.spans[0].span_end_index == 2);
    ASSERT(ctc_align_float_close(word_spans.spans[0].start_seconds,
                                 0.10f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].end_seconds,
                                 0.40f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(word_spans.spans[0].score,
                                 -0.20f,
                                 0.00001f));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_line_timestamps_repeated_boundary_alignment(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcCtcEmissions emissions;
    int32 *target_token_ids;
    float *values;
    char text[] = "a\na\n";
    int32 token_count;
    int32 frame_count;
    int32 vocabulary_size;
    int64 value_count;

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!ctc_align_load_lyrics_text(&lyrics, text, strlen32(text))) {
        fatal(ctc_align_test_fail("load repeated boundary lyrics"));
    }
    if (!ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        fatal(ctc_align_test_fail("normalize repeated boundary lyrics"));
    }
    if (!ctc_align_load_no_space_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load repeated boundary tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize repeated boundary lyrics"));
    }
    ASSERT(tokens.token_count == 2);
    ASSERT(tokens.tokens[0].token_id == tokens.tokens[1].token_id);
    ASSERT(tokens.tokens[0].line_index == 0);
    ASSERT(tokens.tokens[1].line_index == 1);

    token_count = tokens.token_count;
    frame_count = 5;
    vocabulary_size = tokenizer.token_count;
    value_count = (int64)frame_count*(int64)vocabulary_size;
    target_token_ids = malloc2(token_count*SIZEOF(*target_token_ids));
    values = malloc2(value_count*SIZEOF(*values));
    for (int32 i = 0; i < token_count; i += 1) {
        target_token_ids[i] = tokens.tokens[i].token_id;
    }
    for (int32 i = 0; i < value_count; i += 1) {
        values[i] = -12.0f;
    }
    for (int32 frame = 0; frame < frame_count; frame += 1) {
        values[frame*vocabulary_size + tokenizer.blank_id] = -0.05f;
    }
    values[1*vocabulary_size + tokenizer.blank_id] = -6.0f;
    values[1*vocabulary_size + target_token_ids[0]] = -0.01f;
    values[3*vocabulary_size + tokenizer.blank_id] = -6.0f;
    values[3*vocabulary_size + target_token_ids[1]] = -0.02f;
    ctc_align_make_emissions(&emissions, values, frame_count, vocabulary_size);

    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        token_count,
                                        tokenizer.blank_id,
                                        &result)) {
        fatal(ctc_align_test_fail("score repeated boundary path"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   token_count,
                                   tokenizer.blank_id,
                                   &path,
                                   &result)) {
        fatal(ctc_align_test_fail("backtrack repeated boundary path"));
    }
    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     0.10f,
                                     &token_spans,
                                     &result)) {
        fatal(ctc_align_test_fail("span repeated boundary path"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("word repeated boundary path"));
    }
    if (!lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                               &normalized,
                                               &line_timestamps,
                                               &result)) {
        fatal(ctc_align_test_fail("line repeated boundary path"));
    }

    ASSERT(path.step_count == 5);
    ASSERT(path.steps[0].state_index == 0);
    ASSERT(path.steps[1].state_index == 1);
    ASSERT(path.steps[2].state_index == 2);
    ASSERT(path.steps[3].state_index == 3);
    ASSERT(path.steps[4].state_index == 4);
    ASSERT(token_spans.span_count == 2);
    ASSERT(token_spans.spans[0].token_index == 0);
    ASSERT(token_spans.spans[1].token_index == 1);
    ASSERT(token_spans.spans[0].start_frame == 1);
    ASSERT(token_spans.spans[1].start_frame == 3);
    ASSERT(word_spans.span_count == 2);
    ASSERT(word_spans.spans[0].line_index == 0);
    ASSERT(word_spans.spans[1].line_index == 1);
    ASSERT(line_timestamps.line_count == 2);
    ASSERT(line_timestamps.lines[0].kind
           == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
    ASSERT(line_timestamps.lines[0].line_index == 0);
    ASSERT(line_timestamps.lines[0].word_start_index == 0);
    ASSERT(line_timestamps.lines[0].word_end_index == 1);
    ASSERT(ctc_align_float_close(line_timestamps.lines[0].start_seconds,
                                 0.10f,
                                 0.00001f));
    ASSERT(line_timestamps.lines[1].kind
           == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
    ASSERT(line_timestamps.lines[1].line_index == 1);
    ASSERT(line_timestamps.lines[1].word_start_index == 1);
    ASSERT(line_timestamps.lines[1].word_end_index == 2);
    ASSERT(ctc_align_float_close(line_timestamps.lines[1].start_seconds,
                                 0.30f,
                                 0.00001f));

    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);
    free2(values, value_count*SIZEOF(*values));
    free2(target_token_ids, token_count*SIZEOF(*target_token_ids));
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_word_spans_reject_bad_inputs(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcAlignResult result;
    char text[] = "a b\n";

    if (lrc_ctc_token_spans_to_word_spans(NULL,
                                          &tokens,
                                          &normalized,
                                          &word_spans,
                                          &result)) {
        fatal(ctc_align_test_fail("missing token spans accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load bad-input word lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make bad-input token spans"));
    }

    token_spans.span_count -= 1;
    if (lrc_ctc_token_spans_to_word_spans(&token_spans,
                                          &tokens,
                                          &normalized,
                                          &word_spans,
                                          &result)) {
        fatal(ctc_align_test_fail("short token spans accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS);
    token_spans.span_count += 1;

    token_spans.spans[0].token_id = 999;
    if (lrc_ctc_token_spans_to_word_spans(&token_spans,
                                          &tokens,
                                          &normalized,
                                          &word_spans,
                                          &result)) {
        fatal(ctc_align_test_fail("mismatched token span accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TOKEN_SPANS);
    token_spans.spans[0].token_id = tokens.tokens[0].token_id;

    tokens.tokens[0].normalized_end = normalized.text_len;
    if (lrc_ctc_token_spans_to_word_spans(&token_spans,
                                          &tokens,
                                          &normalized,
                                          &word_spans,
                                          &result)) {
        fatal(ctc_align_test_fail("mixed word/space token accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_TOKENIZED_TEXT);

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_maxwell_word_line_mapping(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsNormalized normalized = {0};
    LrcLyricsLoadResult lyrics_result;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcAlignResult result;
    char *lyrics_path;
    int32 expected_lines[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2,
        4, 4, 4, 4, 4,
        5, 5, 5, 5, 5,
    };

    lyrics_path = getenv("LRC_TEST_MAXWELL_TXT");
    if (lyrics_path == NULL) {
        return;
    }

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!lrc_lyrics_load_file(&lyrics, lyrics_path, &lyrics_result)) {
        fatal(ctc_align_test_fail("load maxwell word lyrics"));
    }
    if (!ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        fatal(ctc_align_test_fail("normalize maxwell word lyrics"));
    }
    if (!ctc_align_load_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load maxwell word tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize maxwell word lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.02f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make maxwell word token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert maxwell word spans"));
    }

    ASSERT(word_spans.span_count == LENGTH(expected_lines));
    for (int32 i = 0; i < word_spans.span_count; i += 1) {
        LrcCtcWordSpan *word = word_spans.spans + i;
        int32 line_start;
        int32 line_end;

        ASSERT(word->line_index == expected_lines[i]);
        ASSERT(lrc_lyrics_normalized_line_range(&normalized,
                                                word->line_index,
                                                &line_start,
                                                &line_end));
        ASSERT(word->normalized_start >= line_start);
        ASSERT(word->normalized_end <= line_end);
    }
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 0,
                               STRLIT("can"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 22,
                               STRLIT("bang"));
    ctc_align_assert_word_text(&normalized,
                               word_spans.spans + 27,
                               STRLIT("came"));

    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}



static void
ctc_align_test_line_timestamps_from_generated_words(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcCtcAlignResult result;
    char text[] = "Alpha beta\n\nGamma!\n!!!\nDelta\n";

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load generated line lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make generated line token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert generated line word spans"));
    }
    if (!lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                               &normalized,
                                               &line_timestamps,
                                               &result)) {
        fatal(ctc_align_test_fail("convert generated line timestamps"));
    }

    ASSERT(result.header.error == LS_ERROR_NONE);
    ASSERT(line_timestamps.line_count == 4);
    ASSERT(line_timestamps.timestamped_line_count == 3);
    ASSERT(line_timestamps.blank_line_count == 1);

    ASSERT(line_timestamps.lines[0].kind
           == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
    ASSERT(line_timestamps.lines[0].line_index == 0);
    ASSERT(line_timestamps.lines[0].word_start_index == 0);
    ASSERT(line_timestamps.lines[0].word_end_index == 2);
    ASSERT(ctc_align_float_close(line_timestamps.lines[0].start_seconds,
                                 0.0f,
                                 0.00001f));
    ASSERT(ctc_align_float_close(line_timestamps.lines[0].end_seconds,
                                 1.0f,
                                 0.00001f));

    ASSERT(line_timestamps.lines[1].kind
           == LRC_CTC_LINE_TIMESTAMP_KIND_BLANK);
    ASSERT(line_timestamps.lines[1].line_index == 1);
    ASSERT(line_timestamps.lines[1].word_start_index == -1);
    ASSERT(line_timestamps.lines[1].word_end_index == -1);

    ASSERT(line_timestamps.lines[2].kind
           == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
    ASSERT(line_timestamps.lines[2].line_index == 2);
    ASSERT(line_timestamps.lines[2].word_start_index == 2);
    ASSERT(line_timestamps.lines[2].word_end_index == 3);
    ASSERT(ctc_align_float_close(line_timestamps.lines[2].start_seconds,
                                 1.1f,
                                 0.00001f));

    ASSERT(line_timestamps.lines[3].kind
           == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
    ASSERT(line_timestamps.lines[3].line_index == 4);
    ASSERT(line_timestamps.lines[3].word_start_index == 3);
    ASSERT(line_timestamps.lines[3].word_end_index == 4);
    ASSERT(ctc_align_float_close(line_timestamps.lines[3].start_seconds,
                                 1.7f,
                                 0.00001f));

    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_line_timestamps_reject_bad_inputs(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens;
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcCtcAlignResult result;
    char text[] = "a b\n";

    if (lrc_ctc_word_spans_to_line_timestamps(NULL,
                                              &normalized,
                                              &line_timestamps,
                                              &result)) {
        fatal(ctc_align_test_fail("missing word spans accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    if (!ctc_align_load_tokenized_lyrics(text,
                                         strlen32(text),
                                         &lyrics,
                                         &normalized,
                                         &tokenizer,
                                         &tokens)) {
        fatal(ctc_align_test_fail("load bad line lyrics"));
    }
    if (!ctc_align_make_token_spans_from_tokens(&tokens,
                                                0.0f,
                                                0.10f,
                                                &token_spans)) {
        fatal(ctc_align_test_fail("make bad line token spans"));
    }
    if (!lrc_ctc_token_spans_to_word_spans(&token_spans,
                                           &tokens,
                                           &normalized,
                                           &word_spans,
                                           &result)) {
        fatal(ctc_align_test_fail("convert bad line word spans"));
    }

    word_spans.spans[0].line_index = -1;
    if (lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                              &normalized,
                                              &line_timestamps,
                                              &result)) {
        fatal(ctc_align_test_fail("bad word line accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS);
    word_spans.spans[0].line_index = 0;

    word_spans.spans[1].line_index = 0;
    word_spans.spans[1].start_seconds = -INFINITY;
    if (lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                              &normalized,
                                              &line_timestamps,
                                              &result)) {
        fatal(ctc_align_test_fail("bad word timing accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_WORD_SPANS);

    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_maxwell_line_timestamp_comparison(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsNormalized normalized = {0};
    LrcLyricsLoadResult lyrics_result;
    LrcParsedFile parsed = {0};
    LrcParseResult parse_result;
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcCtcAlignResult align_result;
    char *lyrics_path;
    char *lrc_path;
    char *lrc_text;
    int32 lrc_text_len;
    int32 word_index;

    lyrics_path = getenv("LRC_TEST_MAXWELL_TXT");
    if (lyrics_path == NULL) {
        lyrics_path = "next-phase/maxwell.txt";
    }
    lrc_path = getenv("LRC_TEST_MAXWELL_LRC");
    if (lrc_path == NULL) {
        lrc_path = "next-phase/maxwell.lrc";
    }
    if (!util_file_exists(lyrics_path) || !util_file_exists(lrc_path)) {
        return;
    }

    if (!lrc_lyrics_load_file(&lyrics, lyrics_path, &lyrics_result)) {
        fatal(ctc_align_test_fail("load maxwell line lyrics"));
    }
    if (!ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        fatal(ctc_align_test_fail("normalize maxwell line lyrics"));
    }

    if ((lrc_text_len = read_entire_file(lrc_path, &lrc_text)) < 0) {
        fatal(ctc_align_test_fail("read maxwell expected lrc"));
    }
    if (!lrc_parse_text(&parsed, lrc_text, lrc_text_len, &parse_result)) {
        free2(lrc_text, ((int32)lrc_text_len + 1)*SIZEOF(*lrc_text));
        fatal(ctc_align_test_fail("parse maxwell expected lrc"));
    }
    if (!lrc_ctc_word_spans_allocate(&word_spans,
                                     parsed.timestamped_line_count,
                                     &align_result)) {
        free2(lrc_text, ((int32)lrc_text_len + 1)*SIZEOF(*lrc_text));
        fatal(ctc_align_test_fail("allocate maxwell expected words"));
    }

    word_index = 0;
    for (int32 i = 0; i < parsed.line_count; i += 1) {
        LrcParsedLine *line = parsed.lines + i;
        LrcCtcWordSpan *word;
        int32 line_start;
        int32 line_end;

        if (line->kind != LRC_PARSED_LINE_KIND_TIMESTAMPED) {
            continue;
        }
        ASSERT(lrc_lyrics_normalized_line_range(&normalized,
                                                line->source_line_index,
                                                &line_start,
                                                &line_end));
        word = word_spans.spans + word_index;
        word->word_index = word_index;
        word->token_start_index = word_index;
        word->token_end_index = word_index + 1;
        word->span_start_index = word_index;
        word->span_end_index = word_index + 1;
        word->normalized_start = line_start;
        word->normalized_end = line_end;
        word->line_index = line->source_line_index;
        word->start_seconds = line->timestamp_seconds;
        word->end_seconds = line->timestamp_seconds + 0.5f;
        word->score = -0.10f;
        word_index += 1;
    }
    ASSERT(word_index == word_spans.span_count);

    if (!lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                               &normalized,
                                               &line_timestamps,
                                               &align_result)) {
        free2(lrc_text, ((int32)lrc_text_len + 1)*SIZEOF(*lrc_text));
        fatal(ctc_align_test_fail("convert maxwell line timestamps"));
    }

    ASSERT(line_timestamps.line_count == parsed.line_count);
    ASSERT(line_timestamps.timestamped_line_count
           == parsed.timestamped_line_count);
    ASSERT(line_timestamps.blank_line_count == parsed.blank_line_count);
    for (int32 i = 0; i < parsed.line_count; i += 1) {
        LrcParsedLine *expected = parsed.lines + i;
        LrcCtcLineTimestamp *actual = line_timestamps.lines + i;

        ASSERT(actual->line_index == expected->source_line_index);
        if (expected->kind == LRC_PARSED_LINE_KIND_BLANK) {
            ASSERT(actual->kind == LRC_CTC_LINE_TIMESTAMP_KIND_BLANK);
            continue;
        }

        ASSERT(actual->kind == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
        ASSERT(ctc_align_float_close(actual->start_seconds,
                                     expected->timestamp_seconds,
                                     0.015f));
    }

    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_parsed_file_destroy(&parsed);
    free2(lrc_text, ((int32)lrc_text_len + 1)*SIZEOF(*lrc_text));
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_full_synthetic_alignment_pipeline(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult align_result;
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcTokenSpans spans = {0};
    LrcCtcEmissions emissions;
    int32 *target_token_ids;
    float *values;
    char text[] = "AB, cab!\n";
    float frame_duration_seconds;
    int32 token_count;
    int32 frame_count;
    int32 vocabulary_size;
    int64 value_count;

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!ctc_align_load_lyrics_text(&lyrics, text, strlen32(text))) {
        fatal(ctc_align_test_fail("load synthetic lyrics"));
    }
    if (!ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        fatal(ctc_align_test_fail("normalize synthetic lyrics"));
    }
    ASSERT_EQUAL(normalized.text, "ab cab");

    if (!ctc_align_load_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load synthetic tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize synthetic lyrics"));
    }

    ASSERT(tokens.token_count == normalized.text_len);
    for (int32 i = 0; i < tokens.token_count; i += 1) {
        ASSERT(tokens.tokens[i].normalized_start == i);
        ASSERT(tokens.tokens[i].normalized_end == i + 1);
        ASSERT(tokens.tokens[i].line_index == 0);
    }

    token_count = tokens.token_count;
    frame_count = token_count + 2;
    vocabulary_size = tokenizer.token_count;
    value_count = (int64)frame_count*(int64)vocabulary_size;
    target_token_ids = malloc2(token_count*SIZEOF(*target_token_ids));
    values = malloc2(value_count*SIZEOF(*values));
    for (int32 i = 0; i < token_count; i += 1) {
        target_token_ids[i] = tokens.tokens[i].token_id;
    }
    ctc_align_fill_predictable_values(values,
                                      frame_count,
                                      vocabulary_size,
                                      tokenizer.blank_id,
                                      target_token_ids,
                                      token_count);
    ctc_align_make_emissions(&emissions, values, frame_count, vocabulary_size);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        token_count,
                                        tokenizer.blank_id,
                                        &align_result)) {
        fatal(ctc_align_test_fail("score synthetic full path"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   token_count,
                                   tokenizer.blank_id,
                                   &path,
                                   &align_result)) {
        fatal(ctc_align_test_fail("backtrack synthetic full path"));
    }

    frame_duration_seconds = 0.125f;
    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     frame_duration_seconds,
                                     &spans,
                                     &align_result)) {
        fatal(ctc_align_test_fail("span synthetic full path"));
    }

    ASSERT(align_result.header.error == LS_ERROR_NONE);
    ASSERT(spans.span_count == token_count);
    for (int32 i = 0; i < spans.span_count; i += 1) {
        float expected_start = (float)(i + 1)*frame_duration_seconds;
        float expected_end = (float)(i + 2)*frame_duration_seconds;

        ASSERT(spans.spans[i].token_index == i);
        ASSERT(spans.spans[i].token_id == target_token_ids[i]);
        ASSERT(spans.spans[i].start_frame == i + 1);
        ASSERT(spans.spans[i].end_frame == i + 2);
        ASSERT(ctc_align_float_close(spans.spans[i].start_seconds,
                                     expected_start,
                                     0.00001f));
        ASSERT(ctc_align_float_close(spans.spans[i].end_seconds,
                                     expected_end,
                                     0.00001f));
        ASSERT(ctc_align_float_close(spans.spans[i].score,
                                     -0.05f,
                                     0.00001f));
    }

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);
    free2(values, value_count*SIZEOF(*values));
    free2(target_token_ids, token_count*SIZEOF(*target_token_ids));
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}

static void
ctc_align_test_maxwell_fake_token_timing(void) {
    LrcLyrics lyrics = {0};
    LrcLyricsNormalized normalized = {0};
    LrcLyricsLoadResult lyrics_result;
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcAlignResult align_result;
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcTokenSpans spans = {0};
    LrcCtcEmissions emissions;
    int32 *target_token_ids;
    float *values;
    char *lyrics_path;
    int32 token_count;
    int32 frame_count;
    int32 vocabulary_size;
    int64 value_count;

    lyrics_path = getenv("LRC_TEST_MAXWELL_TXT");
    if (lyrics_path == NULL) {
        return;
    }

    lrc_ctc_tokenizer_init(&tokenizer);
    if (!lrc_lyrics_load_file(&lyrics, lyrics_path, &lyrics_result)) {
        fatal(ctc_align_test_fail("load maxwell lyrics"));
    }
    if (!ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        fatal(ctc_align_test_fail("normalize maxwell lyrics"));
    }
    if (!ctc_align_load_alphabet_tokenizer(&tokenizer)) {
        fatal(ctc_align_test_fail("load maxwell test tokenizer"));
    }
    if (!lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                               &normalized,
                                               &tokens,
                                               &tokenize_result)) {
        fatal(ctc_align_test_fail("tokenize maxwell lyrics"));
    }
    ASSERT(tokens.token_count > 0);

    token_count = tokens.token_count;
    frame_count = token_count + 2;
    vocabulary_size = tokenizer.token_count;
    value_count = (int64)frame_count*(int64)vocabulary_size;
    target_token_ids = malloc2(token_count*SIZEOF(*target_token_ids));
    values = malloc2(value_count*SIZEOF(*values));
    for (int32 i = 0; i < token_count; i += 1) {
        target_token_ids[i] = tokens.tokens[i].token_id;
    }
    ctc_align_fill_predictable_values(values,
                                      frame_count,
                                      vocabulary_size,
                                      tokenizer.blank_id,
                                      target_token_ids,
                                      token_count);
    ctc_align_make_emissions(&emissions, values, frame_count, vocabulary_size);
    if (!lrc_ctc_trellis_score_forward(&trellis,
                                        &emissions,
                                        target_token_ids,
                                        token_count,
                                        tokenizer.blank_id,
                                        &align_result)) {
        fatal(ctc_align_test_fail("score maxwell fake path"));
    }
    if (!lrc_ctc_trellis_backtrack(&trellis,
                                   &emissions,
                                   target_token_ids,
                                   token_count,
                                   tokenizer.blank_id,
                                   &path,
                                   &align_result)) {
        fatal(ctc_align_test_fail("backtrack maxwell fake path"));
    }
    if (!lrc_ctc_path_to_token_spans(&path,
                                     &emissions,
                                     0.02f,
                                     &spans,
                                     &align_result)) {
        fatal(ctc_align_test_fail("maxwell fake path to spans"));
    }

    ASSERT(spans.span_count == token_count);
    ASSERT(spans.spans[0].token_index == 0);
    ASSERT(spans.spans[0].start_frame == 1);
    ASSERT(ctc_align_float_close(spans.spans[0].start_seconds,
                                 0.02f,
                                 0.00001f));
    ASSERT(spans.spans[token_count - 1].token_index == token_count - 1);
    ASSERT(spans.spans[token_count - 1].start_frame == token_count);
    ASSERT(ctc_align_float_close(spans.spans[token_count - 1].start_seconds,
                                 (float)token_count*0.02f,
                                 0.0001f));

    lrc_ctc_token_spans_destroy(&spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);
    free2(values, value_count*SIZEOF(*values));
    free2(target_token_ids, token_count*SIZEOF(*target_token_ids));
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return;
}


static void
ctc_align_test_full_synthetic_lrc_pipeline(void) {
    AudioTestSineOptions sine_options;
    LrcCtcAudioConfig audio_config;
    LrcCtcAudioResult audio_result;
    LrcCtcAudio audio = {0};
    LrcCtcModelConfig model_config;
    LrcCtcModelInputResult model_result;
    LrcCtcModelInput input = {0};
    LrcLyrics lyrics = {0};
    LrcLyricsLoadResult lyrics_result;
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcFakeInference fake = {0};
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult inference_result;
    LrcCtcEmissions emissions = {0};
    LrcCtcAlignResult align_result;
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcOutputLine *output_lines;
    LrcWriteResult write_result;
    char temp_dir[PATH_MAX];
    char lyrics_path[PATH_MAX];
    char wav_path[PATH_MAX];
    char lrc_path[PATH_MAX];
    char lyrics_text[] = "ab cd\n\nef\n";
    char expected_lrc[] = "[00:00.01]ab cd\n\n[00:00.07]ef\n";
    char *written_lrc;
    int32 written_lrc_len;
    int32 *target_token_ids;
    float *values;
    float frame_duration_seconds;
    int32 frame_count;
    int32 vocabulary_size;
    int64 value_count;
    int32 token_count;
    bool ok;

    if (!test_command_exists("ffmpeg")) {
        return;
    }

    lrc_ctc_tokenizer_init(&tokenizer);

    target_token_ids = NULL;
    values = NULL;
    output_lines = NULL;
    written_lrc = NULL;
    written_lrc_len = 0;
    ok = true;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_full_lrc");
    test_join_path(lyrics_path, SIZEOF(lyrics_path), temp_dir,
                        "lyrics.txt");
    test_join_path(wav_path, SIZEOF(wav_path), temp_dir, "song.wav");
    test_join_path(lrc_path, SIZEOF(lrc_path), temp_dir, "out.lrc");
    write_entire_file(lyrics_path, lyrics_text, strlen32(lyrics_text));

    audio_test_sine_options_init(&sine_options);
    sine_options.format.sample_rate = 16000;
    sine_options.format.channel_count = 2;
    sine_options.duration_seconds = 0.05;
    sine_options.frequency_hz = 330.0;
    if (!audio_test_generate_sine_wav(wav_path, &sine_options, "ffmpeg")) {
        ok = false;
    }

    lrc_ctc_audio_config_init(&audio_config);
    audio_config.sample_rate = 16000;
    if (ok && !lrc_ctc_audio_decode_file(&audio,
                                         wav_path,
                                         &audio_config,
                                         &audio_result)) {
        ok = false;
    }
    if (ok && (audio.channel_count != 1)) {
        ok = false;
    }
    if (ok && (audio.sample_rate != 16000)) {
        ok = false;
    }
    if (ok && (audio.sample_count <= 0)) {
        ok = false;
    }

    lrc_ctc_model_config_init(&model_config);
    model_config.sample_rate = 16000;
    model_config.inputs_to_logits_ratio = 160;
    model_config.window_seconds = 1;
    model_config.context_seconds = 0;
    if (ok && !lrc_ctc_model_input_prepare(&input,
                                           &audio,
                                           &model_config,
                                           &model_result)) {
        ok = false;
    }
    if (ok && ((input.shape_len != 2) || (input.shape[0] != 1)
               || (input.shape[1] != audio.sample_count))) {
        ok = false;
    }

    if (ok && !lrc_lyrics_load_file(&lyrics, lyrics_path, &lyrics_result)) {
        ok = false;
    }
    if (ok && !ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        ok = false;
    }
    if (ok && !ctc_align_load_alphabet_tokenizer(&tokenizer)) {
        ok = false;
    }
    if (ok && !lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                                     &normalized,
                                                     &tokens,
                                                     &tokenize_result)) {
        ok = false;
    }

    if (ok && (tokens.token_count <= 0)) {
        ok = false;
    }
    token_count = tokens.token_count;
    frame_count = token_count + 2;
    vocabulary_size = tokenizer.token_count;
    value_count = (int64)frame_count*(int64)vocabulary_size;
    if (ok) {
        target_token_ids = malloc2(token_count*SIZEOF(*target_token_ids));
        values = malloc2(value_count*SIZEOF(*values));
        for (int32 i = 0; i < token_count; i += 1) {
            target_token_ids[i] = tokens.tokens[i].token_id;
        }
        ctc_align_fill_predictable_values(values,
                                          frame_count,
                                          vocabulary_size,
                                          tokenizer.blank_id,
                                          target_token_ids,
                                          token_count);
    }

    if (ok && !lrc_ctc_fake_inference_set(&fake,
                                          values,
                                          frame_count,
                                          vocabulary_size)) {
        ok = false;
    }
    if (ok) {
        lrc_ctc_fake_inference_backend(&fake, &backend);
        if (!lrc_ctc_inference_run(&backend,
                                   &input,
                                   &emissions,
                                   &inference_result)) {
            ok = false;
        }
    }
    if (ok && !lrc_ctc_emissions_convert_to_log_probabilities(
        &emissions,
        LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES,
        &inference_result
    )) {
        ok = false;
    }

    if (ok && !lrc_ctc_trellis_score_forward(&trellis,
                                             &emissions,
                                             target_token_ids,
                                             token_count,
                                             tokenizer.blank_id,
                                             &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_trellis_backtrack(&trellis,
                                         &emissions,
                                         target_token_ids,
                                         token_count,
                                         tokenizer.blank_id,
                                         &path,
                                         &align_result)) {
        ok = false;
    }

    frame_duration_seconds = (float)(input.stride_ms/1000.0);
    if (ok && !lrc_ctc_path_to_padded_token_spans(&path,
                                                  &emissions,
                                                  target_token_ids,
                                                  token_count,
                                                  frame_duration_seconds,
                                                  &token_spans,
                                                  &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_token_spans_to_word_spans(&token_spans,
                                                 &tokens,
                                                 &normalized,
                                                 &word_spans,
                                                 &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                                     &normalized,
                                                     &line_timestamps,
                                                     &align_result)) {
        ok = false;
    }

    if (ok && (line_timestamps.line_count > INT32_MAX)) {
        ok = false;
    }
    if (ok) {
        output_lines = malloc2(
            line_timestamps.line_count*SIZEOF(*output_lines)
        );
        if (!ctc_align_output_lines_from_timestamps(&lyrics,
                                                    &line_timestamps,
                                                    output_lines)) {
            ok = false;
        }
    }
    if (ok && !lrc_write_output_file(lrc_path,
                                     output_lines,
                                     (int32)line_timestamps.line_count,
                                     &write_result)) {
        ok = false;
    }
    if (ok) {
        if ((written_lrc_len = read_entire_file(lrc_path, &written_lrc)) < 0) {
            ok = false;
        } else if (!STREQUAL(written_lrc, written_lrc_len, expected_lrc)) {
            ok = false;
        }
    }

    free2(written_lrc, ((int32)written_lrc_len + 1)*SIZEOF(*written_lrc));
    free2(output_lines,
          line_timestamps.line_count*SIZEOF(*output_lines));
    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);
    lrc_ctc_emissions_destroy(&emissions);
    free2(values, value_count*SIZEOF(*values));
    free2(target_token_ids, token_count*SIZEOF(*target_token_ids));
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);
    lrc_ctc_model_input_destroy(&input);
    lrc_ctc_audio_destroy(&audio);
    test_remove_tree(temp_dir);

    if (!ok) {
        fatal(ctc_align_test_fail("full synthetic lrc pipeline"));
    }

    return;
}

static void
ctc_align_test_maxwell_fixture_lrc_pipeline(void) {
    LrcCtcAudioConfig audio_config;
    LrcCtcAudioResult audio_result;
    LrcCtcAudio audio = {0};
    LrcCtcModelConfig model_config;
    LrcCtcModelInputResult model_result;
    LrcCtcModelInput input = {0};
    LrcLyrics lyrics = {0};
    LrcLyricsLoadResult lyrics_result;
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcTokenizeResult tokenize_result;
    LrcCtcFakeInference fake = {0};
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult inference_result;
    LrcCtcEmissions emissions = {0};
    LrcCtcAlignResult align_result;
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    LrcParsedFile expected_lrc = {0};
    LrcParsedFile actual_lrc = {0};
    LrcOutputLine *output_lines;
    LrcWriteResult write_result;
    char *lyrics_path;
    char *vocals_path;
    char *expected_lrc_path;
    char *expected_lrc_text;
    char *actual_lrc_text;
    int32 expected_lrc_text_len;
    int32 actual_lrc_text_len;
    char temp_dir[PATH_MAX];
    char output_lrc_path[PATH_MAX];
    int32 *target_token_ids;
    int32 *token_frames;
    float *values;
    float frame_duration_seconds;
    int32 token_count;
    int32 frame_count;
    int32 vocabulary_size;
    int64 value_count;
    bool ok;

    lyrics_path = getenv("LRC_TEST_MAXWELL_TXT");
    if (lyrics_path == NULL) {
        lyrics_path = "next-phase/maxwell.txt";
    }
    vocals_path = getenv("LRC_TEST_MAXWELL_VOCALS");
    if (vocals_path == NULL) {
        vocals_path = "next-phase/maxwell_vocals.opus";
    }
    expected_lrc_path = getenv("LRC_TEST_MAXWELL_LRC");
    if (expected_lrc_path == NULL) {
        expected_lrc_path = "next-phase/maxwell.lrc";
    }
    if (!test_command_exists("ffmpeg") || !util_file_exists(lyrics_path)
        || !util_file_exists(vocals_path)
        || !util_file_exists(expected_lrc_path)) {
        return;
    }

    lrc_ctc_tokenizer_init(&tokenizer);

    output_lines = NULL;
    expected_lrc_text = NULL;
    actual_lrc_text = NULL;
    expected_lrc_text_len = 0;
    actual_lrc_text_len = 0;
    target_token_ids = NULL;
    token_frames = NULL;
    values = NULL;
    frame_count = 0;
    vocabulary_size = 0;
    value_count = 0;
    ok = true;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "ctc_maxwell_lrc");
    test_join_path(output_lrc_path,
                        SIZEOF(output_lrc_path),
                        temp_dir,
                        "generated.lrc");

    if (!ctc_align_parse_lrc_file(&expected_lrc,
                                  expected_lrc_path,
                                  &expected_lrc_text,
                                  &expected_lrc_text_len)) {
        ok = false;
    }

    lrc_ctc_audio_config_init(&audio_config);
    audio_config.sample_rate = LRC_CTC_AUDIO_DEFAULT_SAMPLE_RATE;
    if (ok && !lrc_ctc_audio_decode_file(&audio,
                                         vocals_path,
                                         &audio_config,
                                         &audio_result)) {
        ok = false;
    }
    if (ok && ((audio.sample_rate != LRC_CTC_AUDIO_DEFAULT_SAMPLE_RATE)
               || (audio.channel_count != 1)
               || (audio.sample_count <= 0))) {
        ok = false;
    }

    lrc_ctc_model_config_init(&model_config);
    if (ok && !lrc_ctc_model_input_prepare(&input,
                                           &audio,
                                           &model_config,
                                           &model_result)) {
        ok = false;
    }
    frame_duration_seconds = (float)(input.stride_ms/1000.0);

    if (ok && !lrc_lyrics_load_file(&lyrics, lyrics_path, &lyrics_result)) {
        ok = false;
    }
    if (ok && !ctc_align_normalize_current_lyrics(&lyrics, &normalized)) {
        ok = false;
    }
    if (ok && !ctc_align_load_alphabet_tokenizer(&tokenizer)) {
        ok = false;
    }
    if (ok && !lrc_ctc_tokenizer_tokenize_normalized(&tokenizer,
                                                     &normalized,
                                                     &tokens,
                                                     &tokenize_result)) {
        ok = false;
    }

    token_count = tokens.token_count;
    if (ok && (token_count <= 0)) {
        ok = false;
    }
    if (ok) {
        target_token_ids = malloc2(token_count*SIZEOF(*target_token_ids));
        token_frames = malloc2(token_count*SIZEOF(*token_frames));
        for (int32 i = 0; i < token_count; i += 1) {
            target_token_ids[i] = tokens.tokens[i].token_id;
        }
        if (!ctc_align_make_line_timed_token_frames(&expected_lrc,
                                                    &tokens,
                                                    frame_duration_seconds,
                                                    token_frames,
                                                    &frame_count)) {
            ok = false;
        }
    }
    if (ok && ((frame_count <= 0)
               || ((double)frame_count*(double)frame_duration_seconds
                   > audio.duration_seconds + 0.5))) {
        ok = false;
    }

    vocabulary_size = tokenizer.token_count;
    if (ok && ((int64)frame_count > INT64_MAX/vocabulary_size)) {
        ok = false;
    }
    if (ok) {
        value_count = (int64)frame_count*(int64)vocabulary_size;
        values = malloc2(value_count*SIZEOF(*values));
        ctc_align_fill_token_frame_values(values,
                                          frame_count,
                                          vocabulary_size,
                                          tokenizer.blank_id,
                                          target_token_ids,
                                          token_frames,
                                          token_count);
    }

    if (ok && !lrc_ctc_fake_inference_set(&fake,
                                          values,
                                          frame_count,
                                          vocabulary_size)) {
        ok = false;
    }
    if (ok) {
        lrc_ctc_fake_inference_backend(&fake, &backend);
        if (!lrc_ctc_inference_run(&backend,
                                   &input,
                                   &emissions,
                                   &inference_result)) {
            ok = false;
        }
    }
    if (ok && !lrc_ctc_emissions_convert_to_log_probabilities(
        &emissions,
        LRC_CTC_EMISSION_VALUES_LOG_PROBABILITIES,
        &inference_result
    )) {
        ok = false;
    }

    if (ok && !lrc_ctc_trellis_score_forward(&trellis,
                                             &emissions,
                                             target_token_ids,
                                             token_count,
                                             tokenizer.blank_id,
                                             &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_trellis_backtrack(&trellis,
                                         &emissions,
                                         target_token_ids,
                                         token_count,
                                         tokenizer.blank_id,
                                         &path,
                                         &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_path_to_token_spans(&path,
                                           &emissions,
                                           frame_duration_seconds,
                                           &token_spans,
                                           &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_token_spans_to_word_spans(&token_spans,
                                                 &tokens,
                                                 &normalized,
                                                 &word_spans,
                                                 &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                                     &normalized,
                                                     &line_timestamps,
                                                     &align_result)) {
        ok = false;
    }

    if (ok && (line_timestamps.line_count > INT32_MAX)) {
        ok = false;
    }
    if (ok) {
        output_lines = malloc2(
            line_timestamps.line_count*SIZEOF(*output_lines)
        );
        if (!ctc_align_output_lines_from_timestamps(&lyrics,
                                                    &line_timestamps,
                                                    output_lines)) {
            ok = false;
        }
    }
    if (ok && !lrc_write_output_file(output_lrc_path,
                                     output_lines,
                                     (int32)line_timestamps.line_count,
                                     &write_result)) {
        ok = false;
    }
    if (ok && !ctc_align_parse_lrc_file(&actual_lrc,
                                        output_lrc_path,
                                        &actual_lrc_text,
                                        &actual_lrc_text_len)) {
        ok = false;
    }
    if (ok && !ctc_align_parsed_files_close(&actual_lrc,
                                            &expected_lrc,
                                            0.015f)) {
        ok = false;
    }

    free2(actual_lrc_text,
          ((int32)actual_lrc_text_len + 1)*SIZEOF(*actual_lrc_text));
    free2(expected_lrc_text,
          ((int32)expected_lrc_text_len + 1)*SIZEOF(*expected_lrc_text));
    free2(output_lines,
          line_timestamps.line_count*SIZEOF(*output_lines));
    free2(values, value_count*SIZEOF(*values));
    free2(token_frames, token_count*SIZEOF(*token_frames));
    free2(target_token_ids, token_count*SIZEOF(*target_token_ids));
    lrc_parsed_file_destroy(&actual_lrc);
    lrc_parsed_file_destroy(&expected_lrc);
    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);
    lrc_ctc_emissions_destroy(&emissions);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);
    lrc_ctc_model_input_destroy(&input);
    lrc_ctc_audio_destroy(&audio);
    test_remove_tree(temp_dir);

    if (!ok) {
        fatal(ctc_align_test_fail("maxwell fixture lrc pipeline"));
    }

    return;
}


static void
ctc_align_set_rank3_row_preference(
    float *values,
    int64 row_index,
    int32 vocabulary_size,
    int32 token_id
) {
    for (int32 i = 0; i < vocabulary_size; i += 1) {
        values[row_index*vocabulary_size + i] = -10.0f;
    }
    values[row_index*vocabulary_size + token_id] = 10.0f;

    return;
}

static void
ctc_align_test_rank3_trimmed_fake_inference_pipeline(void) {
    LrcCtcAudio audio = {0};
    LrcCtcModelConfig model_config;
    LrcCtcModelInputResult model_result;
    LrcCtcModelInput input = {0};
    LrcLyrics lyrics = {0};
    LrcLyricsNormalized normalized = {0};
    LrcCtcTokenizer tokenizer;
    LrcCtcTokenizedText tokens = {0};
    LrcCtcFakeInference fake = {0};
    LrcCtcInferenceBackend backend;
    LrcCtcInferenceResult inference_result;
    LrcCtcEmissions emissions = {0};
    LrcCtcAlignResult align_result;
    LrcCtcTrellis trellis = {0};
    LrcCtcPath path = {0};
    LrcCtcTokenSpans token_spans = {0};
    LrcCtcWordSpans word_spans = {0};
    LrcCtcLineTimestamps line_timestamps = {0};
    int64 shape[3];
    int32 target_token_ids[2];
    int32 star_token_id;
    int64 raw_value_count;
    int32 vocabulary_size;
    float *values;
    float samples[12];
    bool ok;

    for (int32 i = 0; i < LENGTH(samples); i += 1) {
        samples[i] = (float)i;
    }

    lrc_ctc_tokenizer_init(&tokenizer);

    audio.samples = samples;
    audio.sample_count = LENGTH(samples);
    audio.sample_rate = 4;
    audio.channel_count = 1;
    audio.duration_seconds = 3.0;

    values = NULL;
    ok = true;

    lrc_ctc_model_config_init(&model_config);
    model_config.sample_rate = 4;
    model_config.inputs_to_logits_ratio = 1;
    model_config.window_seconds = 2;
    model_config.context_seconds = 1;
    if (!lrc_ctc_model_input_prepare(&input,
                                     &audio,
                                     &model_config,
                                     &model_result)) {
        ok = false;
    }
    if (ok && ((input.chunk_count != 2)
               || (input.raw_chunk_emission_count != 16)
               || (input.original_emission_count != 12))) {
        ok = false;
    }
    if (ok && !ctc_align_load_tokenized_lyrics(STRLIT("ab\n"),
                                               &lyrics,
                                               &normalized,
                                               &tokenizer,
                                               &tokens)) {
        ok = false;
    }
    if (ok && (tokens.token_count != 2)) {
        ok = false;
    }

    vocabulary_size = tokenizer.token_count;
    raw_value_count = input.chunk_count*input.raw_chunk_emission_count
                      *(int64)vocabulary_size;
    if (ok) {
        values = malloc2(raw_value_count*SIZEOF(*values));
        for (int32 i = 0; i < input.chunk_count*input.raw_chunk_emission_count;
             i += 1) {
            ctc_align_set_rank3_row_preference(values,
                                               i,
                                               vocabulary_size,
                                               tokens.tokens[0].token_id);
        }
        target_token_ids[0] = tokens.tokens[0].token_id;
        target_token_ids[1] = tokens.tokens[1].token_id;
    }

    if (ok) {
        int32 output_frame = 0;

        for (int32 i = 0; i < input.chunk_count; i += 1) {
            LrcCtcModelChunk *chunk = &input.chunks[i];
            int64 kept_offset;

            kept_offset = chunk->kept_emission_start
                          - chunk->raw_emission_start;
            for (int32 j = 0; j < chunk->kept_emission_count; j += 1) {
                int64 raw_frame;
                int32 preferred_token;

                if (output_frame >= input.original_emission_count) {
                    break;
                }
                raw_frame = i*input.raw_chunk_emission_count + kept_offset + j;
                preferred_token = tokenizer.blank_id;
                if (output_frame == 1) {
                    preferred_token = target_token_ids[0];
                }
                if (output_frame == 3) {
                    preferred_token = target_token_ids[1];
                }
                ctc_align_set_rank3_row_preference(values,
                                                   raw_frame,
                                                   vocabulary_size,
                                                   preferred_token);
                output_frame += 1;
            }
        }
    }

    shape[0] = input.chunk_count;
    shape[1] = input.raw_chunk_emission_count;
    shape[2] = vocabulary_size;
    if (ok && !lrc_ctc_fake_inference_set_shape(&fake,
                                                values,
                                                raw_value_count,
                                                shape,
                                                3)) {
        ok = false;
    }
    if (ok) {
        lrc_ctc_fake_inference_backend(&fake, &backend);
        backend.values_kind = LRC_CTC_EMISSION_VALUES_LOGITS;
        if (!lrc_ctc_inference_run(&backend,
                                   &input,
                                   &emissions,
                                   &inference_result)) {
            ok = false;
        }
    }
    if (ok && (emissions.frame_count != input.original_emission_count)) {
        ok = false;
    }

    star_token_id = (int32)emissions.vocabulary_size;
    if (ok && !lrc_ctc_trellis_score_forward_with_edge_stars(
        &trellis,
        &emissions,
        target_token_ids,
        LENGTH(target_token_ids),
        tokenizer.blank_id,
        star_token_id,
        &align_result
    )) {
        ok = false;
    }
    if (ok && !lrc_ctc_trellis_backtrack_with_edge_stars(&trellis,
                                                          &emissions,
                                                          target_token_ids,
                                                          LENGTH(
                                                              target_token_ids
                                                          ),
                                                          tokenizer.blank_id,
                                                          star_token_id,
                                                          &path,
                                                          &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_path_to_token_spans(&path,
                                           &emissions,
                                           0.25f,
                                           &token_spans,
                                           &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_token_spans_to_word_spans(&token_spans,
                                                 &tokens,
                                                 &normalized,
                                                 &word_spans,
                                                 &align_result)) {
        ok = false;
    }
    if (ok && !lrc_ctc_word_spans_to_line_timestamps(&word_spans,
                                                     &normalized,
                                                     &line_timestamps,
                                                     &align_result)) {
        ok = false;
    }
    if (ok) {
        ASSERT(token_spans.span_count == 2);
        ASSERT(token_spans.spans[0].start_frame == 1);
        ASSERT(token_spans.spans[1].start_frame == 3);
        ASSERT(line_timestamps.line_count == 1);
        ASSERT(line_timestamps.lines[0].kind
               == LRC_CTC_LINE_TIMESTAMP_KIND_TIMESTAMPED);
        ASSERT(ctc_align_float_close(line_timestamps.lines[0].start_seconds,
                                     0.25f,
                                     0.00001f));
    }

    lrc_ctc_line_timestamps_destroy(&line_timestamps);
    lrc_ctc_word_spans_destroy(&word_spans);
    lrc_ctc_token_spans_destroy(&token_spans);
    lrc_ctc_path_destroy(&path);
    lrc_ctc_trellis_destroy(&trellis);
    lrc_ctc_emissions_destroy(&emissions);
    lrc_ctc_tokenized_text_destroy(&tokens);
    lrc_ctc_tokenizer_destroy(&tokenizer);
    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);
    lrc_ctc_model_input_destroy(&input);
    free2(values, raw_value_count*SIZEOF(*values));

    if (!ok) {
        fatal(ctc_align_test_fail("rank-3 trimmed fake inference pipeline"));
    }

    return;
}

static void
ctc_align_test_prepare_rejects_invalid_emissions(void) {
    LrcCtcAlignResult result;
    LrcCtcTrellis trellis = {0};
    LrcCtcEmissions emissions;
    float values[] = {-0.1f, -0.2f};

    if (lrc_ctc_trellis_prepare(&trellis, NULL, 1, 0, &result)) {
        fatal(ctc_align_test_fail("missing emissions accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_ARGUMENT);

    ctc_align_make_emissions(&emissions, values, 1, 2);
    emissions.value_count = 1;
    if (lrc_ctc_trellis_prepare(&trellis, &emissions, 1, 0, &result)) {
        fatal(ctc_align_test_fail("bad emissions value count accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_EMISSIONS);

    ctc_align_make_emissions(&emissions, values, 1, 2);
    if (lrc_ctc_trellis_prepare(&trellis, &emissions, 1, 2, &result)) {
        fatal(ctc_align_test_fail("bad blank token accepted"));
    }
    ASSERT(result.header.error == LS_ERROR_CTC_ALIGN_INVALID_BLANK_TOKEN);
    ASSERT(result.token_index == 2);

    ASSERT(trellis.scores == NULL);

    return;
}

int32
main(void) {
    ctc_align_test_empty_initializers();
    ctc_align_test_graph_build_layout();
    ctc_align_test_graph_build_edge_stars();
    ctc_align_test_graph_build_segment_stars();
    ctc_align_test_graph_rejects_bad_inputs();
    ctc_align_test_graph_transition_rules();
    ctc_align_test_required_frame_count_for_tokens();
    ctc_align_test_score_rejects_too_few_repeated_frames();
    ctc_align_test_allocate_initializes_to_negative_infinity();
    ctc_align_test_rejects_invalid_dimensions();
    ctc_align_test_prepare_initializes_start_state();
    ctc_align_test_prepare_rejects_invalid_emissions();
    ctc_align_test_trellis_uses_graph_states();
    ctc_align_test_forward_scores_ctc_skip_transition();
    ctc_align_test_best_final_state_selection();
    ctc_align_test_forward_scores_simple_path();
    ctc_align_test_forward_prefers_blank_stay();
    ctc_align_test_forward_rejects_bad_targets();
    ctc_align_test_backtracks_simple_path();
    ctc_align_test_backtracks_repeated_tokens();
    ctc_align_test_backtracks_edge_stars();
    ctc_align_test_backtracks_segment_stars();
    ctc_align_test_edge_stars_reject_bad_star_token();
    ctc_align_test_backtrack_rejects_impossible_alignment();
    ctc_align_test_backtrack_rejects_invalid_trellis();
    ctc_align_test_path_segments_merge_blanks_and_tokens();
    ctc_align_test_path_segments_split_repeated_token_after_blank();
    ctc_align_test_path_segments_keep_stars();
    ctc_align_test_aligned_intervals_keep_edge_star_order();
    ctc_align_test_aligned_intervals_keep_segment_star_order();
    ctc_align_test_pad_intervals_distributes_blank_frames();
    ctc_align_test_pad_intervals_counts_initial_star();
    ctc_align_test_token_spans_from_backtracked_path();
    ctc_align_test_token_spans_preserve_repeated_tokens();
    ctc_align_test_token_spans_collapse_contiguous_steps();
    ctc_align_test_token_spans_reject_bad_inputs();
    ctc_align_test_token_spans_reject_out_of_order_targets();
    ctc_align_test_padded_token_spans_use_blank_boundaries();
    ctc_align_test_synthetic_lrc_uses_active_token_boundaries();
    ctc_align_test_word_spans_use_active_token_boundaries();
    ctc_align_test_segment_word_spans_use_active_token_boundaries();
    ctc_align_test_word_spans_group_generated_words();
    ctc_align_test_word_spans_use_skipped_space_gaps();
    ctc_align_test_word_spans_handle_removed_punctuation();
    ctc_align_test_word_spans_follow_reference_segments();
    ctc_align_test_word_spans_keep_repeated_token_positions();
    ctc_align_test_word_spans_reject_bad_inputs();
    ctc_align_test_line_timestamps_from_generated_words();
    ctc_align_test_line_timestamps_reject_bad_inputs();
    ctc_align_test_line_timestamps_repeated_boundary_alignment();
    ctc_align_test_maxwell_line_timestamp_comparison();
    ctc_align_test_full_synthetic_alignment_pipeline();
    ctc_align_test_full_synthetic_lrc_pipeline();
    ctc_align_test_rank3_trimmed_fake_inference_pipeline();
    ctc_align_test_maxwell_fixture_lrc_pipeline();
    ctc_align_test_maxwell_fake_token_timing();
    ctc_align_test_maxwell_word_line_mapping();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_ctc_align */
