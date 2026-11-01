#pragma once
#include <stdbool.h>
#include <stdint.h>

#define P4_PINYIN_MAX 39
#define P4_PINYIN_PAGE 6
#define P4_PINYIN_TEXT 64

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    unsigned total, offset, count;
    char words[P4_PINYIN_PAGE][P4_PINYIN_TEXT];
    char committed[P4_PINYIN_TEXT];
    char remaining[P4_PINYIN_MAX + 1];
} p4_pinyin_result_t;

/* Single worker owns this decoder; never call concurrently or from the UI. */
bool p4_pinyin_open(const char *dictionary);
void p4_pinyin_close(void);
bool p4_pinyin_query(const char *spelling, unsigned offset, int choose,
                     p4_pinyin_result_t *result);
#ifdef __cplusplus
}
#endif
