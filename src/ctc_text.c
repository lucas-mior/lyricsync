#include "cbase.h"
#include "lyricsync.h"
#include "ctc_text.h"
#include "lyrics.h"
#include "unicode_norm.h"

#if !defined(TESTING_ctc_text)
#define TESTING_ctc_text 0
#endif


static void
lrc_lyrics_normalized_destroy(LrcLyricsNormalized *normalized) {
    if (normalized == NULL) {
        return;
    }

    ARRAY_FREE(normalized->text);
    ARRAY_FREE(normalized->target_text);
    ARRAY_FREE(normalized->bytes);
    ARRAY_FREE(normalized->target_bytes);
    ARRAY_FREE(normalized->lines);
    ARRAY_FREE(normalized->segments);

    memset64(normalized, 0, SIZEOF(*normalized));

    return;
}

static bool
lrc_lyrics_normalized_alloc_lines(
    LrcLyricsNormalized *normalized,
    int32 line_count
) {
    if (line_count <= 0) {
        return true;
    }

    ARRAY_INIT_COUNT(normalized->lines, line_count);
    normalized->line_count = line_count;

    for (int32 i = 0; i < line_count; i += 1) {
        normalized->lines[i].kind = LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK;
        normalized->lines[i].normalized_start = -1;
        normalized->lines[i].normalized_end = -1;
    }

    return true;
}

static bool
lrc_lyrics_normalized_reserve_segments(
    LrcLyricsNormalized *normalized,
    int32 extra_segments
) {
    int64 needed;

    if (extra_segments <= 0) {
        return true;
    }

    needed = (int64)normalized->segment_count + extra_segments;
    if (needed > INT32_MAX) {
        return false;
    }

    return ARRAY_RESERVE(normalized->segments, (int32)needed);
}

static bool
lrc_lyrics_normalized_append_segment(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 source_start,
    int32 source_end,
    int32 normalized_start,
    int32 normalized_end,
    int32 target_start,
    int32 target_end
) {
    CtcTextSegment *segment;

    if (source_end <= source_start) {
        return true;
    }
    if (normalized_end < normalized_start) {
        return true;
    }
    if (target_start < 0) {
        target_start = normalized_start;
    }
    if (target_end < 0) {
        target_end = normalized_end;
    }
    if (target_end < target_start) {
        return true;
    }
    if (!lrc_lyrics_normalized_reserve_segments(normalized, 1)) {
        return false;
    }

    segment = &normalized->segments[normalized->segment_count];
    normalized->segment_count += 1;
    ARRAY_SET_COUNT(normalized->segments, normalized->segment_count);

    segment->line_index = line_index;
    segment->source_start = source_start;
    segment->source_end = source_end;
    segment->normalized_start = normalized_start;
    segment->normalized_end = normalized_end;
    segment->target_start = target_start;
    segment->target_end = target_end;

    return true;
}

typedef void CtcTextMappedWriterFill(void *, int32, int32, int32);

typedef struct CtcTextMappedWriter {
    char **text;
    void **maps;

    int32 *text_len;
    int32 *map_count;
    int64 map_size;

    CtcTextMappedWriterFill *fill;
} CtcTextMappedWriter;

static void
ctc_text_normalized_byte_fill(
    void *map_ptr,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    LrcLyricsNormalizedByte *map = map_ptr;

    map->line_index = line_index;
    map->source_start = source_start;
    map->source_end = source_end;

    return;
}

static void
ctc_text_target_byte_fill(
    void *map_ptr,
    int32 line_index,
    int32 normalized_start,
    int32 normalized_end
) {
    LrcLyricsTargetByte *map = map_ptr;

    map->line_index = line_index;
    map->normalized_start = normalized_start;
    map->normalized_end = normalized_end;

    return;
}

static void
ctc_text_normalized_writer(
    CtcTextMappedWriter *writer,
    LrcLyricsNormalized *normalized
) {
    memset64(writer, 0, SIZEOF(*writer));

    writer->text = &normalized->text;
    writer->maps = (void **)&normalized->bytes;
    writer->text_len = &normalized->text_len;
    writer->map_count = &normalized->byte_count;
    writer->map_size = SIZEOF(*normalized->bytes);
    writer->fill = ctc_text_normalized_byte_fill;

    return;
}

static void
ctc_text_target_writer(
    CtcTextMappedWriter *writer,
    LrcLyricsNormalized *normalized
) {
    memset64(writer, 0, SIZEOF(*writer));

    writer->text = &normalized->target_text;
    writer->maps = (void **)&normalized->target_bytes;
    writer->text_len = &normalized->target_text_len;
    writer->map_count = &normalized->target_byte_count;
    writer->map_size = SIZEOF(*normalized->target_bytes);
    writer->fill = ctc_text_target_byte_fill;

    return;
}

static bool
ctc_text_mapped_writer_reserve(
    CtcTextMappedWriter *writer,
    int32 extra_bytes
) {
    int64 needed;

    if (extra_bytes <= 0) {
        return true;
    }

    needed = (int64)*writer->text_len + extra_bytes;
    if (needed >= INT32_MAX) {
        return false;
    }
    if (!generic_array_reserve((void **)writer->text,
                               (int32)needed + 1,
                               SIZEOF((*writer->text)[0]))) {
        return false;
    }
    if (!generic_array_reserve(writer->maps, (int32)needed, writer->map_size)) {
        return false;
    }

    return true;
}

static bool
ctc_text_mapped_writer_append_bytes(
    CtcTextMappedWriter *writer,
    char *bytes,
    int32 bytes_len,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    char *map_bytes;

    if (bytes_len < 0) {
        return false;
    }
    if (bytes_len == 0) {
        return true;
    }
    if (bytes == NULL) {
        return false;
    }
    if (!ctc_text_mapped_writer_reserve(writer, bytes_len)) {
        return false;
    }

    memcpy64(*writer->text + *writer->text_len, bytes, bytes_len);
    map_bytes = *writer->maps;
    for (int32 i = 0; i < bytes_len; i += 1) {
        void *map = map_bytes + ((int64)*writer->map_count)*writer->map_size;

        *writer->map_count += 1;
        writer->fill(map, line_index, source_start, source_end);
    }
    *writer->text_len += bytes_len;
    (*writer->text)[*writer->text_len] = '\0';
    generic_array_set_count(*writer->maps, *writer->map_count);
    generic_array_set_count(*writer->text, *writer->text_len + 1);

    return true;
}

static bool
ctc_text_mapped_writer_append_space(
    CtcTextMappedWriter *writer,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    char space;

    if (*writer->text_len <= 0) {
        return true;
    }
    if ((*writer->text)[*writer->text_len - 1] == ' ') {
        return true;
    }

    space = ' ';
    return ctc_text_mapped_writer_append_bytes(writer,
                                               &space,
                                               1,
                                               line_index,
                                               source_start,
                                               source_end);
}

static void
ctc_text_mapped_writer_remove_trailing_space(CtcTextMappedWriter *writer) {
    if ((*writer->text_len <= 0)
        || ((*writer->text)[*writer->text_len - 1] != ' ')) {
        return;
    }

    *writer->text_len -= 1;
    *writer->map_count -= 1;
    (*writer->text)[*writer->text_len] = '\0';
    generic_array_set_count(*writer->maps, *writer->map_count);
    generic_array_set_count(*writer->text, *writer->text_len + 1);

    return;
}

static bool
lrc_lyrics_normalized_append_bytes(
    LrcLyricsNormalized *normalized,
    char *bytes,
    int32 bytes_len,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    CtcTextMappedWriter writer;

    ctc_text_normalized_writer(&writer, normalized);
    return ctc_text_mapped_writer_append_bytes(&writer,
                                               bytes,
                                               bytes_len,
                                               line_index,
                                               source_start,
                                               source_end);
}

static bool
lrc_lyrics_normalized_append_space(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    CtcTextMappedWriter writer;

    ctc_text_normalized_writer(&writer, normalized);
    return ctc_text_mapped_writer_append_space(&writer,
                                               line_index,
                                               source_start,
                                               source_end);
}

static bool
lrc_lyrics_normalized_append_char(
    LrcLyricsNormalized *normalized,
    char c,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    return lrc_lyrics_normalized_append_bytes(normalized,
                                              &c,
                                              1,
                                              line_index,
                                              source_start,
                                              source_end);
}

static bool
lrc_lyrics_normalized_append_target_bytes(
    LrcLyricsNormalized *normalized,
    char *bytes,
    int32 bytes_len,
    int32 line_index,
    int32 normalized_start,
    int32 normalized_end
) {
    CtcTextMappedWriter writer;

    ctc_text_target_writer(&writer, normalized);
    return ctc_text_mapped_writer_append_bytes(&writer,
                                               bytes,
                                               bytes_len,
                                               line_index,
                                               normalized_start,
                                               normalized_end);
}

static bool
lrc_lyrics_normalized_append_target_space(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 normalized_start,
    int32 normalized_end
) {
    CtcTextMappedWriter writer;

    ctc_text_target_writer(&writer, normalized);
    return ctc_text_mapped_writer_append_space(&writer,
                                               line_index,
                                               normalized_start,
                                               normalized_end);
}

static bool
lrc_lyrics_normalized_append_target_from_normalized(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 normalized_start,
    int32 normalized_end,
    int32 *target_start_out,
    int32 *target_end_out
) {
    int32 target_start;

    if (target_start_out) {
        *target_start_out = -1;
    }
    if (target_end_out) {
        *target_end_out = -1;
    }
    if ((normalized_start < 0) || (normalized_end <= normalized_start)) {
        return false;
    }

    if (!lrc_lyrics_normalized_append_target_space(normalized,
                                                   line_index,
                                                   normalized_start,
                                                   normalized_start)) {
        return false;
    }

    target_start = normalized->target_text_len;
    if (target_start_out) {
        *target_start_out = target_start;
    }
    for (int32 i = normalized_start; i < normalized_end;) {
        uint32 rune;
        int32 step;

        step = utf8_decode_raw(normalized->text + i,
                               &rune,
                               normalized_end - i);
        if (step <= 0) {
            return false;
        }
        if (normalized->target_text_len > target_start) {
            if (!lrc_lyrics_normalized_append_target_space(normalized,
                                                           line_index,
                                                           i,
                                                           i)) {
                return false;
            }
        }
        if (!lrc_lyrics_normalized_append_target_bytes(normalized,
                                                       normalized->text + i,
                                                       step,
                                                       line_index,
                                                       i,
                                                       i + step)) {
            return false;
        }
        i += step;
    }

    if (target_end_out) {
        *target_end_out = normalized->target_text_len;
    }

    return true;
}

static bool
lrc_lyrics_ascii_space(char c) {
    if (c == ' ') {
        return true;
    }
    if (c == '\t') {
        return true;
    }
    if (c == '\n') {
        return true;
    }
    if (c == '\v') {
        return true;
    }
    if (c == '\f') {
        return true;
    }

    return false;
}

static int32
lrc_lyrics_line_trim_start(LrcLyricsLine *line) {
    int32 start = line->text_start;

    while ((start < line->text_end)
           && lrc_lyrics_ascii_space(line->text[start - line->text_start])) {
        start += 1;
    }

    return start;
}

static int32
lrc_lyrics_line_trim_end(LrcLyricsLine *line, int32 start) {
    int32 end = line->text_end;

    while ((end > start)
           && lrc_lyrics_ascii_space(line->text[end - line->text_start - 1])) {
        end -= 1;
    }

    return end;
}

