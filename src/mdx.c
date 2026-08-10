#include "cbase.h"
#include "lyricsync.h"
#include "mdx.h"
#include "progress.c"

#if !defined(TESTING_mdx)
#define TESTING_mdx 0
#endif

static float
mdx_output_sample(MdxConfig *config, float input_sample, float model_sample) {
    float sample = model_sample;

    if (config->model_output == MDX_MODEL_OUTPUT_INSTRUMENTAL) {
        sample = input_sample - model_sample;
    }

    sample *= config->compensate;

    if (config->clip_mode == MDX_CLIP_MODE_CLAMP) {
        if (sample > 1.0f) {
            sample = 1.0f;
        }
        if (sample < -1.0f) {
            sample = -1.0f;
        }
    }

    return sample;
}

static void
mdx_config_init(MdxConfig *config) {
    config->sample_rate = 44100;
    config->channel_count = 2;
    config->dim_c = 4;
    config->n_fft = 6144;
    config->hop = 1024;
    config->dim_f = 0;
    config->dim_t = 0;
    config->chunk_seconds = 30;
    config->margin_seconds = 3;

    config->chunk_size = 0;
    config->trim = 0;
    config->gen_size = 0;

    config->compensate = 1.035f;
    config->denoise = false;

    config->model_output = MDX_MODEL_OUTPUT_VOCALS;
    config->clip_mode = MDX_CLIP_MODE_CLAMP;

    return;
}

static bool
mdx_config_prepare(MdxConfig *config) {
    int32 max_dim_f;
    int32 chunk_size;
    int32 trim;

    if (config == NULL) {
        error2("MDX configuration is missing\n");
        return false;
    }
    if (config->sample_rate != 44100) {
        error2("MDX sample rate must be 44100, got %d\n",
                config->sample_rate);
        return false;
    }
    if (config->channel_count != 2) {
        error2("MDX audio channel count must be 2, got %d\n",
                config->channel_count);
        return false;
    }
    if (config->dim_c != 4) {
        error2("MDX spectrogram channel count must be 4, got %d\n",
                config->dim_c);
        return false;
    }
    if (config->n_fft <= 0) {
        error2("MDX n_fft must be greater than zero\n");
        return false;
    }
    if ((config->n_fft & 1) != 0) {
        error2("MDX n_fft must be even, got %d\n",
                config->n_fft);
        return false;
    }
    if (config->hop <= 0) {
        error2("MDX hop must be greater than zero\n");
        return false;
    }
    if (config->hop > config->n_fft) {
        error2("MDX hop must not exceed n_fft, got %d/%d\n",
                config->hop,
                config->n_fft);
        return false;
    }
    if (config->dim_f <= 0) {
        error2("MDX dim_f must be known before preparing config\n");
        return false;
    }
    if (config->dim_t <= 1) {
        error2("MDX dim_t must be greater than 1, got %d\n",
                config->dim_t);
        return false;
    }
    if (config->chunk_seconds <= 0) {
        error2("MDX chunk seconds must be greater than zero\n");
        return false;
    }
    if (config->margin_seconds < 0) {
        error2("MDX margin seconds must not be negative\n");
        return false;
    }
    if (config->compensate < 0.0f) {
        error2("MDX compensate must not be negative\n");
        return false;
    }

    max_dim_f = config->n_fft/2 + 1;
    if (config->dim_f > max_dim_f) {
        error2("MDX dim_f=%d exceeds STFT bins=%d\n",
                config->dim_f,
                max_dim_f);
        return false;
    }
    if ((config->dim_t - 1) > INT32_MAX/config->hop) {
        error2("MDX chunk size overflows int32\n");
        return false;
    }

    chunk_size = config->hop*(config->dim_t - 1);
    trim = config->n_fft/2;
    if (chunk_size <= 2*trim) {
        error2("MDX gen_size must be positive; got chunk=%d trim=%d\n",
               chunk_size, trim);
        return false;
    }

    config->chunk_size = chunk_size;
    config->trim = trim;
    config->gen_size = chunk_size - 2*trim;

    return true;
}

static int64
mdx_input_tensor_len(MdxConfig *config) {
    int64 dim_f;
    int64 dim_t;
    int64 dim_c;
    int64 freq_time;

    if (config == NULL) {
        return -1;
    }
    if ((config->dim_f <= 0) || (config->dim_t <= 0)
        || (config->dim_c <= 0)) {
        return -1;
    }

    dim_f = (int64)config->dim_f;
    dim_t = (int64)config->dim_t;
    dim_c = (int64)config->dim_c;
    if (dim_f > INT64_MAX/dim_t) {
        return -1;
    }
    freq_time = dim_f*dim_t;
    if (dim_c > INT64_MAX/freq_time) {
        return -1;
    }

    return dim_c*freq_time;
}

