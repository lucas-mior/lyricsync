#if !defined(LRC_PROGRESS_IMPLEMENTED)
#define LRC_PROGRESS_IMPLEMENTED

#include "cbase.h"
#include "lyricsync.h"
#include "progress.h"

#if !defined(TESTING_progress)
#define TESTING_progress 0
#endif

#define LRC_PROGRESS_LABEL_WIDTH 28
#define LRC_PROGRESS_MAX_LINE_WIDTH 80
#define LRC_PROGRESS_MIN_BAR_WIDTH 4

static bool lrc_progress_line_open;

static int32
lrc_progress_percent(int64 current, int64 total) {
    double ratio;

    if (total <= 0) {
        return 100;
    }

    ratio = (double)current/(double)total;
    ratio = CLAMP(ratio, 0.0, 1.0);

    return (int32)(ratio*100.0 + 0.5);
}

static int32
lrc_progress_decimal_digits(int64 value) {
    int32 digits = 1;

    if (value < 0) {
        digits += 1;
        value = -value;
    }

    while (value >= 10) {
        digits += 1;
        value /= 10;
    }

    return digits;
}

static bool
lrc_progress_stderr_is_terminal(void) {
    return isatty(STDERR_FILENO);
}

static int32
lrc_progress_terminal_columns(void) {
    int32 columns = LRC_PROGRESS_MAX_LINE_WIDTH;

#if OS_UNIX && defined(TIOCGWINSZ)
    struct winsize size;

    if (lrc_progress_stderr_is_terminal()
        && (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0)
        && (size.ws_col > 0)) {
        columns = (int32)size.ws_col;
    }
#endif

    if (columns > LRC_PROGRESS_MAX_LINE_WIDTH) {
        columns = LRC_PROGRESS_MAX_LINE_WIDTH;
    }

    return columns;
}

static int32
lrc_progress_bar_width(LrcProgress *progress) {
    int32 columns;
    int32 total_digits;
    int32 overhead;
    int32 width;

    columns = lrc_progress_terminal_columns();
    total_digits = lrc_progress_decimal_digits(progress->total);
    overhead = LRC_PROGRESS_LABEL_WIDTH + 10 + 2*total_digits;
    width = columns - overhead;
    if (width < LRC_PROGRESS_MIN_BAR_WIDTH) {
        width = LRC_PROGRESS_MIN_BAR_WIDTH;
    }

    return width;
}

static int32
lrc_progress_filled_width(LrcProgress *progress) {
    double ratio;

    if (progress->total <= 0) {
        return progress->width;
    }

    ratio = (double)progress->current/(double)progress->total;
    ratio = CLAMP(ratio, 0.0, 1.0);

    return (int32)(ratio*(double)progress->width + 0.5);
}

static void
lrc_progress_write_bar(LrcProgress *progress, int32 percent, int32 filled) {
    int32 total_digits;
    char *label;

    total_digits = lrc_progress_decimal_digits(progress->total);
    label = progress->label;
    if (label == NULL) {
        label = "progress";
    }

    fprintf(stderr, "%-*.*s [",
            LRC_PROGRESS_LABEL_WIDTH,
            LRC_PROGRESS_LABEL_WIDTH,
            label);
    for (int32 i = 0; i < progress->width; i += 1) {
        if (i < filled) {
            fputc('#', stderr);
        } else {
            fputc('.', stderr);
        }
    }
    fprintf(stderr,
            "] %3d%% %*lld/%lld",
            percent,
            total_digits,
            progress->current,
            progress->total);

    return;
}

static void
lrc_progress_render(LrcProgress *progress, bool end_line) {
    int32 filled;
    int32 percent;
    bool changed;
    bool terminal;

    if ((progress == NULL) || !progress->enabled) {
        return;
    }

    progress->current = CLAMP(progress->current, 0, progress->total);
    percent = lrc_progress_percent(progress->current, progress->total);
    changed = (percent != progress->last_percent)
              || (progress->current != progress->last_current);
    if (!changed) {
        if (end_line) {
            lrc_progress_end_line();
        }
        return;
    }

    progress->width = lrc_progress_bar_width(progress);
    filled = lrc_progress_filled_width(progress);
    terminal = lrc_progress_stderr_is_terminal();

    if (terminal) {
        fputc('\r', stderr);
        fputs("\033[2K", stderr);
    }
    lrc_progress_write_bar(progress, percent, filled);
    if (terminal && !end_line) {
        lrc_progress_line_open = true;
    } else {
        fputc('\n', stderr);
        lrc_progress_line_open = false;
    }
    fflush(stderr);

    progress->rendered = true;
    progress->last_percent = percent;
    progress->last_current = progress->current;

    return;
}

static void
lrc_progress_init(LrcProgress *progress, bool enabled, char *label,
                  int64 total) {
    if (progress == NULL) {
        return;
    }

    progress->label = label;
    progress->current = 0;
    progress->total = total;
    if (progress->total <= 0) {
        progress->total = 1;
    }
    progress->width = LRC_PROGRESS_MIN_BAR_WIDTH;
    progress->last_percent = -1;
    progress->last_current = -1;
    progress->enabled = enabled;
    progress->rendered = false;
    progress->finished = false;

    return;
}

static void
lrc_progress_begin(LrcProgress *progress) {
    if (progress == NULL) {
        return;
    }

    progress->current = 0;
    lrc_progress_render(progress, false);

    return;
}

static void
lrc_progress_update(LrcProgress *progress, int64 current) {
    if (progress == NULL) {
        return;
    }
    if (progress->finished) {
        return;
    }

    progress->current = current;
    lrc_progress_render(progress, false);

    return;
}

static void
lrc_progress_finish(LrcProgress *progress) {
    if (progress == NULL) {
        return;
    }
    if (progress->finished) {
        return;
    }

    progress->current = progress->total;
    lrc_progress_render(progress, true);
    progress->finished = true;

    return;
}

static void
lrc_progress_cancel(LrcProgress *progress) {
    if (progress == NULL) {
        return;
    }
    if (!progress->enabled || !progress->rendered || progress->finished) {
        return;
    }

    lrc_progress_end_line();
    progress->finished = true;

    return;
}

static void
lrc_progress_end_line(void) {
    if (!lrc_progress_line_open) {
        return;
    }

    fputc('\n', stderr);
    fflush(stderr);
    lrc_progress_line_open = false;

    return;
}

#if TESTING_progress
#define CBASE_IMPLEMENT
#include "cbase.h"

int32
main(void) {
    LrcProgress progress;

    lrc_progress_init(&progress, false, "disabled", 3);
    lrc_progress_begin(&progress);
    lrc_progress_update(&progress, 1);
    lrc_progress_finish(&progress);

    lrc_progress_init(&progress, true, "progress test", 2);
    lrc_progress_begin(&progress);
    lrc_progress_update(&progress, 1);
    lrc_progress_finish(&progress);

    exit(EXIT_SUCCESS);
}
#endif

#endif /* LRC_PROGRESS_IMPLEMENTED */
