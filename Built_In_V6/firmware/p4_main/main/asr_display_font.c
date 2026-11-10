/* Cache individual, enlarged A8 glyphs instead of scaling an entire scrolling
 * label into an off-screen layer. SD reads occur only on a glyph cache miss.
 * Shared fonts and the SD file are never modified. */
#include "asr_display_font.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static uint32_t profile[6];
void asr_display_font_profile(uint32_t out[6]) { memcpy(out,profile,sizeof(profile)); }

#define CACHE_SLOTS 512
#define CACHE_BYTE_LIMIT (768 * 1024)
extern const lv_font_t lv_font_asr_symbols;

typedef struct {
    uint32_t letter;
    uint32_t age;
    uint8_t *pixels;
    size_t bytes;
} glyph_entry_t;

typedef struct {
    lv_font_t font;
    const lv_font_t *source;
    unsigned scale;
    bool supplemental;
    glyph_entry_t entries[CACHE_SLOTS];
    size_t bytes;
    uint32_t clock;
    uint32_t hits, misses, evictions;
} display_font_t;

static bool source_glyph(display_font_t *cache, uint32_t letter,
                         lv_font_glyph_dsc_t *glyph)
{
    /* The supplemental font contains ASCII and common Unicode punctuation.
     * Query it first so even empty/missing punctuation in the SD font is covered. */
    if (cache->supplemental && lv_font_get_glyph_dsc(&lv_font_asr_symbols, glyph, letter, 0) &&
        !glyph->is_placeholder) return true;
    return lv_font_get_glyph_dsc(cache->source, glyph, letter, 0) &&
           !glyph->is_placeholder;
}

static bool glyph_descriptor(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                             uint32_t letter, uint32_t next)
{
    const uint64_t measured=esp_timer_get_time();
    (void)next;
    display_font_t *cache = font->user_data;
    lv_font_glyph_dsc_t source;
    if (!source_glyph(cache, letter, &source)) { profile[5]+=esp_timer_get_time()-measured;return false; }
    *out = source;
    out->resolved_font = font;
    out->entry = NULL;
    out->gid.index = letter; /* Independent of either source font's glyph IDs. */
    out->adv_w *= cache->scale;
    out->box_w *= cache->scale;
    out->box_h *= cache->scale;
    out->ofs_x *= cache->scale;
    out->ofs_y *= cache->scale;
    out->stride = out->box_w;
    out->format = LV_FONT_GLYPH_FORMAT_A8;
    profile[5]+=esp_timer_get_time()-measured;
    return true;
}

static glyph_entry_t *oldest_entry(display_font_t *cache)
{
    glyph_entry_t *oldest = NULL;
    for (unsigned i = 0; i < CACHE_SLOTS; ++i) {
        glyph_entry_t *entry = &cache->entries[i];
        if (entry->pixels && (!oldest || entry->age < oldest->age)) oldest = entry;
    }
    return oldest;
}

