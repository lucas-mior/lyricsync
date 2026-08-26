#if !defined(AUDIO_FORMATS_H)
#define AUDIO_FORMATS_H

#include "cbase.h"

#define LRC_AUDIO_FORMAT_DEFAULT "wav"
#define LRC_AUDIO_FORMAT_DEFAULT_ENUM LRC_AUDIO_FORMAT_WAV
#define LRC_AUDIO_FORMAT_NAMES "wav|flac|mp3|opus"

#define LRC_AUDIO_FORMAT_VALUES(XX) \
    XX(LRC_AUDIO_FORMAT_WAV,  "wav",  "wav",  true,  true)  \
    XX(LRC_AUDIO_FORMAT_FLAC, "flac", "flac", true,  true)  \
    XX(LRC_AUDIO_FORMAT_MP3,  "mp3",  "mp3",  true,  false) \
    XX(LRC_AUDIO_FORMAT_OPUS, "opus", "opus", true,  false)

#define ENUM_NAME LrcAudioFormat
#define ENUM_BITFLAGS 0
#define ENUM_PREFIX_ LRC_AUDIO_FORMAT_
#define LRC_AUDIO_FORMAT_ENUM_FIELD(e, name, extension, streaming, lossless) \
    XX(e)
#define ENUM_FIELDS LRC_AUDIO_FORMAT_VALUES(LRC_AUDIO_FORMAT_ENUM_FIELD)
#define XENUMS_NO_TESTS 1
#include "cbase/xenums.c"
#undef XENUMS_NO_TESTS
#undef LRC_AUDIO_FORMAT_ENUM_FIELD

typedef struct LrcAudioFormatInfo {
    enum LrcAudioFormat format;
    char *name;
    char *extension;

    bool supports_streaming;
    bool lossless;
} LrcAudioFormatInfo;

static LrcAudioFormatInfo lrc_audio_format_infos[] = {
#define LRC_AUDIO_FORMAT_INFO_ENTRY(e, name, extension, streaming, lossless) \
    {e, name, extension, streaming, lossless},
    LRC_AUDIO_FORMAT_VALUES(LRC_AUDIO_FORMAT_INFO_ENTRY)
#undef LRC_AUDIO_FORMAT_INFO_ENTRY
};

static LrcAudioFormatInfo *
lrc_audio_format_info_from_name(char *value) {
    if (value == NULL) {
        return NULL;
    }

    for (int32 i = 0; i < LENGTH(lrc_audio_format_infos); i += 1) {
        if (strequal(value, lrc_audio_format_infos[i].name)) {
            return &lrc_audio_format_infos[i];
        }
    }

    return NULL;
}

static LrcAudioFormatInfo *
lrc_audio_format_info_from_extension(char *value) {
    if (value == NULL) {
        return NULL;
    }

    for (int32 i = 0; i < LENGTH(lrc_audio_format_infos); i += 1) {
        if (strequal(value, lrc_audio_format_infos[i].extension)) {
            return &lrc_audio_format_infos[i];
        }
    }

    return NULL;
}

static bool
lrc_audio_format_parse(char *value, enum LrcAudioFormat *format) {
    LrcAudioFormatInfo *info;

    if (format == NULL) {
        return false;
    }

    info = lrc_audio_format_info_from_name(value);
    if (info == NULL) {
        return false;
    }

    *format = info->format;
    return true;
}

static bool
lrc_audio_format_valid(char *value) {
    return lrc_audio_format_info_from_name(value) != NULL;
}

static bool
lrc_audio_format_is(char *value, enum LrcAudioFormat format) {
    enum LrcAudioFormat parsed;

    if (!lrc_audio_format_parse(value, &parsed)) {
        return false;
    }

    return parsed == format;
}

#endif /* AUDIO_FORMATS_H */
