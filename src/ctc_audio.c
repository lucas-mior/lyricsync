#include "cbase.h"
#include "lyricsync.h"
#include "ctc_audio.h"

#if !defined(TESTING_ctc_audio)
#define TESTING_ctc_audio 0
#endif

static void
lrc_ctc_audio_config_init(LrcCtcAudioConfig *config) {
    if (config == NULL) {
        return;
    }

    config->ffmpeg_path = "ffmpeg";
    config->sample_rate = LRC_CTC_AUDIO_DEFAULT_SAMPLE_RATE;

    return;
}

static void
lrc_ctc_audio_result_init(LrcCtcAudioResult *result) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_init(&result->path_header);

    result->sample_index = -1;

    return;
}

static void
lrc_ctc_audio_result_set(
    LrcCtcAudioResult *result,
    enum LsError error,
    char *message,
    char *path,
    int64 sample_index
) {
    if (result == NULL) {
        return;
    }

    lrc_path_result_header_set(&result->path_header, error, message, path);

    result->sample_index = sample_index;

    return;
}


static void
lrc_ctc_audio_destroy(LrcCtcAudio *audio) {
    if (audio == NULL) {
        return;
    }

    free2(audio->samples, audio->sample_count*SIZEOF(*audio->samples));

    memset64(audio, 0, SIZEOF(*audio));

    return;
}

static float
lrc_ctc_audio_abs_sample(float sample) {
    if (sample < 0.0f) {
        return -sample;
    }

    return sample;
}

static bool
lrc_ctc_audio_validate_samples(
    LrcCtcAudio *audio,
    LrcCtcAudioResult *result,
    char *path
) {
    audio->max_abs_sample = 0.0f;
    for (int64 i = 0; i < audio->sample_count; i += 1) {
        float sample = audio->samples[i];
        float abs_sample;

        if (!isfinite((double)sample)) {
            lrc_ctc_audio_result_set(
                result,
                LS_ERROR_CTC_AUDIO_NON_FINITE_SAMPLE,
                "decoded CTC audio contains a non-finite sample",
                path,
                i
            );
            return false;
        }

        abs_sample = lrc_ctc_audio_abs_sample(sample);
        if (abs_sample > audio->max_abs_sample) {
            audio->max_abs_sample = abs_sample;
        }
    }

    return true;
}

static bool
lrc_ctc_audio_decode_file(
    LrcCtcAudio *audio,
    char *path,
    LrcCtcAudioConfig *config,
    LrcCtcAudioResult *result
) {
    AudioBuffer decoded;
    AudioIoFormat format;
    LrcCtcAudioConfig default_config;

    if (result) {
        lrc_ctc_audio_result_init(result);
    }
    if (audio == NULL) {
        lrc_ctc_audio_result_set(
            result,
            LS_ERROR_CTC_AUDIO_INVALID_ARGUMENT,
            "CTC audio decode received invalid arguments",
            path,
            -1
        );
        return false;
    }

    lrc_ctc_audio_destroy(audio);
    if (config == NULL) {
        lrc_ctc_audio_config_init(&default_config);
        config = &default_config;
    }

    if (path_missing(path)) {
        lrc_ctc_audio_result_set(
            result,
            LS_ERROR_CTC_AUDIO_MISSING_PATH,
            "CTC audio input path is missing",
            path,
            -1
        );
        return false;
    }
    if (path_missing(config->ffmpeg_path)) {
        lrc_ctc_audio_result_set(
            result,
            LS_ERROR_CTC_AUDIO_MISSING_FFMPEG,
            "ffmpeg path is missing",
            config->ffmpeg_path,
            -1
        );
        return false;
    }
    if (config->sample_rate <= 0) {
        lrc_ctc_audio_result_set(
            result,
            LS_ERROR_CTC_AUDIO_INVALID_SAMPLE_RATE,
            "CTC audio sample rate is invalid",
            path,
            -1
        );
        return false;
    }

    format.sample_rate = config->sample_rate;
    format.channel_count = 1;

    audio_buffer_init(&decoded);
    if (!audio_read_file_format(&decoded, path, &format, config->ffmpeg_path)) {
        audio_buffer_destroy(&decoded);
        lrc_ctc_audio_result_set(
            result,
            LS_ERROR_CTC_AUDIO_DECODE_FAILED,
            "could not decode CTC audio input",
            path,
            -1
        );
        return false;
    }
    if (decoded.frame_count <= 0) {
        audio_buffer_destroy(&decoded);
        lrc_ctc_audio_result_set(
            result,
            LS_ERROR_CTC_AUDIO_EMPTY_AUDIO,
            "decoded CTC audio is empty",
            path,
            -1
        );
        return false;
    }

    audio->samples = decoded.left;
    audio->sample_count = decoded.frame_count;
    audio->sample_rate = decoded.sample_rate;
    audio->channel_count = decoded.channel_count;
    audio->duration_seconds = (double)audio->sample_count
                              /(double)audio->sample_rate;

    decoded.left = NULL;
    audio_buffer_destroy(&decoded);

    if (!lrc_ctc_audio_validate_samples(audio, result, path)) {
        lrc_ctc_audio_destroy(audio);
        return false;
    }

    return true;
}

