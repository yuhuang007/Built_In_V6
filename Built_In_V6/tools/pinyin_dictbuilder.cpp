#include <cstdio>
#include "dicttrie.h"

int main(int argc, char **argv)
{
    static_assert(sizeof(size_t) == 4, "P4 dictionary requires 32-bit size_t");
    static_assert(sizeof(ime_pinyin::LmaNodeLE0) == 16, "P4 node layout");
    if (argc != 4) return 2;
    ime_pinyin::DictTrie dictionary;
    if (!dictionary.build_dict(argv[1], argv[2])) return 3;
    if (!dictionary.save_dict(argv[3])) return 4;
    std::puts("dictionary_built=1");
    return 0;
}