static bool
lrc_lyrics_line_is_section_marker(
    LrcLyrics *lyrics,
    int32 start,
    int32 end
) {
    char first;
    char last;

    if ((end - start) < 2) {
        return false;
    }

    first = lyrics->text[start];
    last = lyrics->text[end - 1];
    if (((first == '[') || (first == '('))
        && ((last == ']') || (last == ')'))) {
        return true;
    }

    return false;
}

static bool
lrc_lyrics_ascii_alnum(char c) {
    return isalnum((uint8)c);
}

static char
lrc_lyrics_ascii_lower(char c) {
    if ((c >= 'A') && (c <= 'Z')) {
        return (char)(c - 'A' + 'a');
    }

    return c;
}

static bool
ctc_text_is_unicode_space(uint32 rune) {
    if (rune == ' ') {
        return true;
    }
    if ((rune >= 0x0009) && (rune <= 0x000D)) {
        return true;
    }
    if (rune == 0x0085) {
        return true;
    }
    if (rune == 0x00A0) {
        return true;
    }
    if (rune == 0x1680) {
        return true;
    }
    if ((rune >= 0x2000) && (rune <= 0x200A)) {
        return true;
    }
    if (rune == 0x2028) {
        return true;
    }
    if (rune == 0x2029) {
        return true;
    }
    if (rune == 0x202F) {
        return true;
    }
    if (rune == 0x205F) {
        return true;
    }
    if (rune == 0x3000) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_digit(uint32 rune) {
    if ((rune >= '0') && (rune <= '9')) {
        return true;
    }
    if ((rune >= 0x09E6) && (rune <= 0x09EF)) {
        return true;
    }
    if ((rune >= 0x17E0) && (rune <= 0x17E9)) {
        return true;
    }
    if ((rune >= 0x0966) && (rune <= 0x096F)) {
        return true;
    }
    if ((rune >= 0x0B66) && (rune <= 0x0B6F)) {
        return true;
    }
    if ((rune >= 0x06F0) && (rune <= 0x06F9)) {
        return true;
    }
    if ((rune >= 0xA900) && (rune <= 0xA909)) {
        return true;
    }
    if ((rune >= 0xFF10) && (rune <= 0xFF19)) {
        return true;
    }
    if ((rune >= 0x0D66) && (rune <= 0x0D6F)) {
        return true;
    }
    if ((rune >= 0x1040) && (rune <= 0x1049)) {
        return true;
    }
    if ((rune >= 0x2170) && (rune <= 0x2179)) {
        return true;
    }
    if (rune == 0x206F) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_delete(uint32 rune) {
    if (rune == 0x200B) {
        return true;
    }
    if (rune == 0x200C) {
        return true;
    }
    if (rune == 0x200E) {
        return true;
    }
    if (rune == 0x200F) {
        return true;
    }
    if (rune == 0x202A) {
        return true;
    }
    if (rune == 0x202C) {
        return true;
    }
    if ((rune >= 0x064B) && (rune <= 0x0652)) {
        return true;
    }
    if ((rune >= 0x0656) && (rune <= 0x0657)) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_single_quote(uint32 rune) {
    if (rune == 0x2018) {
        return true;
    }
    if (rune == 0x2019) {
        return true;
    }
    if (rune == 0x201B) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_quote_punc(uint32 rune) {
    if (rune == '"') {
        return true;
    }
    if ((rune >= 0x201C) && (rune <= 0x201F)) {
        return true;
    }
    if ((rune >= 0x2039) && (rune <= 0x203A)) {
        return true;
    }
    if (rune == 0x00AB) {
        return true;
    }
    if (rune == 0x00BB) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_chinese_punc(uint32 rune) {
    if ((rune >= 0x3002) && (rune <= 0x300F)) {
        return true;
    }
    if ((rune >= 0x3008) && (rune <= 0x300B)) {
        return true;
    }
    if (rune == 0x2027) {
        return true;
    }
    if (rune == 0x22EF) {
        return true;
    }
    if (rune == 0x2500) {
        return true;
    }
    if ((rune >= 0xFF01) && (rune <= 0xFF0F)) {
        return true;
    }
    if ((rune >= 0xFF1A) && (rune <= 0xFF1B)) {
        return true;
    }
    if ((rune >= 0xFF1F) && (rune <= 0xFF20)) {
        return true;
    }
    if ((rune >= 0xFF3B) && (rune <= 0xFF3F)) {
        return true;
    }
    if ((rune >= 0xFF5B) && (rune <= 0xFF5E)) {
        return true;
    }
    if (rune == 0xFE4F) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_armenian_punc(uint32 rune) {
    if ((rune >= 0x055A) && (rune <= 0x055F)) {
        return true;
    }
    if (rune == 0x0589) {
        return true;
    }

    return false;
}

static bool
ctc_text_is_reference_punctuation(uint32 rune) {
    if ((rune == '.') || (rune == '?') || (rune == ',') || (rune == ':')) {
        return true;
    }
    if ((rune == '!') || (rune == '{') || (rune == '}') || (rune == ';')) {
        return true;
    }
    if ((rune == '(') || (rune == ')') || (rune == '[') || (rune == ']')) {
        return true;
    }
    if ((rune == '<') || (rune == '>') || (rune == '/')) {
        return true;
    }
    if ((rune == '-') || (rune == '_') || (rune == '\\') || (rune == '|')) {
        return true;
    }
    if ((rune == 0x00A1) || (rune == 0x00BF)) {
        return true;
    }
    if ((rune == 0x060C) || (rune == 0x061B) || (rune == 0x061F)) {
        return true;
    }
    if (rune == 0x0964) {
        return true;
    }
    if (ctc_text_is_reference_single_quote(rune)) {
        return true;
    }
    if (ctc_text_is_reference_quote_punc(rune)) {
        return true;
    }
    if (ctc_text_is_reference_chinese_punc(rune)) {
        return true;
    }
    if (ctc_text_is_reference_armenian_punc(rune)) {
        return true;
    }
    if ((rune >= 0x2000) && (rune <= 0x206F)) {
        return true;
    }

    return false;
}

static bool
ctc_text_reference_output_reserve(
    CtcUnicodeNormResult *result,
    int32 extra_bytes
) {
    int64 needed;

    if (extra_bytes <= 0) {
        return true;
    }
    if (result == NULL) {
        return false;
    }

    needed = (int64)result->text.len + extra_bytes;
    if (needed >= INT32_MAX) {
        return false;
    }

    sb_reserve(&result->text, extra_bytes);

    return true;
}

static bool
ctc_text_reference_output_append_bytes(
    CtcUnicodeNormResult *result,
    char *text,
    int32 text_len
) {
    if (text_len < 0) {
        return false;
    }
    if (text_len == 0) {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    if (!ctc_text_reference_output_reserve(result, text_len)) {
        return false;
    }

    sb_append(&result->text, text, text_len);

    return true;
}

static bool
ctc_text_reference_output_append_rune(
    CtcUnicodeNormResult *result,
    uint32 rune
) {
    char encoded[4];
    int32 encoded_len;

    encoded_len = utf8_encode(rune, encoded, SIZEOF(encoded));
    if (encoded_len <= 0) {
        return false;
    }

    return ctc_text_reference_output_append_bytes(result,
                                                 encoded,
                                                 encoded_len);
}

static bool
ctc_text_reference_output_append_space(CtcUnicodeNormResult *result) {
    char space;

    if (result->text.len <= 0) {
        return true;
    }
    if (result->text.data[result->text.len - 1] == ' ') {
        return true;
    }

    space = ' ';
    return ctc_text_reference_output_append_bytes(result, &space, 1);
}

static void
ctc_text_reference_output_trim_spaces(CtcUnicodeNormResult *result) {
    while ((result->text.len > 0) && (result->text.data[0] == ' ')) {
        memmove64(result->text.data,
                  result->text.data + 1,
                  result->text.len);
        result->text.len -= 1;
        result->text.data[result->text.len] = '\0';
    }
    while ((result->text.len > 0)
           && (result->text.data[result->text.len - 1] == ' ')) {
        result->text.len -= 1;
        result->text.data[result->text.len] = '\0';
    }

    return;
}

typedef struct CtcTextUtf8Rune {
    char *text;

    int32 text_len;
    int32 byte_index;
    int32 byte_end;

    uint32 rune;
    uint32 previous_rune;
    uint32 next_rune;
} CtcTextUtf8Rune;

typedef struct CtcTextUtf8Transform {
    CtcUnicodeNormResult *result;
    void *context;
    bool trim_spaces;
} CtcTextUtf8Transform;

typedef bool CtcTextUtf8TransformRuneFn(
    CtcTextUtf8Transform *,
    CtcTextUtf8Rune *,
    int32 *,
    uint32 *
);
typedef bool CtcTextUtf8TransformFinishFn(CtcTextUtf8Transform *);

static bool
ctc_text_utf8_transform_run(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result,
    CtcTextUtf8TransformRuneFn *rune_fn,
    CtcTextUtf8TransformFinishFn *finish_fn,
    void *context,
    bool trim_spaces
) {
    CtcTextUtf8Transform transform;
    uint32 previous_rune;

    ctc_unicode_norm_result_destroy(result);
    memset64(&transform, 0, SIZEOF(transform));
    transform.result = result;
    transform.context = context;
    transform.trim_spaces = trim_spaces;
    previous_rune = 0;

    for (int32 i = 0; i < text_len;) {
        CtcTextUtf8Rune rune;
        uint32 next_previous_rune;
        int32 next_index;
        int32 step;

        step = utf8_decode_raw(text + i, &rune.rune, text_len - i);
        if (step <= 0) {
            return false;
        }

        rune.text = text;
        rune.text_len = text_len;
        rune.byte_index = i;
        rune.byte_end = i + step;
        rune.previous_rune = previous_rune;
        rune.next_rune = 0;

        if ((i + step) < text_len) {
            int32 next_step;

            next_step = utf8_decode_raw(text + i + step,
                                        &rune.next_rune,
                                        text_len - i - step);
            if (next_step <= 0) {
                return false;
            }
        }

        next_index = i + step;
        next_previous_rune = rune.rune;
        if (!rune_fn(&transform, &rune, &next_index, &next_previous_rune)) {
            return false;
        }
        if ((next_index <= i) || (next_index > text_len)) {
            return false;
        }

        previous_rune = next_previous_rune;
        i = next_index;
    }

    if (finish_fn) {
        if (!finish_fn(&transform)) {
            return false;
        }
    }
    if (transform.trim_spaces) {
        ctc_text_reference_output_trim_spaces(result);
    }

    return true;
}

static bool
ctc_text_reference_has_space_before(
    char *text,
    int32 byte_index
) {
    uint32 rune;
    int32 step;
    int32 previous = -1;

    for (int32 i = 0; i < byte_index;) {
        previous = i;
        step = utf8_decode_raw(text + i, &rune, byte_index - i);
        if (step <= 0) {
            return false;
        }
        i += step;
    }
    if (previous < 0) {
        return false;
    }

    step = utf8_decode_raw(text + previous, &rune, byte_index - previous);
    if (step <= 0) {
        return false;
    }

    return ctc_text_is_unicode_space(rune);
}

static bool
ctc_text_reference_has_space_after(
    char *text,
    int32 text_len,
    int32 byte_index
) {
    uint32 rune;
    int32 step;

    if (byte_index >= text_len) {
        return false;
    }

    step = utf8_decode_raw(text + byte_index, &rune, text_len - byte_index);
    if (step <= 0) {
        return false;
    }

    return ctc_text_is_unicode_space(rune);
}

static bool
ctc_text_reference_is_digit_run(
    char *text,
    int32 start,
    int32 end
) {
    for (int32 i = start; i < end;) {
        uint32 rune;
        int32 step;

        step = utf8_decode_raw(text + i, &rune, end - i);
        if (step <= 0) {
            return false;
        }
        if (!ctc_text_is_reference_digit(rune)) {
            return false;
        }
        i += step;
    }

    return start < end;
}

static bool
ctc_text_reference_should_remove_digit_run(
    char *text,
    int32 text_len,
    int32 start,
    int32 end
) {
    bool before;
    bool after;

    if (!ctc_text_reference_is_digit_run(text, start, end)) {
        return false;
    }

    before = ctc_text_reference_has_space_before(text, start);
    after = ctc_text_reference_has_space_after(text, text_len, end);
    if ((start == 0) && after) {
        return true;
    }
    if (before && after) {
        return true;
    }
    if (before && (end == text_len)) {
        return true;
    }

    return false;
}

static bool
ctc_text_reference_remove_parentheses_rune(
    CtcTextUtf8Transform *transform,
    CtcTextUtf8Rune *rune,
    int32 *next_index,
    uint32 *next_previous_rune
) {
    if (rune->rune == '(') {
        int32 close = -1;
        bool found_digit = false;

        for (int32 i = rune->byte_end; i < rune->text_len;) {
            uint32 inner_rune;
            int32 step;

            step = utf8_decode_raw(rune->text + i,
                                   &inner_rune,
                                   rune->text_len - i);
            if (step <= 0) {
                return false;
            }
            if (ctc_text_is_reference_digit(inner_rune)) {
                found_digit = true;
            }
            if (inner_rune == ')') {
                close = i + step;
                break;
            }
            i += step;
        }
        if ((close >= 0) && found_digit) {
            if (!ctc_text_reference_output_append_space(transform->result)) {
                return false;
            }
            *next_index = close;
            *next_previous_rune = 0;
            return true;
        }
    }

    *next_previous_rune = rune->rune;
    return ctc_text_reference_output_append_rune(transform->result,
                                                rune->rune);
}

static bool
ctc_text_reference_remove_number_parentheses(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    return ctc_text_utf8_transform_run(
        text,
        text_len,
        result,
        ctc_text_reference_remove_parentheses_rune,
        NULL,
        NULL,
        false
    );
}

static bool
ctc_text_reference_apply_mappings_rune(
    CtcTextUtf8Transform *transform,
    CtcTextUtf8Rune *rune,
    int32 *next_index,
    uint32 *next_previous_rune
) {
    uint32 mapped_rune;

    if (BEGINS_WITH(rune->text + rune->byte_index,
                    rune->text_len - rune->byte_index,
                    "&lt;")) {
        *next_index = rune->byte_index + 4;
        *next_previous_rune = 0;
        return true;
    }
    if (BEGINS_WITH(rune->text + rune->byte_index,
                    rune->text_len - rune->byte_index,
                    "&gt;")) {
        *next_index = rune->byte_index + 4;
        *next_previous_rune = 0;
        return true;
    }
    if (BEGINS_WITH(rune->text + rune->byte_index,
                    rune->text_len - rune->byte_index,
                    "&nbsp")) {
        *next_index = rune->byte_index + 5;
        *next_previous_rune = 0;
        return true;
    }

    mapped_rune = rune->rune;
    if ((mapped_rune >= 'A') && (mapped_rune <= 'Z')) {
        mapped_rune = (uint32)(mapped_rune - 'A' + 'a');
    }

    *next_previous_rune = mapped_rune;
    if (ctc_text_is_reference_single_quote(mapped_rune)
        && !ctc_text_is_unicode_space(rune->previous_rune)
        && !ctc_text_is_unicode_space(rune->next_rune)
        && (rune->previous_rune != 0)
        && (rune->next_rune != 0)) {
        return ctc_text_reference_output_append_bytes(transform->result,
                                                      STRLIT("'"));
    }

    return ctc_text_reference_output_append_rune(transform->result,
                                                mapped_rune);
}

static bool
ctc_text_reference_apply_mappings(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    return ctc_text_utf8_transform_run(text,
                                       text_len,
                                       result,
                                       ctc_text_reference_apply_mappings_rune,
                                       NULL,
                                       NULL,
                                       false);
}

static bool
ctc_text_reference_replace_punctuation_rune(
    CtcTextUtf8Transform *transform,
    CtcTextUtf8Rune *rune,
    int32 *next_index,
    uint32 *next_previous_rune
) {
    (void)next_index;

    if (ctc_text_is_reference_delete(rune->rune)) {
        *next_previous_rune = 0;
        return true;
    }
    if (ctc_text_is_reference_punctuation(rune->rune)) {
        *next_previous_rune = ' ';
        return ctc_text_reference_output_append_space(transform->result);
    }

    *next_previous_rune = rune->rune;
    return ctc_text_reference_output_append_rune(transform->result,
                                                rune->rune);
}

static bool
ctc_text_reference_replace_punctuation_delete(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    return ctc_text_utf8_transform_run(
        text,
        text_len,
        result,
        ctc_text_reference_replace_punctuation_rune,
        NULL,
        NULL,
        false
    );
}

typedef struct CtcTextDigitRunTransform {
    char *text;

    int32 text_len;
    int32 run_start;

    bool in_digit_run;
} CtcTextDigitRunTransform;

static bool
ctc_text_reference_remove_numbers_flush(
    CtcTextUtf8Transform *transform,
    int32 run_end
) {
    CtcTextDigitRunTransform *context = transform->context;

    if (!context->in_digit_run) {
        return true;
    }

    if (ctc_text_reference_should_remove_digit_run(context->text,
                                                   context->text_len,
                                                   context->run_start,
                                                   run_end)) {
        if (!ctc_text_reference_output_append_space(transform->result)) {
            return false;
        }
    } else if (!ctc_text_reference_output_append_bytes(transform->result,
                                                       context->text
                                                       + context->run_start,
                                                       run_end
                                                       - context->run_start)) {
        return false;
    }

    context->in_digit_run = false;
    context->run_start = -1;

    return true;
}

static bool
ctc_text_reference_remove_numbers_rune(
    CtcTextUtf8Transform *transform,
    CtcTextUtf8Rune *rune,
    int32 *next_index,
    uint32 *next_previous_rune
) {
    CtcTextDigitRunTransform *context;

    (void)next_index;
    context = transform->context;
    if (ctc_text_is_reference_digit(rune->rune)) {
        if (!context->in_digit_run) {
            context->run_start = rune->byte_index;
            context->in_digit_run = true;
        }
        *next_previous_rune = 0;
        return true;
    }

    if (!ctc_text_reference_remove_numbers_flush(transform, rune->byte_index)) {
        return false;
    }

    *next_previous_rune = rune->rune;
    return ctc_text_reference_output_append_rune(transform->result,
                                                rune->rune);
}

static bool
ctc_text_reference_remove_numbers_finish(CtcTextUtf8Transform *transform) {
    CtcTextDigitRunTransform *context = transform->context;

    return ctc_text_reference_remove_numbers_flush(transform,
                                                   context->text_len);
}

static bool
ctc_text_reference_remove_numbers(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    CtcTextDigitRunTransform context;

    memset64(&context, 0, SIZEOF(context));
    context.text = text;
    context.text_len = text_len;
    context.run_start = -1;

    return ctc_text_utf8_transform_run(text,
                                       text_len,
                                       result,
                                       ctc_text_reference_remove_numbers_rune,
                                       ctc_text_reference_remove_numbers_finish,
                                       &context,
                                       false);
}

static bool
ctc_text_reference_collapse_spaces_rune(
    CtcTextUtf8Transform *transform,
    CtcTextUtf8Rune *rune,
    int32 *next_index,
    uint32 *next_previous_rune
) {
    (void)next_index;

    if (ctc_text_is_unicode_space(rune->rune)) {
        *next_previous_rune = ' ';
        return ctc_text_reference_output_append_space(transform->result);
    }

    *next_previous_rune = rune->rune;
    return ctc_text_reference_output_append_rune(transform->result,
                                                rune->rune);
}

static bool
ctc_text_reference_collapse_spaces(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    return ctc_text_utf8_transform_run(text,
                                       text_len,
                                       result,
                                       ctc_text_reference_collapse_spaces_rune,
                                       NULL,
                                       NULL,
                                       true);
}

static bool
ctc_text_reference_normalize_uroman_rune(
    CtcTextUtf8Transform *transform,
    CtcTextUtf8Rune *rune,
    int32 *next_index,
    uint32 *next_previous_rune
) {
    uint32 mapped_rune;

    (void)next_index;
    mapped_rune = rune->rune;
    if ((mapped_rune >= 'A') && (mapped_rune <= 'Z')) {
        mapped_rune = (uint32)(mapped_rune - 'A' + 'a');
    }

    if (((mapped_rune >= 'a') && (mapped_rune <= 'z'))
        || (mapped_rune == '\'')) {
        *next_previous_rune = mapped_rune;
        return ctc_text_reference_output_append_rune(transform->result,
                                                    mapped_rune);
    }

    *next_previous_rune = ' ';
    return ctc_text_reference_output_append_space(transform->result);
}

static bool
ctc_text_reference_normalize_uroman(
    char *text,
    int32 text_len,
    CtcUnicodeNormResult *result
) {
    return ctc_text_utf8_transform_run(text,
                                       text_len,
                                       result,
                                       ctc_text_reference_normalize_uroman_rune,
                                       NULL,
                                       NULL,
                                       true);
}

static bool
ctc_text_reference_should_romanize(
    LrcLyricsPreprocessOptions *options
) {
    if (options == NULL) {
        return false;
    }
    if (options->romanization == LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU) {
        return true;
    }

    return false;
}

static bool
ctc_text_reference_normalize_word(
    char *text,
    int32 text_len,
    LrcLyricsPreprocessOptions *options,
    CtcUnicodeNormResult *result
) {
    CtcUnicodeNormResult stage1;
    CtcUnicodeNormResult stage2;
    CtcUnicodeNormResult stage3;
    CtcUnicodeNormResult stage4;
    CtcUnicodeNormResult stage5;
    CtcUnicodeNormResult stage6;
    bool ok;

    ctc_unicode_norm_result_init(&stage1);
    ctc_unicode_norm_result_init(&stage2);
    ctc_unicode_norm_result_init(&stage3);
    ctc_unicode_norm_result_init(&stage4);
    ctc_unicode_norm_result_init(&stage5);
    ctc_unicode_norm_result_init(&stage6);
    ctc_unicode_norm_result_destroy(result);

    ok = false;
    if (!ctc_unicode_norm_nfkc_lower(text, text_len, &stage1)) {
        goto done;
    }
    if (!ctc_text_reference_remove_number_parentheses(stage1.text.data,
                                                      stage1.text.len,
                                                      &stage2)) {
        goto done;
    }
    if (!ctc_text_reference_apply_mappings(stage2.text.data,
                                           stage2.text.len,
                                           &stage3)) {
        goto done;
    }
    if (!ctc_text_reference_replace_punctuation_delete(stage3.text.data,
                                                       stage3.text.len,
                                                       &stage4)) {
        goto done;
    }
    if (!ctc_text_reference_remove_numbers(stage4.text.data,
                                           stage4.text.len,
                                           &stage5)) {
        goto done;
    }
    if (!ctc_text_reference_collapse_spaces(stage5.text.data,
                                            stage5.text.len,
                                            result)) {
        goto done;
    }
    if (ctc_text_reference_should_romanize(options)) {
        if (result->text.len > 0) {
            if (!ctc_unicode_norm_transliterate_latin(result->text.data,
                                                      result->text.len,
                                                      &stage6)) {
                goto done;
            }
            if (!ctc_text_reference_normalize_uroman(stage6.text.data,
                                                     stage6.text.len,
                                                     result)) {
                goto done;
            }
        }
    }
    ok = true;

done:
    ctc_unicode_norm_result_destroy(&stage6);
    ctc_unicode_norm_result_destroy(&stage5);
    ctc_unicode_norm_result_destroy(&stage4);
    ctc_unicode_norm_result_destroy(&stage3);
    ctc_unicode_norm_result_destroy(&stage2);
    ctc_unicode_norm_result_destroy(&stage1);

    return ok;
}

typedef struct CtcTextSegmentBuild {
    bool active;
    bool has_text;

    int32 line_index;
    int32 source_start;
    int32 source_end;
    int32 normalized_start;
    int32 normalized_end;
} CtcTextSegmentBuild;

static void
ctc_text_segment_build_init(
    CtcTextSegmentBuild *segment
) {
    memset64(segment, 0, SIZEOF(*segment));

    segment->line_index = -1;
    segment->source_start = -1;
    segment->source_end = -1;
    segment->normalized_start = -1;
    segment->normalized_end = -1;

    return;
}

static void
ctc_text_segment_build_source(
    CtcTextSegmentBuild *segment,
    int32 line_index,
    int32 source_start,
    int32 source_end
) {
    if (!segment->active) {
        segment->active = true;
        segment->line_index = line_index;
        segment->source_start = source_start;
    }

    segment->source_end = source_end;

    return;
}

static void
ctc_text_segment_build_text(
    CtcTextSegmentBuild *segment,
    int32 normalized_start,
    int32 normalized_end
) {
    ASSERT(segment->active);

    if (!segment->has_text) {
        segment->has_text = true;
        segment->normalized_start = normalized_start;
    }

    segment->normalized_end = normalized_end;

    return;
}

static bool
ctc_text_segment_build_finish(
    LrcLyricsNormalized *normalized,
    CtcTextSegmentBuild *segment
) {
    bool ok;

    if (!segment->active) {
        return true;
    }
    if (!segment->has_text) {
        ctc_text_segment_build_init(segment);
        return true;
    }

    ok = lrc_lyrics_normalized_append_segment(normalized,
                                              segment->line_index,
                                              segment->source_start,
                                              segment->source_end,
                                              segment->normalized_start,
                                              segment->normalized_end,
                                              -1,
                                              -1);
    ctc_text_segment_build_init(segment);

    return ok;
}

static void
lrc_lyrics_normalized_line_finish(
    LrcLyricsNormalized *normalized,
    LrcLyricsNormalizedLine *line_range
) {
    if ((line_range->normalized_start >= 0)
        && (line_range->normalized_end > line_range->normalized_start)) {
        line_range->kind = LRC_LYRICS_NORMALIZED_LINE_KIND_ALIGNABLE;
        normalized->alignable_line_count += 1;
    } else {
        line_range->kind = LRC_LYRICS_NORMALIZED_LINE_KIND_PUNCTUATION_ONLY;
        line_range->normalized_start = -1;
        line_range->normalized_end = -1;
    }

    return;
}

static bool
lrc_lyrics_normalize_line(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 start,
    int32 end
) {
    LrcLyricsNormalizedLine *line_range = &normalized->lines[line_index];
    CtcTextSegmentBuild segment;
    bool wrote_line;

    ctc_text_segment_build_init(&segment);
    wrote_line = false;
    for (int32 i = start; i < end;) {
        uint32 rune;
        int32 step;

        step = utf8_decode_raw(lyrics->text + i, &rune, end - i);
        if (step <= 0) {
            return false;
        }

        if (rune < 0x80) {
            char c = lyrics->text[i];

            if (!lrc_lyrics_ascii_space(c)) {
                ctc_text_segment_build_source(&segment,
                                              line_index,
                                              i,
                                              i + step);
            }
            if (lrc_lyrics_ascii_alnum(c)) {
                c = lrc_lyrics_ascii_lower(c);
                if (!wrote_line && (normalized->text_len > 0)) {
                    if (!lrc_lyrics_normalized_append_space(normalized,
                                                            line_index,
                                                            i,
                                                            i + step)) {
                        return false;
                    }
                }
                if (line_range->normalized_start < 0) {
                    line_range->normalized_start = normalized->text_len;
                }
                ctc_text_segment_build_text(&segment,
                                             normalized->text_len,
                                             normalized->text_len);
                if (!lrc_lyrics_normalized_append_char(normalized,
                                                       c,
                                                       line_index,
                                                       i,
                                                       i + step)) {
                    return false;
                }
                ctc_text_segment_build_text(&segment,
                                             segment.normalized_start,
                                             normalized->text_len);
                line_range->normalized_end = normalized->text_len;
                wrote_line = true;
            } else if (lrc_lyrics_ascii_space(c)) {
                if (wrote_line) {
                    if (!ctc_text_segment_build_finish(normalized, &segment)) {
                        return false;
                    }
                    if (!lrc_lyrics_normalized_append_space(normalized,
                                                            line_index,
                                                            i,
                                                            i + step)) {
                        return false;
                    }
                    line_range->normalized_end = normalized->text_len;
                }
            }
        } else {
            ctc_text_segment_build_source(&segment,
                                          line_index,
                                          i,
                                          i + step);
            if (!wrote_line && (normalized->text_len > 0)) {
                if (!lrc_lyrics_normalized_append_space(normalized,
                                                        line_index,
                                                        i,
                                                        i + step)) {
                    return false;
                }
            }
            if (line_range->normalized_start < 0) {
                line_range->normalized_start = normalized->text_len;
            }
            ctc_text_segment_build_text(&segment,
                                         normalized->text_len,
                                         normalized->text_len);
            if (!lrc_lyrics_normalized_append_bytes(normalized,
                                                    lyrics->text + i,
                                                    step,
                                                    line_index,
                                                    i,
                                                    i + step)) {
                return false;
            }
            ctc_text_segment_build_text(&segment,
                                         segment.normalized_start,
                                         normalized->text_len);
            line_range->normalized_end = normalized->text_len;
            wrote_line = true;
        }

        i += step;
    }

    if (!ctc_text_segment_build_finish(normalized, &segment)) {
        return false;
    }

    if ((normalized->text_len > 0)
        && (normalized->text[normalized->text_len - 1] == ' ')) {
        CtcTextMappedWriter writer;

        ctc_text_normalized_writer(&writer, normalized);
        ctc_text_mapped_writer_remove_trailing_space(&writer);
        line_range->normalized_end = normalized->text_len;
    }

    lrc_lyrics_normalized_line_finish(normalized, line_range);

    return true;
}

static bool
lrc_lyrics_normalize_split_segment(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsNormalizedLine *line_range,
    LrcLyricsPreprocessOptions *options,
    int32 line_index,
    int32 source_start,
    int32 source_end,
    bool separate_from_previous,
    bool *wrote_line
) {
    CtcUnicodeNormResult chunk;
    int32 normalized_start;
    int32 normalized_end;
    int32 target_start;
    int32 target_end;
    bool ok;

    ctc_unicode_norm_result_init(&chunk);
    normalized_start = normalized->text_len;
    normalized_end = normalized_start;
    target_start = normalized->target_text_len;
    target_end = target_start;
    ok = false;

    if (!ctc_text_reference_normalize_word(lyrics->text + source_start,
                                           source_end - source_start,
                                           options,
                                           &chunk)) {
        goto done;
    }

    if (chunk.text.len > 0) {
        if (separate_from_previous && (normalized->text_len > 0)
            && !lrc_lyrics_normalized_append_space(normalized,
                                                   line_index,
                                                   source_start,
                                                   source_end)) {
            goto done;
        }
        if (line_range->normalized_start < 0) {
            line_range->normalized_start = normalized->text_len;
        }

        normalized_start = normalized->text_len;
        if (!lrc_lyrics_normalized_append_bytes(normalized,
                                                chunk.text.data,
                                                chunk.text.len,
                                                line_index,
                                                source_start,
                                                source_end)) {
            goto done;
        }
        normalized_end = normalized->text_len;

        if (!lrc_lyrics_normalized_append_target_from_normalized(
            normalized,
            line_index,
            normalized_start,
            normalized_end,
            &target_start,
            &target_end
        )) {
            goto done;
        }

        line_range->normalized_end = normalized_end;
        *wrote_line = true;
    }

    ok = lrc_lyrics_normalized_append_segment(normalized,
                                              line_index,
                                              source_start,
                                              source_end,
                                              normalized_start,
                                              normalized_end,
                                              target_start,
                                              target_end);

done:
    ctc_unicode_norm_result_destroy(&chunk);

    return ok;
}

static bool
lrc_lyrics_normalize_word_segment(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsNormalizedLine *line_range,
    LrcLyricsPreprocessOptions *options,
    int32 line_index,
    int32 word_start,
    int32 word_end,
    bool *wrote_line
) {
    return lrc_lyrics_normalize_split_segment(lyrics,
                                              normalized,
                                              line_range,
                                              options,
                                              line_index,
                                              word_start,
                                              word_end,
                                              true,
                                              wrote_line);
}

static int32
lrc_lyrics_next_word_end(
    LrcLyrics *lyrics,
    int32 start,
    int32 end
) {
    for (int32 i = start; i < end;) {
        uint32 rune;
        int32 step;

        step = utf8_decode_raw(lyrics->text + i, &rune, end - i);
        if (step <= 0) {
            return -1;
        }
        if (rune < 0x80) {
            char c = lyrics->text[i];

            if (lrc_lyrics_ascii_space(c)) {
                return i;
            }
        }

        i += step;
    }

    return end;
}

static bool
lrc_lyrics_preprocess_language_is(
    LrcLyricsPreprocessOptions *options,
    char *language
) {
    if ((options == NULL) || (language == NULL)) {
        return false;
    }

    return STREQUAL(options->language,
                     options->language_len,
                     language,
                     strlen32(language));
}

static enum LrcLyricsPreprocessSplitSize
lrc_lyrics_preprocess_effective_split_size(
    LrcLyricsPreprocessOptions *options
) {
    if (options == NULL) {
        return LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CURRENT;
    }
    if (options->split_size == LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CURRENT) {
        return options->split_size;
    }
    if (lrc_lyrics_preprocess_language_is(options, "jpn")
        || lrc_lyrics_preprocess_language_is(options, "chi")) {
        return LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CHAR;
    }

    return options->split_size;
}

static bool
lrc_lyrics_normalize_line_word(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsPreprocessOptions *options,
    int32 line_index,
    int32 start,
    int32 end
) {
    LrcLyricsNormalizedLine *line_range = &normalized->lines[line_index];
    bool wrote_line = false;


    for (int32 i = start; i < end;) {
        uint32 rune;
        int32 step;
        int32 word_start;
        int32 word_end;

        step = utf8_decode_raw(lyrics->text + i, &rune, end - i);
        if (step <= 0) {
            return false;
        }
        if ((rune < 0x80) && lrc_lyrics_ascii_space(lyrics->text[i])) {
            i += step;
            continue;
        }

        word_start = i;
        word_end = lrc_lyrics_next_word_end(lyrics, word_start, end);
        if (word_end < 0) {
            return false;
        }
        if (!lrc_lyrics_normalize_word_segment(lyrics,
                                               normalized,
                                               line_range,
                                               options,
                                               line_index,
                                               word_start,
                                               word_end,
                                               &wrote_line)) {
            return false;
        }
        i = word_end;
    }

    lrc_lyrics_normalized_line_finish(normalized, line_range);

    return true;
}

static bool
lrc_lyrics_normalize_char_segment(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsNormalizedLine *line_range,
    LrcLyricsPreprocessOptions *options,
    int32 line_index,
    int32 char_start,
    int32 char_end,
    bool *wrote_line
) {
    return lrc_lyrics_normalize_split_segment(lyrics,
                                              normalized,
                                              line_range,
                                              options,
                                              line_index,
                                              char_start,
                                              char_end,
                                              false,
                                              wrote_line);
}

static bool
lrc_lyrics_normalize_line_char(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsPreprocessOptions *options,
    int32 line_index,
    int32 start,
    int32 end
) {
    LrcLyricsNormalizedLine *line_range = &normalized->lines[line_index];
    bool wrote_line = false;


    for (int32 i = start; i < end;) {
        uint32 rune;
        int32 step;

        step = utf8_decode_raw(lyrics->text + i, &rune, end - i);
        if (step <= 0) {
            return false;
        }
        if (!lrc_lyrics_normalize_char_segment(lyrics,
                                               normalized,
                                               line_range,
                                               options,
                                               line_index,
                                               i,
                                               i + step,
                                               &wrote_line)) {
            return false;
        }
        i += step;
    }

    lrc_lyrics_normalized_line_finish(normalized, line_range);

    return true;
}

static bool
lrc_lyrics_normalize_with_options(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    LrcLyricsPreprocessOptions *options
) {
    LrcLyricsPreprocessOptions local_options;
    enum LrcLyricsPreprocessSplitSize split_size;

    if ((lyrics == NULL) || (normalized == NULL)) {
        return false;
    }

    if (options == NULL) {
        lrc_lyrics_preprocess_options_init(&local_options);
        options = &local_options;
    }

    split_size = lrc_lyrics_preprocess_effective_split_size(options);

    lrc_lyrics_normalized_destroy(normalized);
    if (!lrc_lyrics_normalized_alloc_lines(normalized, lyrics->line_count)) {
        return false;
    }

    for (int32 i = 0; i < lyrics->line_count; i += 1) {
        LrcLyricsLine *line = &lyrics->lines[i];
        bool ok;
        int32 start;
        int32 end;

        start = lrc_lyrics_line_trim_start(line);
        end = lrc_lyrics_line_trim_end(line, start);
        if (start >= end) {
            normalized->lines[i].kind = LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK;
            continue;
        }
        if (lrc_lyrics_line_is_section_marker(lyrics, start, end)) {
            normalized->lines[i].kind =
                LRC_LYRICS_NORMALIZED_LINE_KIND_SECTION_MARKER;
            continue;
        }

        switch (split_size) {
        case LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CURRENT:
            ok = lrc_lyrics_normalize_line(lyrics,
                                           normalized,
                                           i,
                                           start,
                                           end);
            break;
        case LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD:
            ok = lrc_lyrics_normalize_line_word(lyrics,
                                                normalized,
                                                options,
                                                i,
                                                start,
                                                end);
            break;
        case LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CHAR:
            ok = lrc_lyrics_normalize_line_char(lyrics,
                                                normalized,
                                                options,
                                                i,
                                                start,
                                                end);
            break;
        case LRC_LYRICS_PREPROCESS_SPLIT_SIZE_SENTENCE:
        case LRC_LYRICS_PREPROCESS_SPLIT_SIZE_COUNT:
        default:
            ok = false;
            break;
        }
        if (!ok) {
            lrc_lyrics_normalized_destroy(normalized);
            return false;
        }
    }

    return normalized->text_len > 0;
}

static enum LrcLyricsNormalizedLineKind
lrc_lyrics_normalized_line_kind(
    LrcLyricsNormalized *normalized,
    int32 line_index
) {
    if (normalized == NULL) {
        return LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK;
    }
    if ((line_index < 0) || (line_index >= normalized->line_count)) {
        return LRC_LYRICS_NORMALIZED_LINE_KIND_BLANK;
    }

    return normalized->lines[line_index].kind;
}

static bool
lrc_lyrics_normalized_line_range(
    LrcLyricsNormalized *normalized,
    int32 line_index,
    int32 *start,
    int32 *end
) {
    LrcLyricsNormalizedLine *line;

    if (start) {
        *start = -1;
    }
    if (end) {
        *end = -1;
    }
    if (normalized == NULL) {
        return false;
    }
    if ((line_index < 0) || (line_index >= normalized->line_count)) {
        return false;
    }

    line = &normalized->lines[line_index];
    if (line->kind != LRC_LYRICS_NORMALIZED_LINE_KIND_ALIGNABLE) {
        return false;
    }
    if (start) {
        *start = line->normalized_start;
    }
    if (end) {
        *end = line->normalized_end;
    }

    return true;
}

#if TESTING
static bool
lrc_lyrics_normalize(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized
) {
    LrcLyricsPreprocessOptions options;

    lrc_lyrics_preprocess_options_init(&options);

    return lrc_lyrics_normalize_with_options(lyrics, normalized, &options);
}

static int32
lrc_lyrics_normalized_line_at(
    LrcLyricsNormalized *normalized,
    int32 byte_offset
) {
    if (normalized == NULL) {
        return -1;
    }
    if ((byte_offset < 0) || (byte_offset >= normalized->byte_count)) {
        return -1;
    }

    return normalized->bytes[byte_offset].line_index;
}
#endif

#if TESTING_ctc_text
#define CBASE_IMPLEMENT
#include "cbase.h"

#include "lyrics.c"
#include "unicode_norm.c"

static int32
lrc_lyrics_normalized_segment_count(
    LrcLyricsNormalized *normalized
) {
    if (normalized == NULL) {
        return 0;
    }

    return normalized->segment_count;
}

static CtcTextSegment *
lrc_lyrics_normalized_segment(
    LrcLyricsNormalized *normalized,
    int32 segment_index
) {
    if (normalized == NULL) {
        return NULL;
    }
    if ((segment_index < 0) || (segment_index >= normalized->segment_count)) {
        return NULL;
    }

    return &normalized->segments[segment_index];
}

static int32
ctc_text_test_fail(char *name) {
    error2("CTC text test failed: %s\n", name);

    return 1;
}


static bool
ctc_text_load_lyrics(
    LrcLyrics *lyrics,
    char *text,
    char *name
) {
    LrcLyricsLoadResult result;
    char temp_dir[PATH_MAX];
    char path[PATH_MAX];
    bool ok;

    test_make_temp_dir(temp_dir, SIZEOF(temp_dir), name);
    test_join_path(path, SIZEOF(path), temp_dir, "lyrics.txt");
    if (write_entire_file(path, text, strlen32(text)) < 0) {
        test_remove_tree(temp_dir);
        return false;
    }

    memset64(lyrics, 0, SIZEOF(*lyrics));
    ok = lrc_lyrics_load_file(lyrics, path, &result);
    test_remove_tree(temp_dir);

    return ok;
}

static void
ctc_text_test_assert_segment(
    LrcLyrics *lyrics,
    LrcLyricsNormalized *normalized,
    int32 segment_index,
    int32 expected_line_index,
    int32 expected_normalized_start,
    int32 expected_normalized_end,
    int32 expected_target_start,
    int32 expected_target_end,
    char *expected_source
) {
    CtcTextSegment *segment;
    int32 source_len;

    segment = lrc_lyrics_normalized_segment(normalized, segment_index);
    ASSERT(segment);

    source_len = segment->source_end - segment->source_start;

    ASSERT(segment->line_index == expected_line_index);
    ASSERT(segment->normalized_start == expected_normalized_start);
    ASSERT(segment->normalized_end == expected_normalized_end);
    ASSERT(segment->target_start == expected_target_start);
    ASSERT(segment->target_end == expected_target_end);
    ASSERT(segment->target_start >= 0);
    ASSERT(segment->target_end <= normalized->target_byte_count);
    ASSERT(STREQUAL(lyrics->text + segment->source_start,
                     source_len,
                     expected_source,
                     strlen32(expected_source)));
    for (int32 i = segment->target_start; i < segment->target_end; i += 1) {
        LrcLyricsTargetByte *target_byte = &normalized->target_bytes[i];

        ASSERT(target_byte->line_index == expected_line_index);
        ASSERT(target_byte->normalized_start >= segment->normalized_start);
        ASSERT(target_byte->normalized_end <= segment->normalized_end);
    }

    return;
}



enum CtcTextReferenceCase {
    CTC_TEXT_REFERENCE_CASE_OTHER,
    CTC_TEXT_REFERENCE_CASE_PLAIN_ENGLISH,
    CTC_TEXT_REFERENCE_CASE_APOSTROPHES,
    CTC_TEXT_REFERENCE_CASE_PUNCTUATION_DIGITS,
    CTC_TEXT_REFERENCE_CASE_ACCENTS,
    CTC_TEXT_REFERENCE_CASE_BRACKETS_BLANK_LINES,
    CTC_TEXT_REFERENCE_CASE_ENGLISH_CHAR,
    CTC_TEXT_REFERENCE_CASE_JAPANESE_FORCE_CHAR,
    CTC_TEXT_REFERENCE_CASE_CHINESE_FORCE_CHAR,
    CTC_TEXT_REFERENCE_CASE_PORTUGUESE_SOLTASBRUXA,
    CTC_TEXT_REFERENCE_CASE_GERMAN_ICH_WILL,
};

typedef struct CtcTextReferenceFixtureTotals {
    bool saw_format;
    bool saw_plain_english;
    bool saw_apostrophes;
    bool saw_punctuation_digits;
    bool saw_accents;
    bool saw_brackets_blank_lines;
    bool saw_english_char;
    bool saw_japanese_force_char;
    bool saw_chinese_force_char;
    bool saw_portuguese_soltasbruxa;
    bool saw_german_ich_will;

    int32 fixture_count;
} CtcTextReferenceFixtureTotals;

typedef struct CtcTextReferenceFixtureCurrent {
    bool in_fixture;
    bool saw_language;
    bool saw_split_size;
    bool saw_effective_split_size;
    bool saw_romanize;
    bool saw_input;

    enum CtcTextReferenceCase case_id;

    int32 text_split_count;
    int32 normalized_count;
    int32 tokens_count;
    int32 edges_tokens_count;
    int32 edges_text_count;
    int32 segment_tokens_count;
    int32 segment_text_count;
} CtcTextReferenceFixtureCurrent;

static bool
ctc_text_reference_field_equal(
    char *field,
    int32 field_len,
    char *expected
) {
    return STREQUAL(field, field_len, expected, strlen32(expected));
}

static int32
ctc_text_reference_line_tab(
    char *line,
    int32 line_len
) {
    for (int32 i = 0; i < line_len; i += 1) {
        if (line[i] == '\t') {
            return i;
        }
    }

    return -1;
}

static void
ctc_text_reference_totals_mark_case(
    CtcTextReferenceFixtureTotals *totals,
    CtcTextReferenceFixtureCurrent *current,
    char *value,
    int32 value_len
) {
    if (ctc_text_reference_field_equal(value, value_len, "plain_english")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_PLAIN_ENGLISH;
        totals->saw_plain_english = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "apostrophes")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_APOSTROPHES;
        totals->saw_apostrophes = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "punctuation_digits")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_PUNCTUATION_DIGITS;
        totals->saw_punctuation_digits = true;
    } else if (ctc_text_reference_field_equal(value, value_len, "accents")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_ACCENTS;
        totals->saw_accents = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "brackets_blank_lines")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_BRACKETS_BLANK_LINES;
        totals->saw_brackets_blank_lines = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "english_char")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_ENGLISH_CHAR;
        totals->saw_english_char = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "japanese_force_char")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_JAPANESE_FORCE_CHAR;
        totals->saw_japanese_force_char = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "chinese_force_char")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_CHINESE_FORCE_CHAR;
        totals->saw_chinese_force_char = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "portuguese_soltasbruxa")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_PORTUGUESE_SOLTASBRUXA;
        totals->saw_portuguese_soltasbruxa = true;
    } else if (ctc_text_reference_field_equal(value,
                                              value_len,
                                              "german_ich_will")) {
        current->case_id = CTC_TEXT_REFERENCE_CASE_GERMAN_ICH_WILL;
        totals->saw_german_ich_will = true;
    } else {
        current->case_id = CTC_TEXT_REFERENCE_CASE_OTHER;
    }

    return;
}