#if TESTING_ctc_audio
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "audio.c"

static int32
ctc_audio_test_fail(char *name) {
    error2("CTC audio test failed: %s\n", name);

    return 1;
}


static bool
ctc_audio_double_close(double a, double b, double max_error) {
    double diff;

    diff = fabs(a - b);

    return diff <= max_error;
}

static void
ctc_audio_test_defaults_and_invalid_inputs(void) {
    LrcCtcAudio audio = {0};
    LrcCtcAudioConfig config;
    LrcCtcAudioResult result;

    lrc_ctc_audio_config_init(&config);
    lrc_ctc_audio_result_init(&result);

    ASSERT(strequal(config.ffmpeg_path, "ffmpeg"));
    ASSERT(config.sample_rate == LRC_CTC_AUDIO_DEFAULT_SAMPLE_RATE);
    ASSERT(result.path_header.header.error == LS_ERROR_NONE);
    ASSERT(strequal(result.path_header.header.message, "ok"));
    ASSERT(result.path_header.path == NULL);
    ASSERT(result.sample_index == -1);
    ASSERT(audio.samples == NULL);
    ASSERT(audio.sample_count == 0);
    ASSERT(audio.sample_rate == 0);
    ASSERT(audio.channel_count == 0);

    if (lrc_ctc_audio_decode_file(NULL, "song.wav", &config, &result)) {
        fatal(ctc_audio_test_fail("invalid null audio accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_AUDIO_INVALID_ARGUMENT);

    if (lrc_ctc_audio_decode_file(&audio, NULL, &config, &result)) {
        fatal(ctc_audio_test_fail("missing path accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_AUDIO_MISSING_PATH);

    config.ffmpeg_path = "";
    if (lrc_ctc_audio_decode_file(&audio, "song.wav", &config, &result)) {
        fatal(ctc_audio_test_fail("missing ffmpeg accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_AUDIO_MISSING_FFMPEG);

    lrc_ctc_audio_config_init(&config);
    config.sample_rate = 0;
    if (lrc_ctc_audio_decode_file(&audio, "song.wav", &config, &result)) {
        fatal(ctc_audio_test_fail("invalid sample rate accepted"));
    }
    ASSERT(result.path_header.header.error == LS_ERROR_CTC_AUDIO_INVALID_SAMPLE_RATE);

    lrc_ctc_audio_destroy(&audio);

    return;
}

static void
ctc_audio_test_generated_decode(
    int32 source_sample_rate,
    int32 source_channel_count,
    double duration_seconds,
    int32 target_sample_rate,
    char *name
) {
    AudioFileInfo info;
    AudioTestSineOptions sine;
    LrcCtcAudio audio = {0};
    LrcCtcAudioConfig config;
    LrcCtcAudioResult result;
    char temp_dir[PATH_MAX];
    char wav_path[PATH_MAX];
    int64 expected_samples;
    int64 delta;

    if (!test_command_exists("ffmpeg") || !test_command_exists("ffprobe")) {
        return;
    }

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), name);
    test_join_path(wav_path, SIZEOF(wav_path), temp_dir, "input.wav");

    audio_test_sine_options_init(&sine);
    sine.format.sample_rate = source_sample_rate;
    sine.format.channel_count = source_channel_count;
    sine.duration_seconds = duration_seconds;
    sine.frequency_hz = 330.0;
    sine.amplitude = 0.45f;
    if (!audio_test_generate_sine_wav(wav_path, &sine, "ffmpeg")) {
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("generate sine fixture"));
    }

    if (!audio_file_info_read(&info, wav_path, "ffprobe")) {
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("probe generated fixture"));
    }
    if (info.sample_rate != source_sample_rate) {
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("source sample rate"));
    }
    if (info.channel_count != source_channel_count) {
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("source channel count"));
    }
    if (!ctc_audio_double_close(info.duration_seconds,
                                duration_seconds,
                                0.02)) {
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("source duration"));
    }

    lrc_ctc_audio_config_init(&config);
    config.sample_rate = target_sample_rate;
    if (!lrc_ctc_audio_decode_file(&audio, wav_path, &config, &result)) {
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("decode generated fixture"));
    }

    expected_samples = (int64)(duration_seconds*(double)target_sample_rate
                               + 0.5);
    delta = audio.sample_count - expected_samples;
    if (delta < 0) {
        delta = -delta;
    }
    if (delta > 2) {
        lrc_ctc_audio_destroy(&audio);
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("decoded sample count"));
    }
    if (audio.sample_rate != target_sample_rate) {
        lrc_ctc_audio_destroy(&audio);
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("decoded sample rate"));
    }
    if (audio.channel_count != 1) {
        lrc_ctc_audio_destroy(&audio);
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("decoded channel count"));
    }
    if (!ctc_audio_double_close(audio.duration_seconds,
                                duration_seconds,
                                0.01)) {
        lrc_ctc_audio_destroy(&audio);
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("decoded duration"));
    }
    if ((audio.max_abs_sample <= 0.0f) || (audio.max_abs_sample > 1.0f)) {
        lrc_ctc_audio_destroy(&audio);
        test_remove_tree(temp_dir);
        fatal(ctc_audio_test_fail("decoded float range"));
    }

    lrc_ctc_audio_destroy(&audio);
    test_remove_tree(temp_dir);

    return;
}

