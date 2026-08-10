#include "cbase.h"
#include "lyricsync.h"
#include "audio.h"

#if !defined(TESTING_audio)
#define TESTING_audio 0
#endif

static void
audio_io_format_init(AudioIoFormat *format) {
    format->sample_rate = 44100;
    format->channel_count = 2;

    return;
}

static bool
audio_io_format_valid(AudioIoFormat *format) {
    if (format == NULL) {
        return false;
    }
    if (format->sample_rate <= 0) {
        return false;
    }
    if ((format->channel_count != 1) && (format->channel_count != 2)) {
        return false;
    }

    return true;
}

static bool
audio_opus_sample_rate_valid(int32 sample_rate) {
    return (sample_rate == 48000)
           || (sample_rate == 24000)
           || (sample_rate == 16000)
           || (sample_rate == 12000)
           || (sample_rate == 8000);
}

static void
audio_prepare_output_format(
    AudioIoFormat *format,
    char *container_format
) {
    if ((format == NULL) || (container_format == NULL)) {
        return;
    }
    if (lrc_audio_format_is(container_format, LRC_AUDIO_FORMAT_OPUS)
        && !audio_opus_sample_rate_valid(format->sample_rate)) {
        format->sample_rate = 48000;
    }

    return;
}

static void
audio_buffer_init(AudioBuffer *audio) {
    audio->left = NULL;
    audio->right = NULL;

    audio->frame_count = 0;
    audio->sample_rate = 0;
    audio->channel_count = 0;

    return;
}

static void
audio_buffer_destroy(AudioBuffer *audio) {
    int64 allocation_size = audio->frame_count*SIZEOF(*audio->left);

    free2(audio->left, allocation_size);
    free2(audio->right, allocation_size);
    audio_buffer_init(audio);

    return;
}

static bool
audio_run_process(int32 argc, char **argv) {
    Command command = {0};
    bool result;

    command_push_array(&command, argc, argv);
    result = command_run_capture_all(&command);
    if (result) {
        result = command.result.exited
                 && (command.result.exit_status == 0);
    }
    command_free(&command);

    return result;
}

static bool
audio_check_ffmpeg(char *ffmpeg_path) {
    char *argv[] = {
        ffmpeg_path,
        "-hide_banner",
        "-version",
        NULL,
    };

    return audio_run_process(LENGTH(argv) - 1, argv);
}

static bool
audio_can_decode_file(char *path, char *ffmpeg_path) {
    char *argv[] = {
        ffmpeg_path,
        "-v",
        "error",
        "-i",
        path,
        "-t",
        "0.1",
        "-f",
        "null",
        "-",
        NULL,
    };

    return audio_run_process(LENGTH(argv) - 1, argv);
}

static bool
audio_read_file_format(
    AudioBuffer *audio,
    char *path,
    AudioIoFormat *format,
    char *ffmpeg_path
) {
    char channel_count_arg[32];
    char sample_rate_arg[32];
    char *argv[] = {
        ffmpeg_path,
        "-v",
        "error",
        "-i",
        path,
        "-ac",
        channel_count_arg,
        "-ar",
        sample_rate_arg,
        "-f",
        "f32le",
        "-",
        NULL,
    };
    Command command = {0};
    char *raw;
    bool result = false;
    int64 frame_bytes;
    int64 raw_len;
    int64 sample_count;

    if ((audio == NULL) || (path == NULL) || (ffmpeg_path == NULL)) {
        return false;
    }
    if (!audio_io_format_valid(format)) {
        return false;
    }

    ITOA(channel_count_arg, format->channel_count);
    ITOA(sample_rate_arg, format->sample_rate);

    audio_buffer_destroy(audio);
    command_push_array(&command, LENGTH(argv) - 1, argv);
    if (!command_run_capture_all(&command)) {
        goto cleanup;
    }
    if (!command.result.exited || (command.result.exit_status != 0)) {
        goto cleanup;
    }

    raw_len = command.result.stdout_len;
    frame_bytes = (int64)format->channel_count*SIZEOF(*audio->left);
    if ((raw_len % frame_bytes) != 0) {
        goto cleanup;
    }

    audio->frame_count = raw_len/frame_bytes;
    audio->sample_rate = format->sample_rate;
    audio->channel_count = format->channel_count;
    if (audio->frame_count == 0) {
        result = true;
        goto cleanup;
    }
    if (audio->frame_count > INT64_MAX/SIZEOF(*audio->left)) {
        goto cleanup;
    }

    sample_count = audio->frame_count*SIZEOF(*audio->left);
    audio->left = malloc2(sample_count);
    if (audio->channel_count == 2) {
        audio->right = malloc2(sample_count);
    }

    raw = command.result.stdout_output;
    for (int64 i = 0; i < audio->frame_count; i += 1) {
        int64 frame_offset = frame_bytes*i;

        memcpy64(&audio->left[i],
                 raw + frame_offset,
                 SIZEOF(*audio->left));
        if (audio->channel_count == 2) {
            memcpy64(&audio->right[i],
                     raw + frame_offset + SIZEOF(*audio->left),
                     SIZEOF(*audio->right));
        }
    }
    result = true;

cleanup:
    command_free(&command);
    if (!result) {
        audio_buffer_destroy(audio);
    }

    return result;
}

static bool
audio_buffer_valid(AudioBuffer *audio) {
    AudioIoFormat format;

    if (audio == NULL) {
        return false;
    }

    format.sample_rate = audio->sample_rate;
    format.channel_count = audio->channel_count;
    if (!audio_io_format_valid(&format)) {
        return false;
    }
    if (audio->frame_count < 0) {
        return false;
    }
    if ((audio->frame_count > 0) && (audio->left == NULL)) {
        return false;
    }
    if ((audio->frame_count > 0) && (audio->channel_count == 2)
        && (audio->right == NULL)) {
        return false;
    }
    if ((audio->channel_count == 1) && audio->right) {
        return false;
    }

    return true;
}