static bool
mdx_pack_input(
    MdxConfig *config,
    StftPlan *stft_plan,
    float *left,
    float *right,
    int64 frame_count,
    float *tensor,
    int64 tensor_len
) {
    float *left_real;
    float *left_imag;
    float *right_real;
    float *right_imag;
    int64 full_len;
    int64 wanted_len;
    int64 channel_stride;
    int32 stft_frames;
    int32 complex_count;

    if ((config == NULL) || (stft_plan == NULL) || (left == NULL)
        || (right == NULL) || (tensor == NULL)) {
        return false;
    }
    if ((config->dim_c != 4) || (config->dim_f <= 0)
        || (config->dim_t <= 0)) {
        return false;
    }
    if ((stft_plan->n_fft != config->n_fft)
        || (stft_plan->hop != config->hop)) {
        return false;
    }
    if (stft_plan->complex_count < config->dim_f) {
        return false;
    }
    if ((config->chunk_size <= 0)
        || (frame_count != config->chunk_size)) {
        return false;
    }

    wanted_len = mdx_input_tensor_len(config);
    if ((wanted_len <= 0) || (tensor_len < wanted_len)) {
        return false;
    }

    stft_frames = stft_frame_count(stft_plan, frame_count);
    if (stft_frames != config->dim_t) {
        return false;
    }

    complex_count = stft_plan->complex_count;
    if ((int64)complex_count > INT64_MAX/(int64)stft_frames) {
        return false;
    }
    full_len = (int64)complex_count*(int64)stft_frames;
    if (full_len > INT64_MAX/SIZEOF(*left_real)) {
        return false;
    }

    left_real = malloc2(full_len*SIZEOF(*left_real));
    left_imag = malloc2(full_len*SIZEOF(*left_imag));
    right_real = malloc2(full_len*SIZEOF(*right_real));
    right_imag = malloc2(full_len*SIZEOF(*right_imag));

    if (!stft_forward_channel(stft_plan,
                              left,
                              frame_count,
                              left_real,
                              left_imag,
                              stft_frames)) {
        free2(left_real, full_len*SIZEOF(*left_real));
        free2(left_imag, full_len*SIZEOF(*left_imag));
        free2(right_real, full_len*SIZEOF(*right_real));
        free2(right_imag, full_len*SIZEOF(*right_imag));
        return false;
    }
    if (!stft_forward_channel(stft_plan,
                              right,
                              frame_count,
                              right_real,
                              right_imag,
                              stft_frames)) {
        free2(left_real, full_len*SIZEOF(*left_real));
        free2(left_imag, full_len*SIZEOF(*left_imag));
        free2(right_real, full_len*SIZEOF(*right_real));
        free2(right_imag, full_len*SIZEOF(*right_imag));
        return false;
    }

    channel_stride = (int64)config->dim_f*(int64)config->dim_t;
    for (int32 bin = 0; bin < config->dim_f; bin += 1) {
        for (int32 frame = 0; frame < config->dim_t; frame += 1) {
            int64 input_index = (int64)bin*(int64)stft_frames + (int64)frame;
            int64 output_index;

            output_index = (int64)bin*(int64)config->dim_t
                           + (int64)frame;

            tensor[output_index] = left_real[input_index];
            tensor[channel_stride + output_index] = left_imag[input_index];
            tensor[2*channel_stride + output_index] = right_real[input_index];
            tensor[3*channel_stride + output_index] = right_imag[input_index];
        }
    }

    free2(left_real, full_len*SIZEOF(*left_real));
    free2(left_imag, full_len*SIZEOF(*left_imag));
    free2(right_real, full_len*SIZEOF(*right_real));
    free2(right_imag, full_len*SIZEOF(*right_imag));

    return true;
}

static void
mdx_model_info_init_empty(MdxModelInfo *info) {
    info->input_name = NULL;
    info->output_name = NULL;

    info->batch_size = 0;
    info->channel_count = 0;
    info->dim_f = 0;
    info->dim_t = 0;

    info->input_shape_dynamic = false;
    info->output_shape_dynamic = false;

    return;
}