static void
ctc_text_reference_validate_current(
    CtcTextReferenceFixtureCurrent *current
) {
    ASSERT(current->in_fixture);
    ASSERT(current->saw_language);
    ASSERT(current->saw_split_size);
    ASSERT(current->saw_effective_split_size);
    ASSERT(current->saw_romanize);
    ASSERT(current->saw_input);
    ASSERT(current->text_split_count > 0);
    ASSERT(current->normalized_count == current->text_split_count);
    ASSERT(current->tokens_count == current->normalized_count);
    ASSERT(current->edges_tokens_count == current->tokens_count + 2);
    ASSERT(current->edges_text_count == current->text_split_count + 2);
    ASSERT(current->segment_tokens_count == current->tokens_count*2);
    ASSERT(current->segment_text_count == current->text_split_count*2);

    return;
}

static void
ctc_text_reference_parse_field(
    CtcTextReferenceFixtureTotals *totals,
    CtcTextReferenceFixtureCurrent *current,
    char *field,
    int32 field_len,
    char *value,
    int32 value_len
) {
    if (ctc_text_reference_field_equal(field, field_len, "format")) {
        ASSERT(!current->in_fixture);
        ASSERT(ctc_text_reference_field_equal(value, value_len, "1"));
        totals->saw_format = true;
    } else if (ctc_text_reference_field_equal(field, field_len, "fixture")) {
        ASSERT(!current->in_fixture);
        memset64(current, 0, SIZEOF(*current));
        current->in_fixture = true;
        totals->fixture_count += 1;
        ctc_text_reference_totals_mark_case(totals,
                                            current,
                                            value,
                                            value_len);
    } else if (ctc_text_reference_field_equal(field, field_len, "language")) {
        ASSERT(current->in_fixture);
        ASSERT(value_len == 3);
        current->saw_language = true;
    } else if (ctc_text_reference_field_equal(field, field_len, "split_size")) {
        ASSERT(current->in_fixture);
        ASSERT(ctc_text_reference_field_equal(value, value_len, "word")
               || ctc_text_reference_field_equal(value, value_len, "char"));
        current->saw_split_size = true;
    } else if (ctc_text_reference_field_equal(field,
                                              field_len,
                                              "effective_split_size")) {
        ASSERT(current->in_fixture);
        ASSERT(ctc_text_reference_field_equal(value, value_len, "word")
               || ctc_text_reference_field_equal(value, value_len, "char"));
        current->saw_effective_split_size = true;
    } else if (ctc_text_reference_field_equal(field, field_len, "romanize")) {
        ASSERT(current->in_fixture);
        ASSERT(ctc_text_reference_field_equal(value, value_len, "false"));
        current->saw_romanize = true;
    } else if (ctc_text_reference_field_equal(field, field_len, "input")) {
        ASSERT(current->in_fixture);
        ASSERT(value_len > 0);
        current->saw_input = true;
    } else if (ctc_text_reference_field_equal(field, field_len, "text_split")) {
        ASSERT(current->in_fixture);
        current->text_split_count += 1;
    } else if (ctc_text_reference_field_equal(field, field_len, "normalized")) {
        ASSERT(current->in_fixture);
        if (current->case_id == CTC_TEXT_REFERENCE_CASE_PLAIN_ENGLISH) {
            if (current->normalized_count == 0) {
                ASSERT(ctc_text_reference_field_equal(value,
                                                      value_len,
                                                      "68656c6c6f"));
            } else if (current->normalized_count == 1) {
                ASSERT(ctc_text_reference_field_equal(value,
                                                      value_len,
                                                      "776f726c64"));
            }
        }
        current->normalized_count += 1;
    } else if (ctc_text_reference_field_equal(field, field_len, "tokens")) {
        ASSERT(current->in_fixture);
        if (current->case_id == CTC_TEXT_REFERENCE_CASE_PLAIN_ENGLISH) {
            if (current->tokens_count == 0) {
                ASSERT(ctc_text_reference_field_equal(value,
                                                      value_len,
                                                      "682065206c206c206f"));
            } else if (current->tokens_count == 1) {
                ASSERT(ctc_text_reference_field_equal(value,
                                                      value_len,
                                                      "77206f2072206c2064"));
            }
        }
        current->tokens_count += 1;
    } else if (ctc_text_reference_field_equal(field,
                                              field_len,
                                              "edges_tokens")) {
        ASSERT(current->in_fixture);
        current->edges_tokens_count += 1;
    } else if (ctc_text_reference_field_equal(field, field_len, "edges_text")) {
        ASSERT(current->in_fixture);
        current->edges_text_count += 1;
    } else if (ctc_text_reference_field_equal(field,
                                              field_len,
                                              "segment_tokens")) {
        ASSERT(current->in_fixture);
        current->segment_tokens_count += 1;
    } else if (ctc_text_reference_field_equal(field,
                                              field_len,
                                              "segment_text")) {
        ASSERT(current->in_fixture);
        current->segment_text_count += 1;
    } else {
        ASSERT(false);
    }

    return;
}

