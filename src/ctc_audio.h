#if !defined(CTC_AUDIO_H)
#define CTC_AUDIO_H

#include "cbase.h"
#include "errors.h"
#include "audio.h"

#define LRC_CTC_AUDIO_DEFAULT_SAMPLE_RATE 16000

typedef struct LrcCtcAudioConfig {
    char *ffmpeg_path;

    int32 sample_rate;
} LrcCtcAudioConfig;

typedef struct LrcCtcAudioResult {
    LrcPathResultHeader path_header;

    int64 sample_index;
} LrcCtcAudioResult;

typedef struct LrcCtcAudio {
    float *samples;

    int64 sample_count;
    int32 sample_rate;
    int32 channel_count;
    double duration_seconds;
    float max_abs_sample;
} LrcCtcAudio;

static void lrc_ctc_audio_config_init(LrcCtcAudioConfig *config);
static void lrc_ctc_audio_result_init(LrcCtcAudioResult *result);
static void lrc_ctc_audio_destroy(LrcCtcAudio *audio);
static bool lrc_ctc_audio_decode_file(LrcCtcAudio *audio, char *path,
                                      LrcCtcAudioConfig *config,
                                      LrcCtcAudioResult *result);

#endif /* CTC_AUDIO_H */