static bool
mdx_unpack_output(
    MdxConfig *config,
    StftPlan *stft_plan,
    float *tensor,
    int64 tensor_len,
    float *left,
    float *right,
    int64 frame_count
) {
    float *left_real;
    float *left_imag;
    float *right_real;
    float *right_imag;
    int64 full_len;
    int64 wanted_len;
    int64 channel_stride;
    int32 stft_frames;
    int32 complex_count;

    if ((config == NULL) || (stft_plan == NULL) || (tensor == NULL)
        || (left == NULL) || (right == NULL)) {
        return false;
    }
    if ((config->dim_c != 4) || (config->dim_f <= 0)
        || (config->dim_t <= 0)) {
        return false;
    }
    if ((stft_plan->n_fft != config->n_fft)
        || (stft_plan->hop != config->hop)) {
        return false;
    }
    if (stft_plan->complex_count < config->dim_f) {
        return false;
    }
    if ((config->chunk_size <= 0)
        || (frame_count != config->chunk_size)) {
        return false;
    }

    wanted_len = mdx_input_tensor_len(config);
    if ((wanted_len <= 0) || (tensor_len < wanted_len)) {
        return false;
    }

    stft_frames = stft_frame_count(stft_plan, frame_count);
    if (stft_frames != config->dim_t) {
        return false;
    }

    complex_count = stft_plan->complex_count;
    if ((int64)complex_count > INT64_MAX/(int64)stft_frames) {
        return false;
    }
    full_len = (int64)complex_count*(int64)stft_frames;
    if (full_len > INT64_MAX/SIZEOF(*left_real)) {
        return false;
    }

    left_real = malloc2_zero(full_len*SIZEOF(*left_real));
    left_imag = malloc2_zero(full_len*SIZEOF(*left_imag));
    right_real = malloc2_zero(full_len*SIZEOF(*right_real));
    right_imag = malloc2_zero(full_len*SIZEOF(*right_imag));

    channel_stride = (int64)config->dim_f*(int64)config->dim_t;
    for (int32 bin = 0; bin < config->dim_f; bin += 1) {
        for (int32 frame = 0; frame < config->dim_t; frame += 1) {
            int64 input_index = (int64)bin*(int64)config->dim_t + (int64)frame;
            int64 output_index = (int64)bin*(int64)stft_frames + (int64)frame;


            left_real[output_index] = tensor[input_index];
            left_imag[output_index] = tensor[channel_stride + input_index];
            right_real[output_index] = tensor[2*channel_stride + input_index];
            right_imag[output_index] = tensor[3*channel_stride + input_index];
        }
    }

    if (!stft_inverse_channel(stft_plan,
                              left_real,
                              left_imag,
                              stft_frames,
                              left,
                              frame_count)) {
        free2(left_real, full_len*SIZEOF(*left_real));
        free2(left_imag, full_len*SIZEOF(*left_imag));
        free2(right_real, full_len*SIZEOF(*right_real));
        free2(right_imag, full_len*SIZEOF(*right_imag));
        return false;
    }
    if (!stft_inverse_channel(stft_plan,
                              right_real,
                              right_imag,
                              stft_frames,
                              right,
                              frame_count)) {
        free2(left_real, full_len*SIZEOF(*left_real));
        free2(left_imag, full_len*SIZEOF(*left_imag));
        free2(right_real, full_len*SIZEOF(*right_real));
        free2(right_imag, full_len*SIZEOF(*right_imag));
        return false;
    }

    free2(left_real, full_len*SIZEOF(*left_real));
    free2(left_imag, full_len*SIZEOF(*left_imag));
    free2(right_real, full_len*SIZEOF(*right_real));
    free2(right_imag, full_len*SIZEOF(*right_imag));

    return true;
}


