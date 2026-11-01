#include "chat_store_codec.h"

static const uint8_t magic[8] = {'P','4','C','H','A','T','0','1'};
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
uint32_t chat_crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}
void chat_header_encode(uint8_t *out, uint32_t generation, uint32_t count, uint32_t crc)
{
    for (unsigned i = 0; i < 8; ++i) out[i] = magic[i];
    write32(out + 8, generation); write32(out + 12, count); write32(out + 16, crc);
}
bool chat_header_decode(const uint8_t *data, uint32_t *generation, uint32_t *count, uint32_t *crc)
{
    for (unsigned i = 0; i < 8; ++i) if (data[i] != magic[i]) return false;
    *generation = read32(data + 8); *count = read32(data + 12); *crc = read32(data + 16);
    return *generation != 0 && *count <= CHAT_MAX_MESSAGES;
}
void chat_record_encode(uint8_t *out, unsigned role, unsigned length, uint32_t turn)
{
    out[0] = (uint8_t)role; out[1] = 0;
    out[2] = (uint8_t)length; out[3] = (uint8_t)(length >> 8);
    write32(out + 4, turn);
}
bool chat_record_decode(const uint8_t *data, unsigned *role, unsigned *length, uint32_t *turn)
{
    *role = data[0]; *length = (unsigned)data[2] | (unsigned)data[3] << 8;
    *turn = read32(data + 4);
    return data[1] == 0 && *role <= 2 && *length > 0 && *length < CHAT_TEXT_BYTES;
}
size_t chat_utf8_copy(char *out, size_t capacity, const char *text)
{
    if (!capacity) return 0;
    size_t n = 0;
    while (text[n]) {
        const uint8_t a = (uint8_t)text[n];
        unsigned width = a < 0x80 ? 1 : a >= 0xC2 && a <= 0xDF ? 2 :
                         a >= 0xE0 && a <= 0xEF ? 3 : a >= 0xF0 && a <= 0xF4 ? 4 : 0;
        if (!width || n + width >= capacity) break;
        bool valid = true;
        for (unsigned i = 1; i < width; ++i)
            if ((uint8_t)text[n + i] < 0x80 || (uint8_t)text[n + i] > 0xBF) { valid = false; break; }
        if (!valid || (a == 0xE0 && (uint8_t)text[n + 1] < 0xA0) ||
            (a == 0xED && (uint8_t)text[n + 1] >= 0xA0) ||
            (a == 0xF0 && (uint8_t)text[n + 1] < 0x90) ||
            (a == 0xF4 && (uint8_t)text[n + 1] >= 0x90)) break;
        for (unsigned i = 0; i < width; ++i) out[n + i] = text[n + i];
        n += width;
    }
    out[n] = '\0';
    return n;
}

bool chat_stream_update(char *buffer, size_t capacity, size_t *prefix, int *sequence,
                        const char *text, int incoming_sequence, bool cumulative)
{
    if (!capacity || *prefix >= capacity || !text[0]) return false;
    if (incoming_sequence >= 0 && *sequence >= 0 && incoming_sequence < *sequence) return false;
    /* User ASR revises the whole held utterance, including punctuation and
     * earlier words. A higher sequence is not a new sentence boundary. */
    if (cumulative) *prefix = 0;
    const char *old = buffer + *prefix;
    size_t shared = 0;
    while (old[shared] && text[shared] && old[shared] == text[shared]) ++shared;
    if (!old[shared] && !text[shared]) {
        if (incoming_sequence >= 0) *sequence = incoming_sequence;
        return false;
    }
    if (!cumulative && incoming_sequence >= 0 && *sequence >= 0 && incoming_sequence != *sequence && old[shared]) {
        size_t length = 0;
        while (buffer[length]) ++length;
        if (length >= capacity - 2) return false;
        buffer[length++] = '\n'; buffer[length] = '\0'; *prefix = length;
    }
    if (incoming_sequence >= 0) *sequence = incoming_sequence;
    chat_utf8_copy(buffer + *prefix, capacity - *prefix, text);
    return true;
}