static bool
audio_interleaved_buffer_create(
    AudioBuffer *audio,
    char **out,
    int64 *out_len
) {
    float *samples;
    int64 sample_count;
    int64 sample_size;

    if ((audio == NULL) || (out == NULL) || (out_len == NULL)) {
        return false;
    }

    *out = NULL;
    *out_len = 0;
    if (audio->frame_count <= 0) {
        return true;
    }
    if (audio->frame_count > INT64_MAX/audio->channel_count) {
        return false;
    }

    sample_count = audio->frame_count*audio->channel_count;
    if (sample_count > INT64_MAX/SIZEOF(*samples)) {
        return false;
    }
    sample_size = sample_count*SIZEOF(*samples);

    samples = malloc2(sample_size);
    for (int64 i = 0; i < audio->frame_count; i += 1) {
        samples[audio->channel_count*i] = audio->left[i];
        if (audio->channel_count == 2) {
            samples[2*i + 1] = audio->right[i];
        }
    }

    *out = (char *)samples;
    *out_len = sample_size;
    return true;
}

static bool
audio_write_file_format(
    AudioBuffer *audio,
    char *path,
    char *container_format,
    AudioIoFormat *output_format,
    char *ffmpeg_path
) {
    enum {
        AUDIO_WRITE_CONTAINER_FORMAT_ARG = 18,
    };
    AudioIoFormat buffer_format;
    AudioIoFormat file_format;
    LrcAudioFormatInfo *format_info;
    char input_channel_count_arg[32];
    char input_sample_rate_arg[32];
    char output_channel_count_arg[32];
    char output_sample_rate_arg[32];
    char *argv[] = {
        ffmpeg_path,
        "-v",
        "error",
        "-y",
        "-f",
        "f32le",
        "-ar",
        input_sample_rate_arg,
        "-ac",
        input_channel_count_arg,
        "-i",
        "-",
        "-vn",
        "-ac",
        output_channel_count_arg,
        "-ar",
        output_sample_rate_arg,
        "-f",
        container_format,
        path,
        NULL,
    };
    Command command = {0};
    char *interleaved;
    int64 interleaved_size;
    bool result = false;

    if ((audio == NULL) || (path == NULL) || (container_format == NULL)
        || (ffmpeg_path == NULL)) {
        return false;
    }
    if (!audio_buffer_valid(audio)) {
        return false;
    }
    if ((format_info = lrc_audio_format_info_from_name(container_format))
        == NULL) {
        return false;
    }
    if (!format_info->supports_streaming) {
        return false;
    }
    container_format = format_info->name;
    argv[AUDIO_WRITE_CONTAINER_FORMAT_ARG] = container_format;

    buffer_format.sample_rate = audio->sample_rate;
    buffer_format.channel_count = audio->channel_count;
    if (output_format) {
        file_format = *output_format;
    } else {
        file_format = buffer_format;
    }
    audio_prepare_output_format(&file_format, container_format);
    if (!audio_io_format_valid(&file_format)) {
        return false;
    }
    if (!audio_interleaved_buffer_create(audio,
                                         &interleaved,
                                         &interleaved_size)) {
        return false;
    }

    ITOA(input_channel_count_arg, buffer_format.channel_count);
    ITOA(input_sample_rate_arg, buffer_format.sample_rate);
    ITOA(output_channel_count_arg, file_format.channel_count);
    ITOA(output_sample_rate_arg, file_format.sample_rate);

    command_push_array(&command, LENGTH(argv) - 1, argv);
    if (!command_stdin_buffer_set(&command, interleaved, interleaved_size)) {
        goto cleanup;
    }
    if (!command_run_capture_all(&command)) {
        goto cleanup;
    }

    result = command.result.exited && (command.result.exit_status == 0);

cleanup:
    command_free(&command);
    free2(interleaved, interleaved_size);
    return result;
}

#if TESTING
static void
audio_file_info_init(AudioFileInfo *info) {
    info->sample_rate = 0;
    info->channel_count = 0;
    info->estimated_frame_count = 0;

    info->duration_seconds = 0.0;

    return;
}

static bool
audio_parse_int32_field(char *value, int32 *out) {
    char *end;
    int64 parsed;

    errno = 0;
    end = NULL;
    parsed = strtoll(value, &end, 10);
    if ((errno != 0) || (end == value) || (*end != '\0')) {
        return false;
    }
    if ((parsed < 0) || (parsed >= MAXOF(*out))) {
        return false;
    }

    *out = (int32)parsed;

    return true;
}

static bool
audio_parse_double_field(char *value, double *out) {
    char *end;
    double parsed;

    errno = 0;
    end = NULL;
    parsed = strtod(value, &end);
    if ((errno != 0) || (end == value) || (*end != '\0')) {
        return false;
    }
    if (!isfinite(parsed) || (parsed < 0.0)) {
        return false;
    }

    *out = parsed;

    return true;
}