static void evict(display_font_t *cache, glyph_entry_t *entry)
{
    cache->bytes -= entry->bytes;
    heap_caps_free(entry->pixels);
    memset(entry, 0, sizeof(*entry));
    ++cache->evictions; ++profile[2];
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *output)
{
    display_font_t *cache = glyph->resolved_font->user_data;
    const uint64_t bitmap_started=esp_timer_get_time();
    const uint32_t letter = glyph->gid.index;
    if (!output || !glyph->box_w || !glyph->box_h) return NULL;
    const size_t bytes = (size_t)glyph->box_w * glyph->box_h;
    glyph_entry_t *empty = NULL;
    for (unsigned i = 0; i < CACHE_SLOTS; ++i) {
        glyph_entry_t *entry = &cache->entries[i];
        if (!entry->pixels) { if (!empty) empty = entry; continue; }
        if (entry->letter == letter) {
            entry->age = ++cache->clock;
            ++cache->hits; ++profile[0];
            for (unsigned y = 0; y < glyph->box_h; ++y)
                memcpy(output->data + y * output->header.stride,
                       entry->pixels + y * glyph->box_w, glyph->box_w);
            lv_draw_buf_flush_cache(output, NULL);
            profile[4]+=esp_timer_get_time()-bitmap_started;
            return output;
        }
    }
    ++cache->misses; ++profile[1];
    const uint64_t started=esp_timer_get_time();
    lv_font_glyph_dsc_t source;
    if (!source_glyph(cache, letter, &source)) return NULL;
    /* Decode through LVGL rather than assuming SD bitmap packing/compression. */
    lv_draw_buf_t *scratch = lv_draw_buf_create(source.box_w, source.box_h,
                                               LV_COLOR_FORMAT_A8, 0);
    if (!scratch) return NULL;
    const lv_draw_buf_t *decoded = lv_font_get_glyph_bitmap(&source, scratch);
    if (!decoded) {
        lv_font_glyph_release_draw_data(&source);
        lv_draw_buf_destroy(scratch);
        return NULL;
    }
    for (unsigned y = 0; y < glyph->box_h; ++y) {
        const uint8_t *row = decoded->data + (y / cache->scale) * decoded->header.stride;
        uint8_t *destination = output->data + y * output->header.stride;
        for (unsigned x = 0; x < glyph->box_w; ++x)
            destination[x] = row[x / cache->scale];
    }
    lv_font_glyph_release_draw_data(&source);
    lv_draw_buf_destroy(scratch);
    profile[3]+=(uint32_t)(esp_timer_get_time()-started);
    /* Allocation failure only disables caching; the freshly drawn glyph remains. */
    if (bytes <= CACHE_BYTE_LIMIT) {
        while (!empty || cache->bytes + bytes > CACHE_BYTE_LIMIT) {
            glyph_entry_t *oldest = oldest_entry(cache);
            if (!oldest) break;
            evict(cache, oldest);
            if (!empty) empty = oldest;
        }
        if (empty) {
            empty->pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (empty->pixels) {
                empty->letter = letter;
                empty->age = ++cache->clock;
                empty->bytes = bytes;
                cache->bytes += bytes;
                for (unsigned y = 0; y < glyph->box_h; ++y)
                    memcpy(empty->pixels + y * glyph->box_w,
                           output->data + y * output->header.stride, glyph->box_w);
            }
        }
    }
    lv_draw_buf_flush_cache(output, NULL);
    profile[4]+=esp_timer_get_time()-bitmap_started;
    return output;
}

static lv_font_t *create_font(const lv_font_t *source, unsigned scale, bool supplemental)
{
    if (!source) return NULL;
    display_font_t *cache = heap_caps_calloc(1, sizeof(*cache),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cache) return NULL;
    cache->source = source;
    cache->scale = scale;
    cache->supplemental = supplemental;
    cache->font = *source;
    cache->font.user_data = cache;
    cache->font.dsc = NULL;
    cache->font.fallback = NULL;
    cache->font.get_glyph_dsc = glyph_descriptor;
    cache->font.get_glyph_bitmap = glyph_bitmap;
    cache->font.release_glyph = NULL;
    cache->font.line_height = scale *
        (!supplemental || source->line_height > lv_font_asr_symbols.line_height ?
         source->line_height : lv_font_asr_symbols.line_height);
    cache->font.base_line *= scale;
    cache->font.cap_height *= scale;
    cache->font.x_height *= scale;
    cache->font.underline_position *= scale;
    cache->font.underline_thickness *= scale;
    cache->font.kerning = LV_FONT_KERNING_NONE;
    cache->font.static_bitmap = 0;
    return &cache->font;
}

lv_font_t *asr_display_font_create(const lv_font_t *source)
{
    return create_font(source, 2, true);
}

lv_font_t *asr_display_font_create_native(const lv_font_t *source)
{
    return create_font(source, 1, false);
}

void asr_display_font_destroy(lv_font_t *font)
{
    if (!font) return;
    display_font_t *cache = font->user_data;
    ESP_LOGI("asr_font", "cache_hits=%lu cache_misses=%lu evictions=%lu bytes=%u",
             (unsigned long)cache->hits, (unsigned long)cache->misses,
             (unsigned long)cache->evictions, (unsigned)cache->bytes);
    for (unsigned i = 0; i < CACHE_SLOTS; ++i) heap_caps_free(cache->entries[i].pixels);
    heap_caps_free(cache);
}