static bool
mdx_process_song_with_progress(
    MdxConfig *config,
    StftPlan *stft_plan,
    OrtContext *ort_context,
    OrtModel *ort_model,
    AudioBuffer *input,
    AudioBuffer *output,
    bool print_progress
) {
    OrtTensor input_tensor;
    OrtTensor output_tensor;
    float *input_data;
    float *window_left;
    float *window_right;
    float *window_output_left;
    float *window_output_right;
    int64 model_shape[4];
    int64 tensor_len;
    int64 song_chunk_size;
    int64 margin_size;
    int64 total_windows;
    int64 processed_windows;
    LrcProgress progress;
    bool result;

    if ((config == NULL) || (stft_plan == NULL) || (ort_context == NULL)
        || (ort_model == NULL) || (input == NULL) || (output == NULL)) {
        return false;
    }
    if ((config->chunk_size <= 0) || (config->gen_size <= 0)
        || (config->trim <= 0)) {
        return false;
    }
    if ((stft_plan->n_fft != config->n_fft)
        || (stft_plan->hop != config->hop)) {
        return false;
    }
    if ((input->sample_rate != config->sample_rate)
        || (input->channel_count != config->channel_count)) {
        return false;
    }
    if (input->frame_count < 0) {
        return false;
    }
    if ((input->frame_count > 0)
        && ((input->left == NULL) || (input->right == NULL))) {
        return false;
    }

    audio_buffer_destroy(output);
    output->sample_rate = config->sample_rate;
    output->channel_count = config->channel_count;
    output->frame_count = input->frame_count;
    if (input->frame_count == 0) {
        return true;
    }
    if (input->frame_count > INT64_MAX/SIZEOF(*output->left)) {
        audio_buffer_destroy(output);
        return false;
    }

    output->left = malloc2(input->frame_count*SIZEOF(*output->left));
    output->right = malloc2(input->frame_count*SIZEOF(*output->right));
    for (int64 i = 0; i < input->frame_count; i += 1) {
        output->left[i] = 0.0f;
        output->right[i] = 0.0f;
    }

    tensor_len = mdx_input_tensor_len(config);
    if (tensor_len <= 0) {
        audio_buffer_destroy(output);
        return false;
    }
    if (tensor_len > INT64_MAX/SIZEOF(*input_data)) {
        audio_buffer_destroy(output);
        return false;
    }
    input_data = malloc2(tensor_len*SIZEOF(*input_data));
    window_left = malloc2(config->chunk_size*SIZEOF(*window_left));
    window_right = malloc2(config->chunk_size*SIZEOF(*window_right));
    window_output_left = malloc2(
        config->chunk_size*SIZEOF(*window_output_left)
    );
    window_output_right = malloc2(
        config->chunk_size*SIZEOF(*window_output_right)
    );

    model_shape[0] = 1;
    model_shape[1] = config->dim_c;
    model_shape[2] = config->dim_f;
    model_shape[3] = config->dim_t;
    song_chunk_size = (int64)config->chunk_seconds
                      *(int64)config->sample_rate;
    margin_size = (int64)config->margin_seconds
                  *(int64)config->sample_rate;
    total_windows = 0;
    for (int64 region_start = 0;
         region_start < input->frame_count;
         region_start += song_chunk_size) {
        int64 region_end = region_start + song_chunk_size;
        int64 region_len;

        if (region_end > input->frame_count) {
            region_end = input->frame_count;
        }
        region_len = region_end - region_start;
        total_windows += (region_len + config->gen_size - 1)
                         /(int64)config->gen_size;
    }

    result = false;
    processed_windows = 0;
    lrc_progress_init(&progress,
                      print_progress,
                      "process MDX chunks",
                      total_windows);
    lrc_progress_begin(&progress);
    ort_tensor_init_empty(&input_tensor);
    ort_tensor_init_empty(&output_tensor);
    for (int64 region_start = 0;
         region_start < input->frame_count;
         region_start += song_chunk_size) {
        int64 region_end = region_start + song_chunk_size;
        int64 region_source_start;
        int64 region_source_end;

        if (region_end > input->frame_count) {
            region_end = input->frame_count;
        }
        region_source_start = region_start - margin_size;
        if (region_source_start < 0) {
            region_source_start = 0;
        }
        region_source_end = region_end + margin_size;
        if (region_source_end > input->frame_count) {
            region_source_end = input->frame_count;
        }

        for (int64 output_start = region_start;
             output_start < region_end;
             output_start += config->gen_size) {
            int64 output_count = region_end - output_start;
            int64 source_start;

            if (output_count > config->gen_size) {
                output_count = config->gen_size;
            }
            source_start = output_start - config->trim;
            for (int32 i = 0; i < config->chunk_size; i += 1) {
                int64 source_index = source_start + (int64)i;

                if ((source_index < 0) || (source_index >= input->frame_count)
                    || (source_index < region_source_start)
                    || (source_index >= region_source_end)) {
                    window_left[i] = 0.0f;
                    window_right[i] = 0.0f;
                } else {
                    window_left[i] = input->left[source_index];
                    window_right[i] = input->right[source_index];
                }
            }

            if (!mdx_pack_input(config,
                                stft_plan,
                                window_left,
                                window_right,
                                config->chunk_size,
                                input_data,
                                tensor_len)) {
                goto cleanup;
            }
            if (!ort_tensor_create_f32(ort_context,
                                       &input_tensor,
                                       input_data,
                                       tensor_len,
                                       model_shape,
                                       4)) {
                goto cleanup;
            }
            if (!ort_model_run_f32(ort_context,
                                   ort_model,
                                   &input_tensor,
                                   &output_tensor)) {
                goto cleanup;
            }
            if ((output_tensor.data_len != tensor_len)
                || (output_tensor.shape_len != 4)
                || (output_tensor.shape[0] != 1)
                || (output_tensor.shape[1] != config->dim_c)
                || (output_tensor.shape[2] != config->dim_f)
                || (output_tensor.shape[3] != config->dim_t)) {
                error2(
                    "ONNX model returned unexpected output shape\n");
                goto cleanup;
            }
            if (!mdx_unpack_output(config,
                                   stft_plan,
                                   output_tensor.data,
                                   output_tensor.data_len,
                                   window_output_left,
                                   window_output_right,
                                   config->chunk_size)) {
                goto cleanup;
            }

            for (int64 i = 0; i < output_count; i += 1) {
                int64 output_index = output_start + i;
                int64 window_index = (int64)config->trim + i;

                output->left[output_index] = mdx_output_sample(
                    config,
                    input->left[output_index],
                    window_output_left[window_index]
                );
                output->right[output_index] = mdx_output_sample(
                    config,
                    input->right[output_index],
                    window_output_right[window_index]
                );
            }

            ort_tensor_destroy(ort_context, &output_tensor);
            ort_tensor_destroy(ort_context, &input_tensor);
            processed_windows += 1;
            lrc_progress_update(&progress, processed_windows);
        }
    }

    result = true;

cleanup:
    ort_tensor_destroy(ort_context, &output_tensor);
    ort_tensor_destroy(ort_context, &input_tensor);
    free2(input_data, tensor_len*SIZEOF(*input_data));
    free2(window_left, config->chunk_size*SIZEOF(*window_left));
    free2(window_right, config->chunk_size*SIZEOF(*window_right));
    free2(window_output_left,
          config->chunk_size*SIZEOF(*window_output_left));
    free2(window_output_right,
          config->chunk_size*SIZEOF(*window_output_right));
    if (result) {
        lrc_progress_finish(&progress);
    } else {
        lrc_progress_cancel(&progress);
        audio_buffer_destroy(output);
    }

    return result;
}