static char *
ctc_audio_maxwell_vocals_path(void) {
    char *path;

    path = getenv("LRC_TEST_MAXWELL_VOCALS");
    if (!path_missing(path)) {
        return path;
    }
    if (util_file_exists("next-phase/maxwell_vocals.opus")) {
        return "next-phase/maxwell_vocals.opus";
    }
    if (util_file_exists("/mnt/data/maxwell_vocals.opus")) {
        return "/mnt/data/maxwell_vocals.opus";
    }

    return NULL;
}

static void
ctc_audio_test_maxwell_vocals(void) {
    AudioFileInfo info;
    LrcCtcAudio audio = {0};
    LrcCtcAudioConfig config;
    LrcCtcAudioResult result;
    char *path;

    if (!test_command_exists("ffmpeg") || !test_command_exists("ffprobe")) {
        return;
    }

    path = ctc_audio_maxwell_vocals_path();
    if (path == NULL) {
        return;
    }

    if (!audio_file_info_read(&info, path, "ffprobe")) {
        fatal(ctc_audio_test_fail("probe Maxwell vocals"));
    }
    if (info.sample_rate != 48000) {
        fatal(ctc_audio_test_fail("Maxwell source sample rate"));
    }
    if (info.channel_count != 2) {
        fatal(ctc_audio_test_fail("Maxwell source channels"));
    }
    if (!ctc_audio_double_close(info.duration_seconds, 21.37, 0.10)) {
        fatal(ctc_audio_test_fail("Maxwell source duration"));
    }

    lrc_ctc_audio_config_init(&config);
    config.sample_rate = 16000;
    if (!lrc_ctc_audio_decode_file(&audio, path, &config, &result)) {
        fatal(ctc_audio_test_fail("decode Maxwell vocals"));
    }
    if (audio.sample_rate != 16000) {
        lrc_ctc_audio_destroy(&audio);
        fatal(ctc_audio_test_fail("Maxwell decoded sample rate"));
    }
    if (audio.channel_count != 1) {
        lrc_ctc_audio_destroy(&audio);
        fatal(ctc_audio_test_fail("Maxwell decoded channels"));
    }
    if ((audio.sample_count < 340000) || (audio.sample_count > 343000)) {
        lrc_ctc_audio_destroy(&audio);
        fatal(ctc_audio_test_fail("Maxwell decoded sample count"));
    }
    if ((audio.max_abs_sample <= 0.0f) || !isfinite(audio.max_abs_sample)) {
        lrc_ctc_audio_destroy(&audio);
        fatal(ctc_audio_test_fail("Maxwell decoded float range"));
    }

    lrc_ctc_audio_destroy(&audio);

    return;
}

int32
main(void) {
    ctc_audio_test_defaults_and_invalid_inputs();
    ctc_audio_test_generated_decode(48000,
                                    2,
                                    0.125,
                                    16000,
                                    "ctc_audio_stereo");
    ctc_audio_test_generated_decode(22050,
                                    1,
                                    0.20,
                                    8000,
                                    "ctc_audio_mono");
    ctc_audio_test_maxwell_vocals();

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_ctc_audio */
