#include "p4_pinyin.h"
#include "vendor/include/pinyinime.h"
#include <cstring>

using namespace ime_pinyin;
static bool ready;

static bool utf8(char *output, const char16 *input)
{
    unsigned used = 0;
    for (unsigned i = 0; input[i]; ++i) {
        unsigned cp = input[i];
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        unsigned n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
        if (used + n >= P4_PINYIN_TEXT) return false;
        if (n == 3) output[used++] = (char)(0xe0 | (cp >> 12));
        if (n >= 2) output[used++] = (char)((n == 2 ? 0xc0 : 0x80) | ((cp >> 6) & (n == 2 ? 0x1f : 0x3f)));
        output[used++] = (char)(n == 1 ? cp : 0x80 | (cp & 0x3f));
    }
    output[used] = 0;
    return true;
}

bool p4_pinyin_open(const char *dictionary)
{
    static_assert(sizeof(size_t) == 4, "Dictionary ABI must match P4");
    static_assert(sizeof(LmaNodeLE0) == 16, "Dictionary node ABI");
    if (ready) return true;
    ready = dictionary && im_open_decoder(dictionary, "");
    if (!ready) im_close_decoder();
    else im_set_max_lens(P4_PINYIN_MAX, 16);
    return ready;
}

void p4_pinyin_close(void) { im_close_decoder(); ready = false; }

bool p4_pinyin_query(const char *spelling, unsigned offset, int choose,
                     p4_pinyin_result_t *result)
{
    if (!result) return false;
    std::memset(result, 0, sizeof(*result));
    if (!ready || !spelling) return false;
    const size_t length = std::strlen(spelling);
    if (length > P4_PINYIN_MAX) return false;
    for (size_t i = 0; i < length; ++i)
        if ((spelling[i] < 'a' || spelling[i] > 'z') && spelling[i] != '\'') return false;
    im_reset_search();
    if (!length) return true;
    result->total = im_search(spelling, length);
    char16 word[41] = {};
    if (choose >= 0) {
        if ((unsigned)choose >= result->total || !im_get_candidate(choose, word, 41)) return false;
        const uint16 *starts = nullptr;
        size_t syllables = im_get_spl_start_pos(starts);
        size_t characters = utf16_strlen(word);
        if (!characters || !starts || characters > syllables) return false;
        const size_t consumed = starts[characters];
        if (!consumed || consumed > length || !utf8(result->committed, word)) return false;
        std::memcpy(result->remaining, spelling + consumed, length - consumed + 1);
        return true;
    }
    if (result->total) {
        result->offset = offset < result->total ? offset : 0;
        for (unsigned i = result->offset; i < result->total && result->count < P4_PINYIN_PAGE; ++i) {
            if (!im_get_candidate(i, word, 41) || !utf8(result->words[result->count], word)) return false;
            ++result->count;
        }
    }
    return true;
}