static int32
ctc_text_test_reference_fixtures_load(void) {
    CtcTextReferenceFixtureTotals totals = {0};
    CtcTextReferenceFixtureCurrent current = {0};
    char *text;
    int32 text_len;
    int32 line_start;

    if ((text_len = read_entire_file(
             "testdata/ctc_text_reference_fixtures.txt", &text)) < 0) {
        return ctc_text_test_fail("load reference fixtures");
    }

    line_start = 0;
    for (int32 i = 0; i <= text_len; i += 1) {
        if ((i == text_len) || (text[i] == '\n')) {
            char *line = text + line_start;
            int32 line_len = i - line_start;
            int32 tab;

            if ((line_len > 0) && (line[line_len - 1] == '\r')) {
                line_len -= 1;
            }
            line_start = i + 1;

            if (line_len <= 0) {
                continue;
            }
            if (line[0] == '#') {
                continue;
            }
            if (ctc_text_reference_field_equal(line, line_len, "end")) {
                ctc_text_reference_validate_current(&current);
                memset64(&current, 0, SIZEOF(current));
                continue;
            }

            tab = ctc_text_reference_line_tab(line, line_len);
            ASSERT(tab > 0);
            ctc_text_reference_parse_field(&totals,
                                           &current,
                                           line,
                                           tab,
                                           line + tab + 1,
                                           line_len - tab - 1);
        }
    }

    ASSERT(!current.in_fixture);
    ASSERT(totals.saw_format);
    ASSERT(totals.fixture_count == 10);
    ASSERT(totals.saw_plain_english);
    ASSERT(totals.saw_apostrophes);
    ASSERT(totals.saw_punctuation_digits);
    ASSERT(totals.saw_accents);
    ASSERT(totals.saw_brackets_blank_lines);
    ASSERT(totals.saw_english_char);
    ASSERT(totals.saw_japanese_force_char);
    ASSERT(totals.saw_chinese_force_char);
    ASSERT(totals.saw_portuguese_soltasbruxa);
    ASSERT(totals.saw_german_ich_will);

    free2(text, text_len + 1);

    return 0;
}


