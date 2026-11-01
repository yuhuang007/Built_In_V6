set(decoder_sources dictbuilder dictlist dicttrie lpicache matrixsearch mystdlib
    ngram pinyinime searchutility spellingtable spellingtrie splparser sync
    userdict utf16char utf16reader)
set(sources p4_pinyin.cpp)
foreach(source IN LISTS decoder_sources)
    list(APPEND sources "vendor/src/${source}.cpp")
endforeach()
idf_component_register(SRCS ${sources} INCLUDE_DIRS "." PRIV_REQUIRES pthread)
target_compile_definitions(${COMPONENT_LIB} PRIVATE P4_PINYIN_READ_ONLY=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-error -fno-strict-aliasing)