static bool
audio_file_info_parse(AudioFileInfo *info, char *output) {
    bool duration_found;
    bool sample_rate_found;
    bool channels_found;

    audio_file_info_init(info);
    duration_found = false;
    sample_rate_found = false;
    channels_found = false;

    for (int32 start = 0; output[start] != '\0'; start += 1) {
        char *line;
        char *value;
        char saved;
        int32 end = start;

        while ((output[end] != '\0') && (output[end] != '\n')) {
            end += 1;
        }

        line = output + start;
        saved = output[end];
        output[end] = '\0';

        if (strncmp32(line, "sample_rate=", 12) == 0) {
            value = line + 12;
            if (!audio_parse_int32_field(value, &info->sample_rate)) {
                return false;
            }
            sample_rate_found = true;
        } else if (strncmp32(line, "channels=", 9) == 0) {
            value = line + 9;
            if (!audio_parse_int32_field(value, &info->channel_count)) {
                return false;
            }
            channels_found = true;
        } else if (strncmp32(line, "duration=", 9) == 0) {
            value = line + 9;
            if (!audio_parse_double_field(value, &info->duration_seconds)) {
                return false;
            }
            duration_found = true;
        }

        if (saved == '\0') {
            break;
        }
        start = end;
    }

    if (!sample_rate_found || !channels_found || !duration_found) {
        return false;
    }
    if (info->sample_rate <= 0) {
        return false;
    }
    if ((info->channel_count != 1) && (info->channel_count != 2)) {
        return false;
    }

    info->estimated_frame_count =
        (int64)(info->duration_seconds*(double)info->sample_rate + 0.5);

    return true;
}

static bool
audio_file_info_read(
    AudioFileInfo *info,
    char *path,
    char *ffprobe_path
) {
    char *argv[] = {
        ffprobe_path,
        "-v",
        "error",
        "-select_streams",
        "a:0",
        "-show_entries",
        "stream=sample_rate,channels:format=duration",
        "-of",
        "default=noprint_wrappers=1",
        path,
        NULL,
    };
    Command command = {0};
    bool result = false;

    if ((info == NULL) || (path == NULL) || (ffprobe_path == NULL)) {
        return false;
    }

    audio_file_info_init(info);
    command_push_array(&command, LENGTH(argv) - 1, argv);
    if (!command_run_capture_all(&command)) {
        goto cleanup;
    }
    if (!command.result.exited || (command.result.exit_status != 0)) {
        goto cleanup;
    }
    if (command.result.stdout_output == NULL) {
        goto cleanup;
    }

    result = audio_file_info_parse(info, command.result.stdout_output);

cleanup:
    command_free(&command);
    if (!result) {
        audio_file_info_init(info);
    }

    return result;
}

static bool
audio_read_file(AudioBuffer *audio, char *path, char *ffmpeg_path) {
    AudioIoFormat format;

    audio_io_format_init(&format);

    return audio_read_file_format(audio, path, &format, ffmpeg_path);
}

static void
audio_test_sine_options_init(AudioTestSineOptions *options) {
    audio_io_format_init(&options->format);

    options->duration_seconds = 0.25;
    options->frequency_hz = 440.0;
    options->amplitude = 0.25f;

    return;
}

static bool
audio_test_sine_options_valid(AudioTestSineOptions *options) {
    if (options == NULL) {
        return false;
    }
    if (!audio_io_format_valid(&options->format)) {
        return false;
    }
    if (!isfinite(options->duration_seconds)
        || (options->duration_seconds <= 0.0)) {
        return false;
    }
    if (!isfinite(options->frequency_hz) || (options->frequency_hz <= 0.0)) {
        return false;
    }
    if (!isfinite((double)options->amplitude)
        || (options->amplitude < 0.0f)
        || (options->amplitude > 1.0f)) {
        return false;
    }

    return true;
}

static bool
audio_test_generate_sine_wav(
    char *path,
    AudioTestSineOptions *options,
    char *ffmpeg_path
) {
    AudioBuffer audio;
    int64 allocation_size;
    int64 frame_count;
    bool result;

    if ((path == NULL) || (ffmpeg_path == NULL)) {
        return false;
    }
    if (!audio_test_sine_options_valid(options)) {
        return false;
    }

    frame_count = (int64)(options->duration_seconds
                          *(double)options->format.sample_rate + 0.5);
    if (frame_count <= 0) {
        return false;
    }
    if (frame_count > INT64_MAX/SIZEOF(*audio.left)) {
        return false;
    }

    audio_buffer_init(&audio);
    audio.frame_count = frame_count;
    audio.sample_rate = options->format.sample_rate;
    audio.channel_count = options->format.channel_count;

    allocation_size = frame_count*SIZEOF(*audio.left);
    audio.left = malloc2(allocation_size);
    if (audio.channel_count == 2) {
        audio.right = malloc2(allocation_size);
    }

    for (int64 i = 0; i < frame_count; i += 1) {
        double fidx = (double)i;
        double A = (double)options->amplitude;
        double phase = π2*options->frequency_hz*fidx/A;
        float sample = (float)(A*sin(phase));

        audio.left[i] = sample;
        if (audio.channel_count == 2) {
            audio.right[i] = sample;
        }
    }

    result = audio_write_file_format(&audio,
                                     path,
                                     "wav",
                                     &options->format,
                                     ffmpeg_path);
    audio_buffer_destroy(&audio);

    return result;
}
#endif


#if TESTING_audio

#define CBASE_IMPLEMENT
#include "cbase.h"

static bool
audio_write_file(
    AudioBuffer *audio,
    char *path,
    char *format,
    char *ffmpeg_path
) {
    return audio_write_file_format(audio, path, format, NULL, ffmpeg_path);
}

static void
audio_compare_options_init(AudioCompareOptions *options) {
    options->mode = AUDIO_COMPARE_MODE_TOLERANT;

    options->max_offset_frames = 0;
    options->max_length_delta_frames = 0;

    options->max_abs_error = 0.0001f;
    options->max_rms_error = 0.00001f;
    options->min_snr_db = 80.0;

    return;
}

