#include <cstdio>
#include <cstring>
#include <chrono>
#include "pinyinime.h"
#include "p4_pinyin.h"

using namespace ime_pinyin;
int main(int argc, char **argv)
{
    static_assert(sizeof(size_t) == 4, "Probe the same model ABI as P4");
    if (argc != 2 || !im_open_decoder(argv[1], "")) return 2;
    im_set_max_lens(39, 16);
    struct Case { const char *pinyin; const char16_t *expected; };
    const Case cases[] = {
        {"nihao", u"你好"}, {"zhongguo", u"中国"},
        {"xiexie", u"谢谢"}, {"zaijian", u"再见"},
        {"woaini", u"我爱你"}, {"ni", u"你"}
    };
    for (const auto &test : cases) {
        im_reset_search();
        auto started = std::chrono::steady_clock::now();
        size_t count = im_search(test.pinyin, std::strlen(test.pinyin));
        char16 word[41] = {};
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (im_get_candidate(i, word, 41) &&
                utf16_strcmp(word, reinterpret_cast<const char16 *>(test.expected)) == 0) found = true;
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::printf("public_case=%s candidates=%u expected_present=%d elapsed_us=%lld\n",
                    test.pinyin, (unsigned)count, found, (long long)us);
        if (!found) return 3;
    }
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        im_reset_search();
        for (size_t n = 1; n <= 5; ++n) im_search("nihao", n);
        im_search("niha", 4);
        im_reset_search();
    }
    im_close_decoder();
    std::puts("repeated_search_reset=100");
    if (!p4_pinyin_open(argv[1])) return 4;
    p4_pinyin_result_t result;
    if (!p4_pinyin_query("nihao", 0, -1, &result) || result.count != 6 || result.total <= 6) return 5;
    if (!p4_pinyin_query("nihao", 0, 0, &result) ||
        std::strcmp(result.committed, "你好") || result.remaining[0]) return 6;
    if (!p4_pinyin_query("nihao", 6, -1, &result) || result.offset != 6 || !result.count) return 7;
    if (p4_pinyin_query("nihao!", 0, -1, &result) || result.count) return 8;
    if (!p4_pinyin_query("", 0, -1, &result) || result.count || result.total) return 9;
    bool partial = false;
    for (int i = 1; i < 62; ++i) {
        if (p4_pinyin_query("nihao", 0, i, &result) &&
            std::strlen(result.committed) == 3 && !std::strcmp(result.remaining, "hao")) partial = true;
    }
    if (!partial) return 10;
    p4_pinyin_close();
    std::puts("production_wrapper_cases=6");
    return 0;
}