#define CTC_TEXT_REFERENCE_WORD_INPUT_MAX 1024
#define CTC_TEXT_REFERENCE_WORD_MAX 64
#define CTC_TEXT_REFERENCE_WORD_TEXT_MAX 128

typedef struct CtcTextReferenceWordFixture {
    char input[CTC_TEXT_REFERENCE_WORD_INPUT_MAX];
    char text_split[CTC_TEXT_REFERENCE_WORD_MAX]
                   [CTC_TEXT_REFERENCE_WORD_TEXT_MAX];
    char normalized[CTC_TEXT_REFERENCE_WORD_MAX]
                   [CTC_TEXT_REFERENCE_WORD_TEXT_MAX];
    char tokens[CTC_TEXT_REFERENCE_WORD_MAX]
               [CTC_TEXT_REFERENCE_WORD_TEXT_MAX];
    char edges_tokens[CTC_TEXT_REFERENCE_WORD_MAX*2]
                     [CTC_TEXT_REFERENCE_WORD_TEXT_MAX];
    char segment_tokens[CTC_TEXT_REFERENCE_WORD_MAX*2]
                       [CTC_TEXT_REFERENCE_WORD_TEXT_MAX];

    int32 input_len;
    int32 text_split_lens[CTC_TEXT_REFERENCE_WORD_MAX];
    int32 normalized_lens[CTC_TEXT_REFERENCE_WORD_MAX];
    int32 tokens_lens[CTC_TEXT_REFERENCE_WORD_MAX];
    int32 edges_tokens_lens[CTC_TEXT_REFERENCE_WORD_MAX*2];
    int32 segment_tokens_lens[CTC_TEXT_REFERENCE_WORD_MAX*2];
    int32 text_split_count;
    int32 normalized_count;
    int32 tokens_count;
    int32 edges_tokens_count;
    int32 segment_tokens_count;
} CtcTextReferenceWordFixture;