static void
audio_compare_result_init(AudioCompareResult *result) {
    result->mode = AUDIO_COMPARE_MODE_TOLERANT;

    result->decoded = false;
    result->valid = false;
    result->finite = false;
    result->length_ok = false;
    result->passed = false;

    result->expected_frames = 0;
    result->actual_frames = 0;
    result->compared_frames = 0;
    result->compared_samples = 0;
    result->length_delta_frames = 0;
    result->best_offset_frames = 0;
    result->nan_samples = 0;
    result->infinite_samples = 0;

    result->max_abs_error = 0.0f;
    result->rms_error = 0.0f;
    result->expected_peak = 0.0f;
    result->actual_peak = 0.0f;
    result->snr_db = 0.0;

    return;
}

static int64
audio_int64_abs(int64 value) {
    if (value < 0) {
        return -value;
    }

    return value;
}

static float
audio_float_abs(float value) {
    if (value < 0.0f) {
        return -value;
    }

    return value;
}


static float
audio_buffer_channel_sample(
    AudioBuffer *audio,
    int64 frame,
    int32 channel
) {
    if (channel == 0) {
        return audio->left[frame];
    }

    return audio->right[frame];
}

static bool
audio_compare_measure_offset(
    AudioCompareResult *result,
    AudioBuffer *expected,
    AudioBuffer *actual,
    int64 offset_frames
) {
    double error_sum;
    double signal_sum;
    int64 expected_start;
    int64 actual_start;
    int64 frame_count;

    audio_compare_result_init(result);
    result->valid = true;
    result->decoded = true;
    result->finite = true;
    result->expected_frames = expected->frame_count;
    result->actual_frames = actual->frame_count;
    result->length_delta_frames = audio_int64_abs(expected->frame_count
                                                  - actual->frame_count);
    result->best_offset_frames = offset_frames;

    expected_start = 0;
    actual_start = 0;
    if (offset_frames > 0) {
        actual_start = offset_frames;
    } else {
        expected_start = -offset_frames;
    }
    if ((expected_start > expected->frame_count)
        || (actual_start > actual->frame_count)) {
        return false;
    }

    frame_count = expected->frame_count - expected_start;
    if ((actual->frame_count - actual_start) < frame_count) {
        frame_count = actual->frame_count - actual_start;
    }
    if (frame_count < 0) {
        return false;
    }

    result->compared_frames = frame_count;
    result->compared_samples = (int64)expected->channel_count*frame_count;
    if (frame_count == 0) {
        result->snr_db = 999.0;
        return true;
    }

    error_sum = 0.0;
    signal_sum = 0.0;
    for (int64 i = 0; i < frame_count; i += 1) {
        for (int32 channel = 0; channel < expected->channel_count;
             channel += 1) {
            double error;
            float actual_abs;
            float actual_sample;
            float expected_abs;
            float expected_sample;

            expected_sample = audio_buffer_channel_sample(expected,
                                                          expected_start + i,
                                                          channel);
            actual_sample = audio_buffer_channel_sample(actual,
                                                        actual_start + i,
                                                        channel);

            if (isnan(expected_sample) || isnan(actual_sample)) {
                result->nan_samples += 1;
                result->finite = false;
                continue;
            }
            if (isinf(expected_sample) || isinf(actual_sample)) {
                result->infinite_samples += 1;
                result->finite = false;
                continue;
            }

            expected_abs = audio_float_abs(expected_sample);
            actual_abs = audio_float_abs(actual_sample);
            if (expected_abs > result->expected_peak) {
                result->expected_peak = expected_abs;
            }
            if (actual_abs > result->actual_peak) {
                result->actual_peak = actual_abs;
            }

            error = (double)expected_sample - (double)actual_sample;
            error_sum += error*error;
            signal_sum += (double)expected_sample*(double)expected_sample;
            if ((float)fabs(error) > result->max_abs_error) {
                result->max_abs_error = (float)fabs(error);
            }
        }
    }

    if (result->compared_samples > 0) {
        result->rms_error = (float)sqrt(error_sum
                                        /(double)result->compared_samples);
    }
    if (error_sum == 0.0) {
        result->snr_db = 999.0;
    } else if (signal_sum == 0.0) {
        result->snr_db = -999.0;
    } else {
        result->snr_db = 10.0*log10(signal_sum/error_sum);
    }

    return true;
}

static bool
audio_compare_better_result(
    AudioCompareResult *candidate,
    AudioCompareResult *best,
    bool have_best
) {
    if (!have_best) {
        return true;
    }
    if (candidate->rms_error < best->rms_error) {
        return true;
    }
    if ((candidate->rms_error == best->rms_error)
        && (candidate->max_abs_error < best->max_abs_error)) {
        return true;
    }

    return false;
}

static bool
audio_compare_result_passes(
    AudioCompareResult *result,
    AudioCompareOptions *options
) {
    if (!result->valid || !result->decoded || !result->finite) {
        return false;
    }
    if (!result->length_ok) {
        return false;
    }
    if (options->mode == AUDIO_COMPARE_MODE_STRICT) {
        return (result->max_abs_error == 0.0f)
               && (result->rms_error == 0.0f);
    }
    if (options->mode == AUDIO_COMPARE_MODE_SNR) {
        return result->snr_db >= options->min_snr_db;
    }

    return (result->max_abs_error <= options->max_abs_error)
           && (result->rms_error <= options->max_rms_error);
}