static bool
mdx_model_inspect(MdxModelInfo *info, MdxConfig *config, OrtModel *model) {
    int64 input_batch;
    int64 input_channels;
    int64 input_dim_f;
    int64 input_dim_t;
    int64 output_batch;
    int64 output_channels;
    int64 output_dim_f;
    int64 output_dim_t;
    OrtModelIoInfo input_info;
    OrtModelIoInfo output_info;

    mdx_model_info_init_empty(info);
    if ((config == NULL) || (model == NULL)) {
        error2("MDX model inspection arguments are invalid\n");
        return false;
    }
    ort_model_io_info_init_empty(&input_info);
    ort_model_io_info_init_empty(&output_info);
    if (!ort_model_input_info(model, &input_info)
        || !ort_model_output_info(model, &output_info)) {
        return false;
    }
    if ((input_info.count != 1) || (output_info.count != 1)) {
        error2(
            "MDX models must have 1 input and 1 output, got %d/%d\n",
            input_info.count,
            output_info.count);
        return false;
    }
    if (input_info.shape_len != 4) {
        error2("MDX model input rank must be 4, got %d\n",
                input_info.shape_len);
        return false;
    }
    if (output_info.shape_len != 4) {
        error2("MDX model output rank must be 4, got %d\n",
                output_info.shape_len);
        return false;
    }

    input_batch = input_info.shape[0];
    input_channels = input_info.shape[1];
    input_dim_f = input_info.shape[2];
    input_dim_t = input_info.shape[3];
    output_batch = output_info.shape[0];
    output_channels = output_info.shape[1];
    output_dim_f = output_info.shape[2];
    output_dim_t = output_info.shape[3];

    if ((input_batch > 0) && (input_batch != 1)) {
        error2("MDX model input batch must be 1, got %lld\n",
                input_batch);
        return false;
    }
    if ((output_batch > 0) && (output_batch != 1)) {
        error2("MDX model output batch must be 1, got %lld\n",
                output_batch);
        return false;
    }
    if ((input_channels > 0) && (input_channels != config->dim_c)) {
        error2("MDX model input channels must be %d, got %lld\n",
                config->dim_c,
                input_channels);
        return false;
    }
    if ((output_channels > 0) && (output_channels != config->dim_c)) {
        error2("MDX model output channels must be %d, got %lld\n",
                config->dim_c,
                output_channels);
        return false;
    }

    if ((input_dim_f > INT32_MAX) || (input_dim_t > INT32_MAX)) {
        error2("MDX model input dimensions are too large\n");
        return false;
    }
    if ((output_dim_f > INT32_MAX) || (output_dim_t > INT32_MAX)) {
        error2("MDX model output dimensions are too large\n");
        return false;
    }

    if (input_dim_f > 0) {
        if ((config->dim_f > 0) && (config->dim_f != input_dim_f)) {
            error2("--dim-f=%d does not match model dim_f=%lld\n",
                    config->dim_f,
                    input_dim_f);
            return false;
        }
        config->dim_f = (int32)input_dim_f;
    }
    if (input_dim_t > 0) {
        if ((config->dim_t > 0) && (config->dim_t != input_dim_t)) {
            error2("--dim-t=%d does not match model dim_t=%lld\n",
                    config->dim_t,
                    input_dim_t);
            return false;
        }
        config->dim_t = (int32)input_dim_t;
    }
    if (output_dim_f > 0) {
        if ((config->dim_f > 0) && (config->dim_f != output_dim_f)) {
            error2(
                "MDX output dim_f=%lld does not match dim_f=%d\n",
                output_dim_f,
                config->dim_f);
            return false;
        }
        config->dim_f = (int32)output_dim_f;
    }
    if (output_dim_t > 0) {
        if ((config->dim_t > 0) && (config->dim_t != output_dim_t)) {
            error2(
                "MDX output dim_t=%lld does not match dim_t=%d\n",
                output_dim_t,
                config->dim_t);
            return false;
        }
        config->dim_t = (int32)output_dim_t;
    }
    if (config->dim_f <= 0) {
        error2("MDX model has dynamic dim_f; pass --dim-f\n");
        return false;
    }
    if (config->dim_t <= 0) {
        error2("MDX model has dynamic dim_t; pass --dim-t\n");
        return false;
    }

    info->input_name = input_info.name;
    info->output_name = output_info.name;
    info->batch_size = 1;
    info->channel_count = config->dim_c;
    info->dim_f = config->dim_f;
    info->dim_t = config->dim_t;
    info->input_shape_dynamic = (input_batch <= 0)
                                || (input_channels <= 0)
                                || (input_dim_f <= 0)
                                || (input_dim_t <= 0);
    info->output_shape_dynamic = (output_batch <= 0)
                                 || (output_channels <= 0)
                                 || (output_dim_f <= 0)
                                 || (output_dim_t <= 0);

    return true;
}