static int32
ctc_text_hex_value(char c) {
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    }
    if ((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    }
    if ((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }

    return -1;
}

static int32
ctc_text_decode_hex(
    char *buffer,
    int32 buffer_cap,
    char *hex,
    int32 hex_len
) {
    int32 len;

    if ((hex_len % 2) != 0) {
        return -1;
    }

    len = hex_len/2;
    if (len >= buffer_cap) {
        return -1;
    }

    for (int32 i = 0; i < len; i += 1) {
        int32 hi;
        int32 lo;

        hi = ctc_text_hex_value(hex[i*2]);
        lo = ctc_text_hex_value(hex[i*2 + 1]);
        if ((hi < 0) || (lo < 0)) {
            return -1;
        }
        buffer[i] = (char)((hi << 4) | lo);
    }
    buffer[len] = '\0';

    return len;
}

static bool
ctc_text_reference_load_word_fixture(
    char *fixture_name,
    CtcTextReferenceWordFixture *fixture
) {
    char *text;
    int32 text_len;
    int32 line_start;
    bool in_fixture;
    bool matched_fixture;
    bool found_fixture;

    memset64(fixture, 0, SIZEOF(*fixture));

    if ((text_len = read_entire_file(
             "testdata/ctc_text_reference_fixtures.txt", &text)) < 0) {
        return false;
    }

    line_start = 0;
    in_fixture = false;
    matched_fixture = false;
    found_fixture = false;
    for (int32 i = 0; i <= text_len; i += 1) {
        if ((i == text_len) || (text[i] == '\n')) {
            char *line = text + line_start;
            int32 line_len = i - line_start;
            int32 tab;

            if ((line_len > 0) && (line[line_len - 1] == '\r')) {
                line_len -= 1;
            }
            line_start = i + 1;

            if ((line_len <= 0) || (line[0] == '#')) {
                continue;
            }
            if (ctc_text_reference_field_equal(line, line_len, "end")) {
                if (matched_fixture) {
                    found_fixture = true;
                    break;
                }
                in_fixture = false;
                matched_fixture = false;
                continue;
            }

            tab = ctc_text_reference_line_tab(line, line_len);
            ASSERT(tab > 0);
            if (ctc_text_reference_field_equal(line, tab, "fixture")) {
                char *value = line + tab + 1;
                int32 value_len = line_len - tab - 1;

                in_fixture = true;
                matched_fixture = ctc_text_reference_field_equal(
                    value,
                    value_len,
                    fixture_name
                );
                continue;
            }
            if (!in_fixture || !matched_fixture) {
                continue;
            }

            if (ctc_text_reference_field_equal(line, tab, "input")) {
                fixture->input_len = ctc_text_decode_hex(
                    fixture->input,
                    CTC_TEXT_REFERENCE_WORD_INPUT_MAX,
                    line + tab + 1,
                    line_len - tab - 1
                );
                ASSERT(fixture->input_len >= 0);
            } else if (ctc_text_reference_field_equal(line,
                                                      tab,
                                                      "text_split")) {
                int32 index = fixture->text_split_count;

                ASSERT(index < CTC_TEXT_REFERENCE_WORD_MAX);
                fixture->text_split_lens[index] = ctc_text_decode_hex(
                    fixture->text_split[index],
                    CTC_TEXT_REFERENCE_WORD_TEXT_MAX,
                    line + tab + 1,
                    line_len - tab - 1
                );
                ASSERT(fixture->text_split_lens[index] >= 0);
                fixture->text_split_count += 1;
            } else if (ctc_text_reference_field_equal(line,
                                                      tab,
                                                      "normalized")) {
                int32 index = fixture->normalized_count;

                ASSERT(index < CTC_TEXT_REFERENCE_WORD_MAX);
                fixture->normalized_lens[index] = ctc_text_decode_hex(
                    fixture->normalized[index],
                    CTC_TEXT_REFERENCE_WORD_TEXT_MAX,
                    line + tab + 1,
                    line_len - tab - 1
                );
                ASSERT(fixture->normalized_lens[index] >= 0);
                fixture->normalized_count += 1;
            } else if (ctc_text_reference_field_equal(line, tab, "tokens")) {
                int32 index = fixture->tokens_count;

                ASSERT(index < CTC_TEXT_REFERENCE_WORD_MAX);
                fixture->tokens_lens[index] = ctc_text_decode_hex(
                    fixture->tokens[index],
                    CTC_TEXT_REFERENCE_WORD_TEXT_MAX,
                    line + tab + 1,
                    line_len - tab - 1
                );
                ASSERT(fixture->tokens_lens[index] >= 0);
                fixture->tokens_count += 1;
            } else if (ctc_text_reference_field_equal(line,
                                                      tab,
                                                      "edges_tokens")) {
                int32 index = fixture->edges_tokens_count;

                ASSERT(index < CTC_TEXT_REFERENCE_WORD_MAX*2);
                fixture->edges_tokens_lens[index] = ctc_text_decode_hex(
                    fixture->edges_tokens[index],
                    CTC_TEXT_REFERENCE_WORD_TEXT_MAX,
                    line + tab + 1,
                    line_len - tab - 1
                );
                ASSERT(fixture->edges_tokens_lens[index] >= 0);
                fixture->edges_tokens_count += 1;
            } else if (ctc_text_reference_field_equal(line,
                                                      tab,
                                                      "segment_tokens")) {
                int32 index = fixture->segment_tokens_count;

                ASSERT(index < CTC_TEXT_REFERENCE_WORD_MAX*2);
                fixture->segment_tokens_lens[index] = ctc_text_decode_hex(
                    fixture->segment_tokens[index],
                    CTC_TEXT_REFERENCE_WORD_TEXT_MAX,
                    line + tab + 1,
                    line_len - tab - 1
                );
                ASSERT(fixture->segment_tokens_lens[index] >= 0);
                fixture->segment_tokens_count += 1;
            }
        }
    }

    free2(text, text_len + 1);

    if (!found_fixture) {
        return false;
    }
    ASSERT(fixture->input_len > 0);
    ASSERT(fixture->text_split_count > 0);
    ASSERT(fixture->normalized_count == fixture->text_split_count);
    ASSERT(fixture->tokens_count == fixture->text_split_count);
    ASSERT(fixture->edges_tokens_count == fixture->tokens_count + 2);
    ASSERT(fixture->segment_tokens_count == fixture->tokens_count*2);

    return true;
}

static int32
ctc_text_test_word_split_fixture_case(char *fixture_name) {
    CtcTextReferenceWordFixture fixture;
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    LrcLyrics lyrics;

    if (!ctc_text_reference_load_word_fixture(fixture_name, &fixture)) {
        return ctc_text_test_fail("load word split fixture");
    }
    if (!ctc_text_load_lyrics(&lyrics, fixture.input, fixture_name)) {
        return ctc_text_test_fail("load word split fixture lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize word split fixture lyrics");
    }

    ASSERT(normalized.segment_count == fixture.text_split_count);
    for (int32 i = 0; i < fixture.text_split_count; i += 1) {
        CtcTextSegment *segment;
        int32 source_len;

        segment = lrc_lyrics_normalized_segment(&normalized, i);
        ASSERT(segment);
        source_len = segment->source_end - segment->source_start;
        ASSERT(STREQUAL(lyrics.text + segment->source_start,
                         source_len,
                         fixture.text_split[i],
                         fixture.text_split_lens[i]));
    }

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_word_split_matches_reference_fixtures(void) {
    int32 status = 0;


    status += ctc_text_test_word_split_fixture_case("plain_english");
    status += ctc_text_test_word_split_fixture_case("apostrophes");
    status += ctc_text_test_word_split_fixture_case("punctuation_digits");
    status += ctc_text_test_word_split_fixture_case("accents");
    status += ctc_text_test_word_split_fixture_case("portuguese_soltasbruxa");
    status += ctc_text_test_word_split_fixture_case("german_ich_will");

    return status;
}

static int32
ctc_text_test_word_normalized_fixture_case(char *fixture_name) {
    CtcTextReferenceWordFixture fixture;
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    LrcLyrics lyrics;

    if (!ctc_text_reference_load_word_fixture(fixture_name, &fixture)) {
        return ctc_text_test_fail("load word normalized fixture");
    }
    if (!ctc_text_load_lyrics(&lyrics, fixture.input, fixture_name)) {
        return ctc_text_test_fail("load word normalized fixture lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize word fixture lyrics");
    }

    ASSERT(normalized.segment_count == fixture.normalized_count);
    for (int32 i = 0; i < fixture.normalized_count; i += 1) {
        CtcTextSegment *segment;
        int32 normalized_len;

        segment = lrc_lyrics_normalized_segment(&normalized, i);
        ASSERT(segment);
        normalized_len = segment->normalized_end - segment->normalized_start;
        ASSERT(STREQUAL(
            normalized.text + segment->normalized_start,
            normalized_len,
            fixture.normalized[i],
            fixture.normalized_lens[i]
        ));
    }

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_word_normalization_matches_reference_fixtures(void) {
    int32 status = 0;


    status += ctc_text_test_word_normalized_fixture_case("plain_english");
    status += ctc_text_test_word_normalized_fixture_case("apostrophes");
    status += ctc_text_test_word_normalized_fixture_case("punctuation_digits");
    status += ctc_text_test_word_normalized_fixture_case("accents");
    status += ctc_text_test_word_normalized_fixture_case(
        "portuguese_soltasbruxa"
    );
    status += ctc_text_test_word_normalized_fixture_case("german_ich_will");

    return status;
}

static int32
ctc_text_test_word_target_fixture_case(char *fixture_name) {
    CtcTextReferenceWordFixture fixture;
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    LrcLyrics lyrics;

    if (!ctc_text_reference_load_word_fixture(fixture_name, &fixture)) {
        return ctc_text_test_fail("load word target fixture");
    }
    if (!ctc_text_load_lyrics(&lyrics, fixture.input, fixture_name)) {
        return ctc_text_test_fail("load word target fixture lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize word target fixture lyrics");
    }

    ASSERT(normalized.target_text);
    ASSERT(normalized.target_text_len > 0);
    ASSERT(normalized.target_byte_count == normalized.target_text_len);
    ASSERT(normalized.segment_count == fixture.tokens_count);

    for (int32 i = 0; i < fixture.tokens_count; i += 1) {
        CtcTextSegment *segment;
        int32 target_len;

        segment = lrc_lyrics_normalized_segment(&normalized, i);
        ASSERT(segment);
        target_len = segment->target_end - segment->target_start;
        ASSERT(STREQUAL(normalized.target_text + segment->target_start,
                         target_len,
                         fixture.tokens[i],
                         fixture.tokens_lens[i]));
    }

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_word_targets_match_reference_fixtures(void) {
    int32 status = 0;


    status += ctc_text_test_word_target_fixture_case("plain_english");
    status += ctc_text_test_word_target_fixture_case("apostrophes");
    status += ctc_text_test_word_target_fixture_case("punctuation_digits");
    status += ctc_text_test_word_target_fixture_case("accents");
    status += ctc_text_test_word_target_fixture_case("portuguese_soltasbruxa");
    status += ctc_text_test_word_target_fixture_case("german_ich_will");

    return status;
}

static void
ctc_text_test_assert_target_item(
    LrcLyricsNormalized *normalized,
    CtcTextSegment *segment,
    char *expected,
    int32 expected_len
) {
    int32 target_len = segment->target_end - segment->target_start;

    ASSERT(target_len >= 0);
    ASSERT(STREQUAL(normalized->target_text + segment->target_start,
                     target_len,
                     expected,
                     expected_len));

    return;
}

static int32
ctc_text_test_star_target_sequence_fixture_case(char *fixture_name) {
    CtcTextReferenceWordFixture fixture;
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    LrcLyrics lyrics;
    int32 expected_index;

    if (!ctc_text_reference_load_word_fixture(fixture_name, &fixture)) {
        return ctc_text_test_fail("load star sequence fixture");
    }
    if (!ctc_text_load_lyrics(&lyrics, fixture.input, fixture_name)) {
        return ctc_text_test_fail("load star sequence lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize star sequence lyrics");
    }

    ASSERT(normalized.segment_count == fixture.tokens_count);

    ASSERT_EQUAL(fixture.edges_tokens[0], "<star>");
    expected_index = 1;
    for (int32 i = 0; i < normalized.segment_count; i += 1) {
        CtcTextSegment *segment;

        segment = lrc_lyrics_normalized_segment(&normalized, i);
        ASSERT(segment);
        ctc_text_test_assert_target_item(
            &normalized,
            segment,
            fixture.edges_tokens[expected_index],
            fixture.edges_tokens_lens[expected_index]
        );
        expected_index += 1;
    }
    ASSERT_EQUAL(fixture.edges_tokens[expected_index], "<star>");
    ASSERT(expected_index + 1 == fixture.edges_tokens_count);

    expected_index = 0;
    for (int32 i = 0; i < normalized.segment_count; i += 1) {
        CtcTextSegment *segment;

        segment = lrc_lyrics_normalized_segment(&normalized, i);
        ASSERT(segment);
        ASSERT_EQUAL(fixture.segment_tokens[expected_index], "<star>");
        expected_index += 1;
        ctc_text_test_assert_target_item(
            &normalized,
            segment,
            fixture.segment_tokens[expected_index],
            fixture.segment_tokens_lens[expected_index]
        );
        expected_index += 1;
    }
    ASSERT(expected_index == fixture.segment_tokens_count);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_star_target_sequences_match_reference(void) {
    int32 status = 0;


    status += ctc_text_test_star_target_sequence_fixture_case("plain_english");
    status += ctc_text_test_star_target_sequence_fixture_case("apostrophes");
    status += ctc_text_test_star_target_sequence_fixture_case(
        "punctuation_digits"
    );

    return status;
}

static int32
ctc_text_test_word_target_text_is_character_spaced(void) {
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    CtcTextSegment *segment;
    LrcLyrics lyrics;

    if (!ctc_text_load_lyrics(&lyrics,
                              "Hello world\n",
                              "ctc_text_word_target")) {
        return ctc_text_test_fail("load word target lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize word target lyrics");
    }

    ASSERT_EQUAL(normalized.text, "hello world");
    ASSERT_EQUAL(normalized.target_text, "h e l l o w o r l d");
    ASSERT(normalized.segment_count == 2);

    segment = lrc_lyrics_normalized_segment(&normalized, 0);
    ASSERT(segment);
    ASSERT(segment->normalized_start == 0);
    ASSERT(segment->normalized_end == 5);
    ASSERT(segment->target_start == 0);
    ASSERT(segment->target_end == 9);

    segment = lrc_lyrics_normalized_segment(&normalized, 1);
    ASSERT(segment);
    ASSERT(segment->normalized_start == 6);
    ASSERT(segment->normalized_end == 11);
    ASSERT(segment->target_start == 10);
    ASSERT(segment->target_end == 19);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static void
ctc_text_test_options_language(
    LrcLyricsPreprocessOptions *options,
    char *language
) {
    int32 language_len;

    language_len = strlen32(language);
    ASSERT(language_len == 3);

    memcpy64(options->language, language, language_len);
    options->language[language_len] = '\0';
    options->language_len = language_len;

    return;
}

static int32
ctc_text_test_char_fixture_case(
    char *fixture_name,
    char *language,
    enum LrcLyricsPreprocessSplitSize split_size
) {
    CtcTextReferenceWordFixture fixture;
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    LrcLyrics lyrics;

    if (!ctc_text_reference_load_word_fixture(fixture_name, &fixture)) {
        return ctc_text_test_fail("load char fixture");
    }
    if (!ctc_text_load_lyrics(&lyrics, fixture.input, fixture_name)) {
        return ctc_text_test_fail("load char fixture lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = split_size;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_OFF;
    ctc_text_test_options_language(&options, language);

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize char fixture lyrics");
    }

    ASSERT(normalized.segment_count == fixture.text_split_count);
    ASSERT(normalized.segment_count == fixture.normalized_count);
    ASSERT(normalized.segment_count == fixture.tokens_count);
    for (int32 i = 0; i < fixture.text_split_count; i += 1) {
        CtcTextSegment *segment;
        int32 source_len;
        int32 normalized_len;
        int32 target_len;

        segment = lrc_lyrics_normalized_segment(&normalized, i);
        ASSERT(segment);

        source_len = segment->source_end - segment->source_start;
        normalized_len = segment->normalized_end - segment->normalized_start;
        target_len = segment->target_end - segment->target_start;

        ASSERT(STREQUAL(lyrics.text + segment->source_start,
                         source_len,
                         fixture.text_split[i],
                         fixture.text_split_lens[i]));
        ASSERT(STREQUAL(normalized.text + segment->normalized_start,
                         normalized_len,
                         fixture.normalized[i],
                         fixture.normalized_lens[i]));
        ASSERT(STREQUAL(normalized.target_text + segment->target_start,
                         target_len,
                         fixture.tokens[i],
                         fixture.tokens_lens[i]));
    }

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_char_split_matches_reference_fixtures(void) {
    int32 status = 0;


    status += ctc_text_test_char_fixture_case(
        "english_char",
        "eng",
        LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CHAR
    );
    status += ctc_text_test_char_fixture_case(
        "japanese_force_char",
        "jpn",
        LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD
    );
    status += ctc_text_test_char_fixture_case(
        "chinese_force_char",
        "chi",
        LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD
    );

    return status;
}

#if LRC_UNICODE_ENABLE_ICU
static int32
ctc_text_test_icu_word_romanization(void) {
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    CtcTextSegment *segment;
    LrcLyrics lyrics;

    if (!ctc_text_load_lyrics(&lyrics,
                              "猫 Привет\n",
                              "ctc_text_icu_word_romanization")) {
        return ctc_text_test_fail("load ICU romanization lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize ICU romanization lyrics");
    }

    ASSERT_EQUAL(normalized.text, "mao privet");
    ASSERT_EQUAL(normalized.target_text, "m a o p r i v e t");
    ASSERT(normalized.segment_count == 2);

    segment = lrc_lyrics_normalized_segment(&normalized, 0);
    ASSERT(segment);
    ASSERT(segment->normalized_start == 0);
    ASSERT(segment->normalized_end == 3);
    ASSERT(segment->target_start == 0);
    ASSERT(segment->target_end == 5);

    segment = lrc_lyrics_normalized_segment(&normalized, 1);
    ASSERT(segment);
    ASSERT(segment->normalized_start == 4);
    ASSERT(segment->normalized_end == 10);
    ASSERT(segment->target_start == 6);
    ASSERT(segment->target_end == 17);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_icu_char_romanization(void) {
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized normalized = {0};
    CtcTextSegment *segment;
    LrcLyrics lyrics;

    if (!ctc_text_load_lyrics(&lyrics,
                              "你好\n",
                              "ctc_text_icu_char_romanization")) {
        return ctc_text_test_fail("load ICU char romanization lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_CHAR;
    options.romanization = LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU;

    if (!lrc_lyrics_normalize_with_options(&lyrics, &normalized, &options)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize ICU char romanization lyrics");
    }

    ASSERT_EQUAL(normalized.text, "nihao");
    ASSERT_EQUAL(normalized.target_text, "n i h a o");
    ASSERT(normalized.segment_count == 2);

    segment = lrc_lyrics_normalized_segment(&normalized, 0);
    ASSERT(segment);
    ASSERT(segment->normalized_start == 0);
    ASSERT(segment->normalized_end == 2);
    ASSERT(segment->target_start == 0);
    ASSERT(segment->target_end == 3);

    segment = lrc_lyrics_normalized_segment(&normalized, 1);
    ASSERT(segment);
    ASSERT(segment->normalized_start == 2);
    ASSERT(segment->normalized_end == 5);
    ASSERT(segment->target_start == 4);
    ASSERT(segment->target_end == 9);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}
#endif

static int32
ctc_text_test_word_split_option_preserves_current_text(void) {
    LrcLyricsPreprocessOptions options;
    LrcLyricsNormalized current = {0};
    LrcLyricsNormalized word = {0};
    LrcLyrics lyrics;

    if (!ctc_text_load_lyrics(&lyrics,
                              "Hello, WORLD!\n[Verse]\nagain?! voce\n",
                              "ctc_text_word_options")) {
        return ctc_text_test_fail("load word option lyrics");
    }

    lrc_lyrics_preprocess_options_init(&options);
    options.split_size = LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD;

    if (!lrc_lyrics_normalize(&lyrics, &current)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize current option text");
    }
    if (!lrc_lyrics_normalize_with_options(&lyrics, &word, &options)) {
        lrc_lyrics_normalized_destroy(&current);
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize word option text");
    }

    ASSERT_EQUAL(current.text, word.text);
    ASSERT(current.segment_count == word.segment_count);
    ASSERT(word.alignable_line_count == 2);

    lrc_lyrics_normalized_destroy(&word);
    lrc_lyrics_normalized_destroy(&current);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_default_options(void) {
    LrcLyricsPreprocessOptions options;

    lrc_lyrics_preprocess_options_init(&options);

    ASSERT(options.split_size == LRC_LYRICS_PREPROCESS_SPLIT_SIZE_WORD);
    ASSERT(
        options.star_frequency == LRC_LYRICS_PREPROCESS_STAR_FREQUENCY_EDGES
    );
    ASSERT(options.romanization == LRC_LYRICS_PREPROCESS_ROMANIZATION_ICU);
    ASSERT_EQUAL(options.language, "eng");

    return 0;
}

static int32
ctc_text_test_word_segments_preserve_line_mapping(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};

    if (!ctc_text_load_lyrics(&lyrics,
                              "Hello, WORLD!\n[Verse]\nagain?! voce\n",
                              "ctc_text_segments")) {
        return ctc_text_test_fail("load segment lyrics");
    }

    if (!lrc_lyrics_normalize(&lyrics, &normalized)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize segment lyrics");
    }

    ASSERT_EQUAL(normalized.text, "hello world again voce");
    ASSERT(lrc_lyrics_normalized_segment_count(&normalized) == 4);
    ASSERT(lrc_lyrics_normalized_segment(&normalized, -1) == NULL);
    ASSERT(lrc_lyrics_normalized_segment(&normalized, 4) == NULL);

    ctc_text_test_assert_segment(
        &lyrics, &normalized, 0, 0, 0, 5, 0, 9, "Hello,"
    );
    ctc_text_test_assert_segment(
        &lyrics, &normalized, 1, 0, 6, 11, 10, 19, "WORLD!"
    );
    ctc_text_test_assert_segment(
        &lyrics, &normalized, 2, 2, 12, 17, 20, 29, "again?!"
    );
    ctc_text_test_assert_segment(
        &lyrics, &normalized, 3, 2, 18, 22, 30, 37, "voce"
    );

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

static int32
ctc_text_test_current_normalization_mapping(void) {
    LrcLyrics lyrics;
    LrcLyricsNormalized normalized = {0};

    if (!ctc_text_load_lyrics(&lyrics,
                              "Hello, WORLD!\n[Verse]\nagain?!\n",
                              "ctc_text_mapping")) {
        return ctc_text_test_fail("load lyrics");
    }

    if (!lrc_lyrics_normalize(&lyrics, &normalized)) {
        lrc_lyrics_destroy(&lyrics);
        return ctc_text_test_fail("normalize lyrics");
    }

    ASSERT_EQUAL(normalized.text, "hello world again");
    ASSERT(normalized.alignable_line_count == 2);
    ASSERT(lrc_lyrics_normalized_line_kind(
        &normalized,
        1
    ) == LRC_LYRICS_NORMALIZED_LINE_KIND_SECTION_MARKER);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized, 0) == 0);
    ASSERT(lrc_lyrics_normalized_line_at(&normalized,
                                         normalized.text_len - 1) == 2);

    lrc_lyrics_normalized_destroy(&normalized);
    lrc_lyrics_destroy(&lyrics);

    return 0;
}

int32
main(void) {
    int32 status = 0;


    status += ctc_text_test_default_options();
    status += ctc_text_test_word_segments_preserve_line_mapping();
    status += ctc_text_test_current_normalization_mapping();
    status += ctc_text_test_reference_fixtures_load();
    status += ctc_text_test_word_split_option_preserves_current_text();
    status += ctc_text_test_word_split_matches_reference_fixtures();
    status += ctc_text_test_word_normalization_matches_reference_fixtures();
    status += ctc_text_test_word_target_text_is_character_spaced();
    status += ctc_text_test_word_targets_match_reference_fixtures();
    status += ctc_text_test_star_target_sequences_match_reference();
    status += ctc_text_test_char_split_matches_reference_fixtures();
#if LRC_UNICODE_ENABLE_ICU
    status += ctc_text_test_icu_word_romanization();
    status += ctc_text_test_icu_char_romanization();
#endif

    exit(status);
}
#endif /* TESTING_ctc_text */