static bool
audio_compare_buffers(
    AudioCompareResult *result,
    AudioBuffer *expected,
    AudioBuffer *actual,
    AudioCompareOptions *options
) {
    AudioCompareOptions default_options;
    AudioCompareResult best_result = {0};
    int64 max_offset;
    bool have_best;

    if (result == NULL) {
        return false;
    }

    audio_compare_result_init(result);
    if (options == NULL) {
        audio_compare_options_init(&default_options);
        options = &default_options;
    }
    result->mode = options->mode;

    if (!audio_buffer_valid(expected) || !audio_buffer_valid(actual)) {
        return false;
    }
    if ((expected->sample_rate != actual->sample_rate)
        || (expected->channel_count != actual->channel_count)) {
        result->valid = true;
        result->decoded = true;
        result->expected_frames = expected->frame_count;
        result->actual_frames = actual->frame_count;
        result->length_delta_frames = audio_int64_abs(expected->frame_count
                                                      - actual->frame_count);
        return false;
    }

    result->valid = true;
    result->decoded = true;
    result->finite = true;
    result->expected_frames = expected->frame_count;
    result->actual_frames = actual->frame_count;
    result->length_delta_frames = audio_int64_abs(expected->frame_count
                                                  - actual->frame_count);
    result->length_ok = result->length_delta_frames
                        <= options->max_length_delta_frames;

    max_offset = 0;
    if (options->mode == AUDIO_COMPARE_MODE_OFFSET_TOLERANT) {
        max_offset = options->max_offset_frames;
        if (max_offset < 0) {
            max_offset = 0;
        }
    }

    have_best = false;
    for (int64 offset = -max_offset; offset <= max_offset; offset += 1) {
        AudioCompareResult candidate;

        if (!audio_compare_measure_offset(&candidate,
                                          expected,
                                          actual,
                                          offset)) {
            continue;
        }
        candidate.mode = options->mode;
        candidate.length_ok = result->length_ok;
        if (audio_compare_better_result(&candidate,
                                        &best_result,
                                        have_best)) {
            best_result = candidate;
            have_best = true;
        }
    }

    if (!have_best) {
        return false;
    }

    *result = best_result;
    result->passed = audio_compare_result_passes(result, options);

    return result->passed;
}

static bool
audio_compare_files(
    AudioCompareResult *result,
    char *expected_path,
    char *actual_path,
    AudioCompareOptions *options,
    char *ffmpeg_path
) {
    AudioBuffer expected;
    AudioBuffer actual;
    bool ok;

    if (result == NULL) {
        return false;
    }

    audio_compare_result_init(result);
    audio_buffer_init(&expected);
    audio_buffer_init(&actual);
    if ((expected_path == NULL) || (actual_path == NULL)
        || (ffmpeg_path == NULL)) {
        return false;
    }

    ok = false;
    if (!audio_read_file(&expected, expected_path, ffmpeg_path)) {
        goto cleanup;
    }
    if (!audio_read_file(&actual, actual_path, ffmpeg_path)) {
        goto cleanup;
    }

    result->decoded = true;
    ok = audio_compare_buffers(result, &expected, &actual, options);

cleanup:
    audio_buffer_destroy(&actual);
    audio_buffer_destroy(&expected);

    return ok;
}

static bool
audio_compare_reconstruction_buffers(
    AudioCompareResult *result,
    AudioBuffer *mixture,
    AudioBuffer *first_stem,
    AudioBuffer *second_stem,
    AudioCompareOptions *options
) {
    AudioBuffer reconstructed;
    bool ok;

    if (result == NULL) {
        return false;
    }

    audio_compare_result_init(result);
    if (!audio_buffer_valid(mixture) || !audio_buffer_valid(first_stem)
        || !audio_buffer_valid(second_stem)) {
        return false;
    }
    if ((first_stem->sample_rate != mixture->sample_rate)
        || (second_stem->sample_rate != mixture->sample_rate)
        || (first_stem->channel_count != mixture->channel_count)
        || (second_stem->channel_count != mixture->channel_count)
        || (first_stem->frame_count != mixture->frame_count)
        || (second_stem->frame_count != mixture->frame_count)) {
        result->valid = true;
        result->expected_frames = mixture->frame_count;
        result->actual_frames = first_stem->frame_count;
        return false;
    }

    audio_buffer_init(&reconstructed);
    reconstructed.frame_count = mixture->frame_count;
    reconstructed.sample_rate = mixture->sample_rate;
    reconstructed.channel_count = mixture->channel_count;
    if (reconstructed.frame_count > 0) {
        reconstructed.left = malloc2(reconstructed.frame_count
                                     *SIZEOF(*reconstructed.left));
        if (reconstructed.channel_count == 2) {
            reconstructed.right = malloc2(reconstructed.frame_count
                                          *SIZEOF(*reconstructed.right));
        }
    }

    for (int64 i = 0; i < reconstructed.frame_count; i += 1) {
        reconstructed.left[i] = first_stem->left[i] + second_stem->left[i];
        if (reconstructed.channel_count == 2) {
            reconstructed.right[i] = first_stem->right[i]
                                     + second_stem->right[i];
        }
    }

    ok = audio_compare_buffers(result, mixture, &reconstructed, options);
    audio_buffer_destroy(&reconstructed);

    return ok;
}