#if TESTING_mdx
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "ort.c"
#include "stft.c"
#include "fftw.c"
#include "audio.c"

static bool
mdx_process_song(
    MdxConfig *config,
    StftPlan *stft_plan,
    OrtContext *ort_context,
    OrtModel *ort_model,
    AudioBuffer *input,
    AudioBuffer *output
) {
    return mdx_process_song_with_progress(config,
                                          stft_plan,
                                          ort_context,
                                          ort_model,
                                          input,
                                          output,
                                          false);
}

typedef struct MdxTestStderrSilence {
    int32 saved_stderr;
    int32 null_fd;
} MdxTestStderrSilence;

static void
mdx_test_stderr_silence_begin(MdxTestStderrSilence *silence) {
    fflush(stderr);

    silence->saved_stderr = dup(STDERR_FILENO);
    ASSERT(silence->saved_stderr >= 0);

    silence->null_fd = open("/dev/null", O_WRONLY);
    if (silence->null_fd < 0) {
        XCLOSE(&silence->saved_stderr);
        ASSERT(false);
    }

    xdup2(silence->null_fd, STDERR_FILENO);

    return;
}

static void
mdx_test_stderr_silence_end(MdxTestStderrSilence *silence) {
    fflush(stderr);
    xdup2(silence->saved_stderr, STDERR_FILENO);
    XCLOSE(&silence->null_fd);
    XCLOSE(&silence->saved_stderr);

    return;
}

#define ASSERT_SILENT_FAILURE(EXPRESSION) \
    do { \
        MdxTestStderrSilence silence; \
        bool expression_result; \
        mdx_test_stderr_silence_begin(&silence); \
        expression_result = (EXPRESSION); \
        mdx_test_stderr_silence_end(&silence); \
        ASSERT(!expression_result); \
    } while (0)

static bool
mdx_float_close(float a, float b) {
    return fabsf(a - b) < 0.001f;
}

