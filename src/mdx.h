#if !defined(MDX_H)
#define MDX_H

#include "cbase.h"
#include "audio.h"
#include "ort.h"
#include "stft.h"
#include "progress.h"

#define MDX_MODEL_OUTPUT_NAMES "vocals|instrumental"

#define MDX_MODEL_OUTPUT_VALUES(XX) \
    XX(MDX_MODEL_OUTPUT_VOCALS, "vocals") \
    XX(MDX_MODEL_OUTPUT_INSTRUMENTAL, "instrumental")

#define ENUM_NAME MdxModelOutput
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ MDX_MODEL_OUTPUT_
#define MDX_MODEL_OUTPUT_ENUM_FIELD(e, name) XX(e)
#define ENUM_FIELDS MDX_MODEL_OUTPUT_VALUES(MDX_MODEL_OUTPUT_ENUM_FIELD)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS
#undef MDX_MODEL_OUTPUT_ENUM_FIELD

#define MDX_CLIP_MODE_NAMES "clamp|none"

#define MDX_CLIP_MODE_VALUES(XX) \
    XX(MDX_CLIP_MODE_CLAMP, "clamp") \
    XX(MDX_CLIP_MODE_NONE, "none")

#define ENUM_NAME MdxClipMode
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ MDX_CLIP_MODE_
#define MDX_CLIP_MODE_ENUM_FIELD(e, name) XX(e)
#define ENUM_FIELDS MDX_CLIP_MODE_VALUES(MDX_CLIP_MODE_ENUM_FIELD)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS
#undef MDX_CLIP_MODE_ENUM_FIELD

typedef struct MdxConfig {
    int32 sample_rate;
    int32 channel_count;
    int32 dim_c;
    int32 n_fft;
    int32 hop;
    int32 dim_f;
    int32 dim_t;
    int32 chunk_seconds;
    int32 margin_seconds;

    int32 chunk_size;
    int32 trim;
    int32 gen_size;

    float compensate;
    bool denoise;

    enum MdxModelOutput model_output;
    enum MdxClipMode clip_mode;
} MdxConfig;

typedef struct MdxModelInfo {
    char *input_name;
    char *output_name;

    int32 batch_size;
    int32 channel_count;
    int32 dim_f;
    int32 dim_t;

    bool input_shape_dynamic;
    bool output_shape_dynamic;
} MdxModelInfo;

static void mdx_config_init(MdxConfig *config);
static bool mdx_config_prepare(MdxConfig *config);
static int64 mdx_input_tensor_len(MdxConfig *config);
static bool mdx_pack_input(
    MdxConfig *config,
    StftPlan *stft_plan,
    float *left,
    float *right,
    int64 frame_count,
    float *tensor,
    int64 tensor_len
);
static bool mdx_unpack_output(
    MdxConfig *config,
    StftPlan *stft_plan,
    float *tensor,
    int64 tensor_len,
    float *left,
    float *right,
    int64 frame_count
);
static bool mdx_process_song_with_progress(
    MdxConfig *config,
    StftPlan *stft_plan,
    OrtContext *ort_context,
    OrtModel *ort_model,
    AudioBuffer *input,
    AudioBuffer *output,
    bool print_progress
);
static void mdx_model_info_init_empty(MdxModelInfo *info);
static bool mdx_model_inspect(
    MdxModelInfo *info,
    MdxConfig *config,
    OrtModel *model
);

#endif /* MDX_H */