static void
audio_compare_result_print(
    AudioCompareResult *result,
    char *name
) {
    char *label = name;
    char *mode;

    if (label == NULL) {
        label = "audio";
    }
    if (result == NULL) {
        error2("%s comparison result is unavailable\n", label);
        return;
    }

    mode = AUDIO_COMPARE_MODE_str(result->mode);
    error2(
        "%s: passed=%d mode=%s expected_frames=%lld "
        "actual_frames=%lld delta=%lld compared_frames=%lld "
        "offset=%lld max_abs=%g rms=%g snr_db=%.2f "
        "expected_peak=%g actual_peak=%g nan=%lld inf=%lld\n",
        label,
        result->passed,
        mode,
        result->expected_frames,
        result->actual_frames,
        result->length_delta_frames,
        result->compared_frames,
        result->best_offset_frames,
        (double)result->max_abs_error,
        (double)result->rms_error,
        result->snr_db,
        (double)result->expected_peak,
        (double)result->actual_peak,
        result->nan_samples,
        result->infinite_samples);
    AUDIO_COMPARE_MODE_str_free(mode);

    return;
}

static int32
audio_test_fail(char *name) {
    error2("audio test failed: %s\n", name);

    return 1;
}

static void
audio_test_buffer(
    AudioBuffer *audio,
    float *left,
    float *right,
    int64 frame_count
) {
    audio->left = left;
    audio->right = right;

    audio->frame_count = frame_count;
    audio->sample_rate = 44100;
    audio->channel_count = 2;

    return;
}

static void
audio_test_compare_helpers(void) {
    AudioBuffer actual;
    AudioBuffer expected;
    AudioBuffer mixture;
    AudioBuffer stem_a;
    AudioBuffer stem_b;
    AudioCompareOptions options;
    AudioCompareResult result;
    char *mode_name;
    float actual_left[] = {0.0f, 0.25f, -0.5f, 0.75f};
    float actual_right[] = {1.0f, -1.0f, 0.5f, -0.25f};
    float expected_left[] = {0.0f, 0.25f, -0.5f, 0.75f};
    float expected_right[] = {1.0f, -1.0f, 0.5f, -0.25f};
    float shifted_left[] = {9.0f, 0.0f, 0.25f, -0.5f};
    float shifted_right[] = {9.0f, 1.0f, -1.0f, 0.5f};
    float mixture_left[] = {1.0f, 0.5f, -0.25f};
    float mixture_right[] = {-0.5f, 0.75f, 0.125f};
    float stem_a_left[] = {0.25f, 0.25f, -0.125f};
    float stem_a_right[] = {-0.25f, 0.25f, 0.5f};
    float stem_b_left[] = {0.75f, 0.25f, -0.125f};
    float stem_b_right[] = {-0.25f, 0.5f, -0.375f};

    audio_test_buffer(&expected,
                      expected_left,
                      expected_right,
                      LENGTH(expected_left));
    audio_test_buffer(&actual,
                      actual_left,
                      actual_right,
                      LENGTH(actual_left));

    mode_name = AUDIO_COMPARE_MODE_str(AUDIO_COMPARE_MODE_OFFSET_TOLERANT);
    if (!strequal(mode_name, "AUDIO_COMPARE_MODE_OFFSET_TOLERANT")) {
        AUDIO_COMPARE_MODE_str_free(mode_name);
        fatal(audio_test_fail("compare mode string"));
    }
    AUDIO_COMPARE_MODE_str_free(mode_name);
    if (AUDIO_COMPARE_MODE_parse("SNR") != AUDIO_COMPARE_MODE_SNR) {
        fatal(audio_test_fail("compare mode parse"));
    }

    audio_compare_options_init(&options);
    options.mode = AUDIO_COMPARE_MODE_STRICT;
    if (!audio_compare_buffers(&result, &expected, &actual, &options)) {
        audio_compare_result_print(&result, "strict equal");
        fatal(audio_test_fail("strict compare equal buffers"));
    }
    if (result.max_abs_error != 0.0f) {
        audio_compare_result_print(&result, "strict metric");
        fatal(audio_test_fail("strict compare metric"));
    }

    actual_left[1] += 0.000001f;
    audio_compare_options_init(&options);
    if (!audio_compare_buffers(&result, &expected, &actual, &options)) {
        audio_compare_result_print(&result, "tolerant close");
        fatal(audio_test_fail("tolerant compare close buffers"));
    }

    actual_left[1] += 0.01f;
    if (audio_compare_buffers(&result, &expected, &actual, &options)) {
        audio_compare_result_print(&result, "tolerant far");
        fatal(audio_test_fail("tolerant compare far buffers"));
    }
    actual_left[1] = expected_left[1];

    audio_test_buffer(&actual,
                      shifted_left,
                      shifted_right,
                      LENGTH(shifted_left));
    audio_compare_options_init(&options);
    options.mode = AUDIO_COMPARE_MODE_OFFSET_TOLERANT;
    options.max_offset_frames = 1;
    if (!audio_compare_buffers(&result, &expected, &actual, &options)) {
        audio_compare_result_print(&result, "offset");
        fatal(audio_test_fail("offset compare shifted buffers"));
    }
    if (result.best_offset_frames != 1) {
        audio_compare_result_print(&result, "offset metric");
        fatal(audio_test_fail("offset compare selected offset"));
    }

    audio_test_buffer(&mixture,
                      mixture_left,
                      mixture_right,
                      LENGTH(mixture_left));
    audio_test_buffer(&stem_a,
                      stem_a_left,
                      stem_a_right,
                      LENGTH(stem_a_left));
    audio_test_buffer(&stem_b,
                      stem_b_left,
                      stem_b_right,
                      LENGTH(stem_b_left));
    audio_compare_options_init(&options);
    options.mode = AUDIO_COMPARE_MODE_STRICT;
    if (!audio_compare_reconstruction_buffers(&result,
                                               &mixture,
                                               &stem_a,
                                               &stem_b,
                                               &options)) {
        audio_compare_result_print(&result, "reconstruction");
        fatal(audio_test_fail("reconstruction compare"));
    }

    audio_test_buffer(&actual,
                      actual_left,
                      actual_right,
                      LENGTH(actual_left));
    audio_compare_options_init(&options);
    options.mode = AUDIO_COMPARE_MODE_SNR;
    options.min_snr_db = 200.0;
    if (!audio_compare_buffers(&result, &expected, &actual, &options)) {
        audio_compare_result_print(&result, "snr equal");
        fatal(audio_test_fail("snr compare equal buffers"));
    }

    return;
}

