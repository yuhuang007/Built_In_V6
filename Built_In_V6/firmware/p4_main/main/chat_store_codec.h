#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define CHAT_MAX_MESSAGES 200
#define CHAT_TEXT_BYTES 4096
#define CHAT_HEADER_BYTES 20
#define CHAT_RECORD_BYTES 8
#define CHAT_FILE_MAX_BYTES (CHAT_HEADER_BYTES + CHAT_MAX_MESSAGES * (CHAT_RECORD_BYTES + CHAT_TEXT_BYTES - 1))

#if defined(CHAT_HOST_TEST) && defined(_WIN32)
#define CHAT_CODEC_API __declspec(dllexport)
#else
#define CHAT_CODEC_API
#endif

/* CRC state starts at UINT32_MAX and finishes with XOR UINT32_MAX. */
CHAT_CODEC_API uint32_t chat_crc_update(uint32_t crc, const uint8_t *data, size_t length);
CHAT_CODEC_API void chat_header_encode(uint8_t *out, uint32_t generation, uint32_t count, uint32_t crc);
CHAT_CODEC_API bool chat_header_decode(const uint8_t *data, uint32_t *generation, uint32_t *count, uint32_t *crc);
CHAT_CODEC_API void chat_record_encode(uint8_t *out, unsigned role, unsigned length, uint32_t turn);
CHAT_CODEC_API bool chat_record_decode(const uint8_t *data, unsigned *role, unsigned *length, uint32_t *turn);
/* Bounded valid UTF-8 copy. Returns bytes copied, excludes the terminator. */
CHAT_CODEC_API size_t chat_utf8_copy(char *out, size_t capacity, const char *text);
CHAT_CODEC_API bool chat_stream_update(char *buffer, size_t capacity, size_t *prefix, int *sequence,
                        const char *text, int incoming_sequence, bool cumulative);
