#include "qinghe_ui.h"
#include <stdatomic.h>

/* Each row is a semantic role, never an image/pixel inversion. Keep dark
 * entries distinct so a hidden page can be refreshed lazily in either direction. */
static const uint32_t palette[][2] = {
    {QH_BG, 0x18221E}, {QH_SURFACE, 0x24312B}, {QH_SOFT, 0x354A3D},
    {QH_INK, 0xE7EFE5}, {QH_MUTED, 0xB5C5B2}, {QH_ACCENT, 0x466951},
    {QH_LINE, 0x53665A}, {QH_DANGER, 0x9C4945}, {QH_WARN, 0xE1BC7C},
    {QH_ERROR, 0xF1B0A8}, {QH_LINK, 0xA8C89C}, {QH_ON_ACCENT, 0xF2F8EE}, {QH_USER, 0x3C5544}, {QH_ASSISTANT, 0x293B32},
    /* Legacy home art shades alias the shared roles during migration. */
    {0x26362C, 0xE7EFE5}, {0x687567, 0xB5C5B2}, {0x73816B, 0xB5C5B2},
    {0xE0E5D5, 0x53665A}, {0xC8D6BB, 0x53665A},
    {0x536548, 0xB5C5B2}, {0x65735C, 0xB5C5B2}, {0xEEEEE0, 0x24312B},
    {0xE7ECE1, 0x293B32}, {0x718264, 0x9BB890}, {0xA47548, 0xDCB77D},
    {0x71896A, 0x9BB890}, {0xA1B091, 0x739274}, {0xFFFDF2, 0x526E5B},
};
static atomic_bool dark_theme;
static void (*save_callback)(void);

bool qh_theme_dark(void) { return atomic_load(&dark_theme); }
void qh_theme_restore(bool dark) { atomic_store(&dark_theme, dark); }
void qh_theme_set_save_callback(void (*callback)(void)) { save_callback = callback; }
lv_color_t qh_color(uint32_t semantic)
{
    for (unsigned i=0; i<sizeof(palette)/sizeof(palette[0]); ++i)
        if (palette[i][0] == semantic) return lv_color_hex(palette[i][qh_theme_dark()]);
    return lv_color_hex(semantic);
}

static bool remap(lv_color_t source, lv_color_t *target)
{
    const uint32_t rgb = lv_color_to_u32(source) & 0xFFFFFF;
    for (unsigned i=0; i<sizeof(palette)/sizeof(palette[0]); ++i) {
        for (unsigned side=0; side<2; ++side) {
            lv_color_t base = lv_color_hex(palette[i][side]);
            if (rgb == palette[i][side]) { *target=qh_color(palette[i][0]); return true; }
            if (rgb == (lv_color_to_u32(lv_color_darken(base, LV_OPA_20)) & 0xFFFFFF)) {
                *target=lv_color_darken(qh_color(palette[i][0]), LV_OPA_20); return true;
            }
        }
    }
    return false;
}

void qh_theme_native(void)
{
#if LV_USE_THEME_DEFAULT
    lv_display_t *display=lv_display_get_default();
    if (display) lv_display_set_theme(display, lv_theme_default_init(display,
        qh_color(QH_ACCENT), qh_color(QH_SOFT), qh_theme_dark(), LV_FONT_DEFAULT));
#endif
}

/* Only existing LOCAL color properties are touched. Geometry, state, text,
 * cursor, focus, scroll offset, timers and business objects are untouched.
 * No object registry, per-widget allocation, retained pages or frame buffer. */
void qh_theme_apply(lv_obj_t *root)
{
    if (!root) return;
    static const lv_style_prop_t properties[] = {LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR,
        LV_STYLE_BORDER_COLOR, LV_STYLE_IMAGE_RECOLOR, LV_STYLE_LINE_COLOR,
        LV_STYLE_OUTLINE_COLOR, LV_STYLE_ARC_COLOR};
    static const lv_part_t parts[] = {LV_PART_MAIN, LV_PART_SCROLLBAR, LV_PART_INDICATOR,
        LV_PART_KNOB, LV_PART_SELECTED, LV_PART_ITEMS, LV_PART_CURSOR, LV_PART_TEXTAREA_PLACEHOLDER};
    static const lv_state_t states[] = {0, LV_STATE_PRESSED, LV_STATE_DISABLED,
        LV_STATE_CHECKED, LV_STATE_FOCUSED, LV_STATE_FOCUSED|LV_STATE_PRESSED};
    for (unsigned p=0;p<sizeof(parts)/sizeof(parts[0]);++p)
        for (unsigned s=0;s<sizeof(states)/sizeof(states[0]);++s)
            for (unsigned c=0;c<sizeof(properties)/sizeof(properties[0]);++c) {
                lv_style_value_t value;
                const lv_style_selector_t selector=parts[p]|states[s];
                if (lv_obj_get_local_style_prop(root,properties[c],&value,selector) != LV_STYLE_RES_FOUND) continue;
                lv_color_t color;
                if (remap(value.color,&color) && !lv_color_eq(value.color,color)) {
                    value.color=color;
                    lv_obj_set_local_style_prop(root,properties[c],value,selector);
                }
            }
    for (uint32_t i=0;i<lv_obj_get_child_count(root);++i) qh_theme_apply(lv_obj_get_child(root,i));
}
void qh_theme_toggle(lv_event_t *event)
{
    (void)event;
    qh_theme_restore(!qh_theme_dark());
    qh_theme_native();
    qh_theme_apply(lv_screen_active());
    qh_theme_apply(lv_layer_top());
    lv_obj_invalidate(lv_screen_active());
    if (save_callback) save_callback();
}