static bool
audio_test_double_close(double a, double b, double max_error) {
    double diff;

    diff = fabs(a - b);

    return diff <= max_error;
}

static void
audio_test_generated_wave_helpers(void) {
    AudioBuffer decoded;
    AudioFileInfo info;
    AudioIoFormat decode_format;
    AudioTestSineOptions options;
    char mono_path[PATH_MAX];
    char stereo_path[PATH_MAX];
    char temp_dir[PATH_MAX];

    if (!test_command_exists("ffmpeg") || !test_command_exists("ffprobe")) {
        return;
    }

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), "audio_generated");
    test_join_path(mono_path, SIZEOF(mono_path), temp_dir, "mono.wav");
    test_join_path(stereo_path, SIZEOF(stereo_path), temp_dir, "stereo.wav");

    audio_test_sine_options_init(&options);
    options.format.sample_rate = 16000;
    options.format.channel_count = 1;
    options.duration_seconds = 0.20;
    options.frequency_hz = 220.0;
    options.amplitude = 0.5f;
    if (!audio_test_generate_sine_wav(mono_path, &options, "ffmpeg")) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generate mono sine wav"));
    }

    if (!audio_file_info_read(&info, mono_path, "ffprobe")) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("probe generated mono wav"));
    }
    if (info.sample_rate != 16000) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated mono sample rate"));
    }
    if (info.channel_count != 1) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated mono channels"));
    }
    if (!audio_test_double_close(info.duration_seconds, 0.20, 0.02)) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated mono duration"));
    }
    if ((info.estimated_frame_count < 3000)
        || (info.estimated_frame_count > 3400)) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated mono estimated frames"));
    }

    audio_test_sine_options_init(&options);
    options.format.sample_rate = 48000;
    options.format.channel_count = 2;
    options.duration_seconds = 0.125;
    options.frequency_hz = 880.0;
    if (!audio_test_generate_sine_wav(stereo_path, &options, "ffmpeg")) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generate stereo sine wav"));
    }

    if (!audio_file_info_read(&info, stereo_path, "ffprobe")) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("probe generated stereo wav"));
    }
    if (info.sample_rate != 48000) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated stereo sample rate"));
    }
    if (info.channel_count != 2) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated stereo channels"));
    }
    if (!audio_test_double_close(info.duration_seconds, 0.125, 0.02)) {
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("generated stereo duration"));
    }

    audio_buffer_init(&decoded);
    audio_io_format_init(&decode_format);
    decode_format.sample_rate = 48000;
    decode_format.channel_count = 2;
    if (!audio_read_file_format(&decoded,
                                stereo_path,
                                &decode_format,
                                "ffmpeg")) {
        audio_buffer_destroy(&decoded);
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("decode generated stereo wav"));
    }
    if (decoded.frame_count != 6000) {
        audio_buffer_destroy(&decoded);
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("decoded generated stereo frames"));
    }
    if ((decoded.sample_rate != 48000) || (decoded.channel_count != 2)) {
        audio_buffer_destroy(&decoded);
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("decoded generated stereo format"));
    }
    if ((decoded.left[0] < -1.0f) || (decoded.left[0] > 1.0f)
        || (decoded.right[100] < -1.0f) || (decoded.right[100] > 1.0f)) {
        audio_buffer_destroy(&decoded);
        test_remove_tree(temp_dir);
        fatal(audio_test_fail("decoded generated stereo range"));
    }

    audio_buffer_destroy(&decoded);
    test_remove_tree(temp_dir);

    return;
}

