#if !defined(PROGRESS_H)
#define PROGRESS_H

#include "cbase.h"

typedef struct LrcProgress {
    char *label;

    int64 current;
    int64 total;
    int32 width;
    int32 last_percent;
    int64 last_current;

    bool enabled;
    bool rendered;
    bool finished;
} LrcProgress;

static void lrc_progress_init(LrcProgress *progress, bool enabled, char *label,
                              int64 total);
static void lrc_progress_begin(LrcProgress *progress);
static void lrc_progress_update(LrcProgress *progress, int64 current);
static void lrc_progress_finish(LrcProgress *progress);
static void lrc_progress_cancel(LrcProgress *progress);
static void lrc_progress_end_line(void);

#endif /* PROGRESS_H */