int32
main(void) {
    AudioBuffer empty_input;
    AudioBuffer empty_output;
    MdxConfig config;
    MdxModelInfo info;
    OrtContext context;
    OrtModel model;
    StftPlan stft_plan;
    float left[12];
    float right[12];
    float tensor[80];
    float too_small[79];
    float left_real[20];
    float left_imag[20];
    float right_real[20];
    float right_imag[20];
    float unpack_left[12];
    float unpack_right[12];
    int64 channel_stride;

    audio_buffer_init(&empty_input);
    audio_buffer_init(&empty_output);
    ort_context_init_empty(&context);

    mdx_config_init(&config);
    ASSERT(config.sample_rate == 44100);
    ASSERT(config.channel_count == 2);
    ASSERT(config.dim_c == 4);
    ASSERT(config.n_fft == 6144);
    ASSERT(config.hop == 1024);
    ASSERT(config.chunk_size == 0);
    ASSERT(config.trim == 0);
    ASSERT(config.gen_size == 0);
    ASSERT(config.model_output == MDX_MODEL_OUTPUT_VOCALS);
    ASSERT(config.clip_mode == MDX_CLIP_MODE_CLAMP);
    ASSERT(mdx_float_close(mdx_output_sample(&config, 0.75f, 0.5f), 0.5175f));
    config.model_output = MDX_MODEL_OUTPUT_INSTRUMENTAL;
    ASSERT(mdx_float_close(mdx_output_sample(&config, 0.75f, 0.5f), 0.25875f));
    config.compensate = 2.0f;
    ASSERT(mdx_float_close(mdx_output_sample(&config, 0.75f, 0.0f), 1.0f));
    config.clip_mode = MDX_CLIP_MODE_NONE;
    ASSERT(mdx_float_close(mdx_output_sample(&config, 0.75f, 0.0f), 1.5f));


    ort_model_init_empty(&model);
    model.input_name = "input";
    model.output_name = "output";
    model.input_count = 1;
    model.output_count = 1;
    model.input_shape_len = 4;
    model.output_shape_len = 4;
    model.input_shape[0] = 1;
    model.input_shape[1] = 4;
    model.input_shape[2] = 3072;
    model.input_shape[3] = 256;
    model.output_shape[0] = 1;
    model.output_shape[1] = 4;
    model.output_shape[2] = 3072;
    model.output_shape[3] = 256;

    ASSERT(mdx_model_inspect(&info, &config, &model));
    ASSERT(config.dim_f == 3072);
    ASSERT(config.dim_t == 256);
    ASSERT(info.input_name == model.input_name);
    ASSERT(info.output_name == model.output_name);
    ASSERT(!info.input_shape_dynamic);
    ASSERT(!info.output_shape_dynamic);
    ASSERT(mdx_config_prepare(&config));
    ASSERT(config.chunk_size == 261120);
    ASSERT(config.trim == 3072);
    ASSERT(config.gen_size == 254976);

    config.dim_f = 2048;
    ASSERT_SILENT_FAILURE(mdx_model_inspect(&info, &config, &model));

    mdx_config_init(&config);
    model.input_shape[2] = -1;
    ASSERT(mdx_model_inspect(&info, &config, &model));
    ASSERT(config.dim_f == 3072);
    ASSERT(info.input_shape_dynamic);

    mdx_config_init(&config);
    model.output_shape[2] = -1;
    ASSERT_SILENT_FAILURE(mdx_model_inspect(&info, &config, &model));

    model.input_shape_len = 2;
    ASSERT_SILENT_FAILURE(mdx_model_inspect(&info, &config, &model));

    mdx_config_init(&config);
    config.dim_f = 3074;
    config.dim_t = 256;
    ASSERT_SILENT_FAILURE(mdx_config_prepare(&config));

    mdx_config_init(&config);
    config.dim_f = 3072;
    config.dim_t = 4;
    ASSERT_SILENT_FAILURE(mdx_config_prepare(&config));

    mdx_config_init(&config);
    ASSERT(mdx_input_tensor_len(&config) < 0);
    config.n_fft = 8;
    config.hop = 4;
    config.dim_f = 3;
    config.dim_t = 4;
    ASSERT(mdx_config_prepare(&config));
    ASSERT(mdx_input_tensor_len(&config) == 48);

    stft_plan_init_empty(&stft_plan);
    ASSERT(stft_plan_init(&stft_plan, config.n_fft, config.hop));
    for (int32 i = 0; i < config.chunk_size; i += 1) {
        left[i] = (float)(i + 1);
        right[i] = (float)(config.chunk_size - i);
    }
    ASSERT(!mdx_pack_input(&config,
                           &stft_plan,
                           left,
                           right,
                           config.chunk_size - 1,
                           tensor,
                           mdx_input_tensor_len(&config)));
    ASSERT(!mdx_pack_input(&config,
                           &stft_plan,
                           left,
                           right,
                           config.chunk_size,
                           too_small,
                           mdx_input_tensor_len(&config) - 1));
    ASSERT(mdx_pack_input(&config,
                          &stft_plan,
                          left,
                          right,
                          config.chunk_size,
                          tensor,
                          mdx_input_tensor_len(&config)));
    ASSERT(stft_forward_channel(&stft_plan,
                                left,
                                config.chunk_size,
                                left_real,
                                left_imag,
                                config.dim_t));
    ASSERT(stft_forward_channel(&stft_plan,
                                right,
                                config.chunk_size,
                                right_real,
                                right_imag,
                                config.dim_t));

    channel_stride = (int64)config.dim_f*(int64)config.dim_t;
    for (int32 bin = 0; bin < config.dim_f; bin += 1) {
        for (int32 frame = 0; frame < config.dim_t; frame += 1) {
            int64 input_index = (int64)bin*(int64)config.dim_t + (int64)frame;
            int64 output_index = (int64)bin*(int64)config.dim_t + (int64)frame;

            ASSERT(mdx_float_close(tensor[output_index],
                                   left_real[input_index]));
            ASSERT(mdx_float_close(tensor[channel_stride + output_index],
                                   left_imag[input_index]));
            ASSERT(mdx_float_close(tensor[2*channel_stride + output_index],
                                   right_real[input_index]));
            ASSERT(mdx_float_close(tensor[3*channel_stride + output_index],
                                   right_imag[input_index]));
        }
    }

    for (int64 i = 0; i < mdx_input_tensor_len(&config); i += 1) {
        tensor[i] = 0.0f;
    }
    ASSERT(!mdx_unpack_output(&config,
                              &stft_plan,
                              tensor,
                              mdx_input_tensor_len(&config),
                              unpack_left,
                              unpack_right,
                              config.chunk_size - 1));
    ASSERT(!mdx_unpack_output(&config,
                              &stft_plan,
                              too_small,
                              mdx_input_tensor_len(&config) - 1,
                              unpack_left,
                              unpack_right,
                              config.chunk_size));
    ASSERT(mdx_unpack_output(&config,
                             &stft_plan,
                             tensor,
                             mdx_input_tensor_len(&config),
                             unpack_left,
                             unpack_right,
                             config.chunk_size));
    for (int32 i = 0; i < config.chunk_size; i += 1) {
        ASSERT(mdx_float_close(unpack_left[i], 0.0f));
        ASSERT(mdx_float_close(unpack_right[i], 0.0f));
    }

    stft_plan_destroy(&stft_plan);
    mdx_config_init(&config);
    config.n_fft = 8;
    config.hop = 4;
    config.dim_f = 5;
    config.dim_t = 4;
    ASSERT(mdx_config_prepare(&config));
    ASSERT(stft_plan_init(&stft_plan, config.n_fft, config.hop));
    for (int32 i = 0; i < config.chunk_size; i += 1) {
        left[i] = (float)i/8.0f - 0.5f;
        right[i] = 0.25f - (float)i/16.0f;
    }
    ASSERT(mdx_pack_input(&config,
                          &stft_plan,
                          left,
                          right,
                          config.chunk_size,
                          tensor,
                          mdx_input_tensor_len(&config)));
    ASSERT(mdx_unpack_output(&config,
                             &stft_plan,
                             tensor,
                             mdx_input_tensor_len(&config),
                             unpack_left,
                             unpack_right,
                             config.chunk_size));
    for (int32 i = 0; i < config.chunk_size; i += 1) {
        ASSERT(mdx_float_close(unpack_left[i], left[i]));
        ASSERT(mdx_float_close(unpack_right[i], right[i]));
    }

    empty_input.sample_rate = config.sample_rate;
    empty_input.channel_count = config.channel_count;
    ASSERT(!mdx_process_song(NULL,
                             &stft_plan,
                             &context,
                             &model,
                             &empty_input,
                             &empty_output));
    ASSERT(mdx_process_song(&config,
                            &stft_plan,
                            &context,
                            &model,
                            &empty_input,
                            &empty_output));
    ASSERT(empty_output.sample_rate == config.sample_rate);
    ASSERT(empty_output.channel_count == config.channel_count);
    ASSERT(empty_output.frame_count == 0);

    audio_buffer_destroy(&empty_output);
    audio_buffer_destroy(&empty_input);
    stft_plan_destroy(&stft_plan);

    exit(EXIT_SUCCESS);
}

#endif /* TESTING_mdx */