int32
main(void) {
    AudioBuffer audio;
    AudioCompareOptions compare_options;
    AudioIoFormat default_format;
    AudioIoFormat mono_format;
    AudioCompareResult compare_result;
    char output[128];
    char output_raw[16];
    char expected_raw[] = {
        0x00,
        0x00,
        (char)0x80,
        0x3f,
        0x00,
        0x00,
        0x00,
        (char)0xc0,
        0x00,
        0x00,
        0x60,
        0x40,
        0x00,
        0x00,
        (char)0x88,
        (char)0xc0,
    };
    char script[128];
    char script_text[] =
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-hide_banner\" ]; then\n"
        "    exit 0\n"
        "fi\n"
        "if [ \"$3\" = \"-y\" ]; then\n"
        "    out=\n"
        "    for arg do out=$arg; done\n"
        "    cat > \"$out\"\n"
        "    exit 0\n"
        "fi\n"
        "printf '\\000\\000\\200\\077\\000\\000\\000\\300'\n"
        "printf '\\000\\000\\140\\100\\000\\000\\210\\300'\n"
        "exit 0\n";
    int32 fd;

    audio_buffer_init(&audio);
    audio_io_format_init(&default_format);
    mono_format = default_format;
    mono_format.sample_rate = 16000;
    mono_format.channel_count = 1;
    if (audio.left || audio.right) {
        fatal(audio_test_fail("buffer pointers"));
    }
    if (audio.frame_count != 0) {
        fatal(audio_test_fail("frame count"));
    }
    if ((default_format.sample_rate != 44100)
        || (default_format.channel_count != 2)) {
        fatal(audio_test_fail("default audio io format"));
    }
    if (!audio_io_format_valid(&mono_format)) {
        fatal(audio_test_fail("mono audio io format"));
    }
    audio_buffer_destroy(&audio);

    audio_test_compare_helpers();
    audio_test_generated_wave_helpers();

    if (audio_check_ffmpeg("/definitely/missing/ffmpeg")) {
        fatal(audio_test_fail("missing ffmpeg accepted"));
    }
    if (audio_can_decode_file("missing.wav",
                              "/definitely/missing/ffmpeg")) {
        fatal(audio_test_fail("missing ffmpeg decode accepted"));
    }

    if (SNPRINTF(script,
                 "/tmp/uvr_fake_ffmpeg_%lld",
                 (int64)getpid()) < 0) {
        fatal(audio_test_fail("fake ffmpeg path"));
    }
    if (SNPRINTF(output,
                 "/tmp/uvr_fake_audio_%lld",
                 (int64)getpid()) < 0) {
        fatal(audio_test_fail("fake output path"));
    }
    fd = open(script, O_WRONLY|O_CREAT|O_EXCL, 0700);
    if (fd < 0) {
        fatal(audio_test_fail("fake ffmpeg file"));
    }
    if (write64(fd, script_text, SIZEOF(script_text) - 1)
        != SIZEOF(script_text) - 1) {
        close(fd);
        unlink(script);
        fatal(audio_test_fail("fake ffmpeg write"));
    }
    close(fd);
    if (chmod(script, 0700) != 0) {
        unlink(script);
        fatal(audio_test_fail("fake ffmpeg chmod"));
    }

    if (!audio_check_ffmpeg(script)) {
        unlink(script);
        fatal(audio_test_fail("fake ffmpeg version"));
    }
    if (!audio_can_decode_file("input.mp3", script)) {
        unlink(script);
        fatal(audio_test_fail("fake ffmpeg decode check"));
    }
    audio_compare_options_init(&compare_options);
    if (!audio_compare_files(&compare_result,
                             "expected.flac",
                             "actual.wav",
                             &compare_options,
                             script)) {
        audio_compare_result_print(&compare_result, "file compare");
        unlink(script);
        fatal(audio_test_fail("compare fake audio files"));
    }
    if (compare_result.expected_frames != 2) {
        audio_compare_result_print(&compare_result, "file compare frames");
        unlink(script);
        fatal(audio_test_fail("compare fake audio frames"));
    }
    if (!audio_read_file(&audio, "input.mp3", script)) {
        unlink(script);
        fatal(audio_test_fail("read fake audio"));
    }

    if (audio.sample_rate != 44100) {
        audio_buffer_destroy(&audio);
        fatal(audio_test_fail("sample rate"));
    }
    if (audio.channel_count != 2) {
        audio_buffer_destroy(&audio);
        fatal(audio_test_fail("channels"));
    }
    if (audio.frame_count != 2) {
        audio_buffer_destroy(&audio);
        fatal(audio_test_fail("decoded frame count"));
    }
    if ((audio.left[0] != 1.0f) || (audio.right[0] != -2.0f)) {
        audio_buffer_destroy(&audio);
        fatal(audio_test_fail("decoded first frame"));
    }
    if ((audio.left[1] != 3.5f) || (audio.right[1] != -4.25f)) {
        audio_buffer_destroy(&audio);
        fatal(audio_test_fail("decoded second frame"));
    }

    unlink(output);
    if (!audio_write_file(&audio, output, "wav", script)) {
        audio_buffer_destroy(&audio);
        unlink(script);
        fatal(audio_test_fail("write fake audio"));
    }
    fd = open(output, O_RDONLY);
    if (fd < 0) {
        audio_buffer_destroy(&audio);
        unlink(script);
        fatal(audio_test_fail("open fake output"));
    }
    if (read64(fd, output_raw, SIZEOF(output_raw))
        != SIZEOF(output_raw)) {
        close(fd);
        audio_buffer_destroy(&audio);
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("read fake output"));
    }
    close(fd);
    for (int32 i = 0; i < (int32)SIZEOF(output_raw); i += 1) {
        if (output_raw[i] != expected_raw[i]) {
            audio_buffer_destroy(&audio);
            unlink(script);
            unlink(output);
            fatal(audio_test_fail("output interleaving"));
        }
    }

    audio_buffer_destroy(&audio);
    if (!audio_read_file_format(&audio,
                                "input.mp3",
                                &mono_format,
                                script)) {
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("read fake mono audio"));
    }
    if (audio.sample_rate != 16000) {
        audio_buffer_destroy(&audio);
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("mono sample rate"));
    }
    if (audio.channel_count != 1) {
        audio_buffer_destroy(&audio);
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("mono channels"));
    }
    if (audio.frame_count != 4) {
        audio_buffer_destroy(&audio);
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("mono decoded frame count"));
    }
    if (audio.right) {
        audio_buffer_destroy(&audio);
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("mono right channel"));
    }
    if ((audio.left[0] != 1.0f) || (audio.left[1] != -2.0f)
        || (audio.left[2] != 3.5f) || (audio.left[3] != -4.25f)) {
        audio_buffer_destroy(&audio);
        unlink(script);
        unlink(output);
        fatal(audio_test_fail("mono decoded samples"));
    }

    unlink(output);
    if (!audio_write_file_format(&audio,
                                 output,
                                 "wav",
                                 &mono_format,
                                 script)) {
        audio_buffer_destroy(&audio);
        unlink(script);
        fatal(audio_test_fail("write fake mono audio"));
    }

    audio_buffer_destroy(&audio);
    unlink(script);
    unlink(output);

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_audio */
