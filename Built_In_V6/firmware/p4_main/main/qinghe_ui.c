#include "qinghe_ui.h"
#include "ui_layout.h"
#include "qinghe_home_art.h"

extern const lv_image_dsc_t qinghe_logo_64;
static lv_event_cb_t settings_callback;

void qh_panel(lv_obj_t *object, uint32_t color, int radius)
{
    lv_obj_set_style_bg_color(object, qh_color(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(object, 0, 0);
}
void qh_screen(lv_obj_t *screen)
{
    qh_panel(screen, QH_BG, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_text_color(screen, qh_color(QH_INK), 0);
    lv_obj_set_scrollable(screen, false);
}
void qh_button(lv_obj_t *object, uint32_t color)
{
    qh_panel(object, color, 24);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_style_text_color(object, qh_color(color == QH_ACCENT || color == QH_DANGER ? QH_ON_ACCENT : QH_INK), 0);
    lv_obj_set_style_bg_color(object, lv_color_darken(qh_color(color), LV_OPA_20), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(object, LV_OPA_50, LV_STATE_DISABLED);
}
lv_obj_t *qh_label(lv_obj_t *parent, const lv_font_t *font, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, qh_color(color), 0);
    lv_label_set_text(label, text);
    if (font == LV_FONT_DEFAULT) lv_obj_add_flag(label, LV_OBJ_FLAG_USER_1);
    return label;
}
lv_obj_t *qh_logo(lv_obj_t *parent)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, &qinghe_logo_64);
    return image;
}
/* The exact 9x9 Qinghe motif from the selected prototype, packed into 11 bytes.
 * One draw object, no 81 child objects, bitmap cache or persistent timer. */
static const uint8_t startup_leaf_bits[11] = {
    0x10,0x70,0x58,0xbb,0x6e,0x0d,0x02,0x04,0x3e,0x10,0x00
};
static void startup_leaf_draw(lv_event_t *event)
{
    lv_obj_t *object = lv_event_get_target_obj(event);
    const unsigned phase = (unsigned)(uintptr_t)lv_obj_get_user_data(object);
    const unsigned sweep=phase<=8?phase:16-phase;
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.border_color = qh_color(QH_ACCENT);
    dot.border_width = 1;
    for (unsigned row = 0; row < 9; ++row) {
        for (unsigned column = 0; column < 9; ++column) {
            const unsigned bit = row * 9 + column;
            const bool leaf = (startup_leaf_bits[bit / 8] >> (bit % 8)) & 1;
            const int radius = leaf ? 6 : 1;
            const unsigned distance=row>sweep?row-sweep:sweep-row;
            dot.bg_color = qh_color(!leaf ? QH_LINE : distance==0 ? QH_ACCENT : distance==1 ? QH_MUTED : QH_LINE);
            const int x = bounds.x1 + 12 + column * 24;
            const int y = bounds.y1 + 12 + row * 24;
            const lv_area_t area = {x-radius, y-radius, x+radius, y+radius};
            lv_draw_rect(layer, &dot, &area);
        }
    }
}
lv_obj_t *qh_startup_leaf(lv_obj_t *parent)
{
    lv_obj_t *leaf = lv_obj_create(parent);
    lv_obj_remove_style_all(leaf);
    lv_obj_set_size(leaf, 216, 216);
    lv_obj_set_scrollable(leaf, false);
    lv_obj_set_clickable(leaf, false);
    lv_obj_set_user_data(leaf, NULL);
    lv_obj_add_event_cb(leaf, startup_leaf_draw, LV_EVENT_DRAW_MAIN, NULL);
    return leaf;
}
void qh_startup_leaf_step(lv_obj_t *object, uint32_t elapsed_ms)
{
    unsigned phase = (elapsed_ms / 200) % 16;
    if ((unsigned)(uintptr_t)lv_obj_get_user_data(object) == phase) return;
    lv_obj_set_user_data(object, (void *)(uintptr_t)phase);
    lv_obj_invalidate(object);
}
void qh_set_settings_callback(lv_event_cb_t callback) { settings_callback = callback; }
lv_obj_t *qh_settings_button(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    qh_button(button, QH_SOFT);
    lv_obj_set_size(button, QH_TOOL_SIZE, QH_TOOL_SIZE);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -20, 12);
    lv_obj_t *label = lv_image_create(button);
    lv_image_set_src(label, &qh_home_settings);
    lv_obj_set_style_image_recolor(label, qh_color(QH_INK), 0);
    lv_obj_set_style_image_recolor_opa(label, LV_OPA_COVER, 0);
    lv_obj_center(label);
    if (settings_callback) lv_obj_add_event_cb(button, settings_callback, LV_EVENT_CLICKED, NULL);
    return button;
}

static void icon_line(lv_layer_t *layer, int x, int y, int x1, int y1, int x2, int y2)
{
    lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
    line.color=qh_color(QH_INK); line.width=2; line.round_start=line.round_end=1;
    line.p1.x=x+x1; line.p1.y=y+y1; line.p2.x=x+x2; line.p2.y=y+y2;
    lv_draw_line(layer,&line);
}
static void icon_draw(lv_event_t *event)
{
    lv_obj_t *object=lv_event_get_target_obj(event);
    unsigned kind=(unsigned)(uintptr_t)lv_obj_get_user_data(object);
    lv_area_t a; lv_obj_get_coords(object,&a);
    int x=a.x1,y=a.y1; lv_layer_t *layer=lv_event_get_layer(event);
    lv_draw_rect_dsc_t r;lv_draw_rect_dsc_init(&r);
    r.border_color=qh_color(QH_INK);r.border_width=2;r.bg_opa=LV_OPA_TRANSP;
    if (kind==QH_ICON_BACK) {
        icon_line(layer,x,y,20,6,10,16);icon_line(layer,x,y,10,16,20,26);return;
    }
    if (kind==QH_ICON_THEME) {
        if (!qh_theme_dark()) {
            /* Closed crescent outline: no background-colored erasing mask. */
            static const int points[][2]={{20,5},{14,5},{9,8},{6,13},{6,19},{9,24},{14,27},
                {20,27},{25,24},{27,20},{22,22},{17,21},{13,18},{12,13},{14,8},{20,5}};
            for(unsigned i=1;i<sizeof(points)/sizeof(points[0]);++i)
                icon_line(layer,x,y,points[i-1][0],points[i-1][1],points[i][0],points[i][1]);
        } else {
            r.radius=LV_RADIUS_CIRCLE;
            lv_area_t b={x+8,y+8,x+24,y+24};lv_draw_rect(layer,&r,&b);
            static const int rays[8][4]={{16,2,16,5},{16,27,16,30},{2,16,5,16},{27,16,30,16},
                {5,5,7,7},{25,25,27,27},{5,27,7,25},{25,7,27,5}};
            for(unsigned i=0;i<8;++i)icon_line(layer,x,y,rays[i][0],rays[i][1],rays[i][2],rays[i][3]);
        }
        return;
    }
    if (kind==QH_ICON_USER) {
        r.radius=LV_RADIUS_CIRCLE;lv_area_t b={x+11,y+3,x+21,y+13};lv_draw_rect(layer,&r,&b);
        r.radius=6;b=(lv_area_t){x+5,y+19,x+27,y+29};lv_draw_rect(layer,&r,&b);return;
    }
    if (kind==QH_ICON_ROBOT) {
        r.radius=5;lv_area_t b={x+4,y+10,x+28,y+28};lv_draw_rect(layer,&r,&b);
        icon_line(layer,x,y,16,3,16,9);
        r.border_width=0;r.bg_opa=LV_OPA_COVER;r.bg_color=qh_color(QH_INK);r.radius=2;
        b=(lv_area_t){x+10,y+16,x+13,y+19};lv_draw_rect(layer,&r,&b);
        b=(lv_area_t){x+20,y+16,x+23,y+19};lv_draw_rect(layer,&r,&b);
        icon_line(layer,x,y,12,24,21,24);return;
    }
    /* Small document/guide marks, no special font glyphs. */
    r.radius=4;lv_area_t b={x+6,y+3,x+26,y+29};lv_draw_rect(layer,&r,&b);
    for(int row=10;row<=23;row+=6)icon_line(layer,x,y,11,row,21,row);
}
lv_obj_t *qh_icon(lv_obj_t *parent, unsigned kind)
{
    lv_obj_t *icon=lv_obj_create(parent);lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon,32,32);lv_obj_set_scrollable(icon,false);lv_obj_set_clickable(icon,false);
    if(kind==QH_ICON_DOCUMENT || kind==QH_ICON_CAMERA || kind==QH_ICON_KEYBOARD) {
        lv_obj_t *image=lv_image_create(icon);
        lv_image_set_src(image,kind==QH_ICON_CAMERA?&qh_home_camera:kind==QH_ICON_KEYBOARD?&qh_home_keyboard:&qh_home_document);
        lv_obj_set_style_image_recolor(image,qh_color(QH_INK),0);
        lv_obj_set_style_image_recolor_opa(image,LV_OPA_COVER,0);lv_obj_center(image);
    } else {
        lv_obj_set_user_data(icon,(void *)(uintptr_t)kind);
        lv_obj_add_event_cb(icon,icon_draw,LV_EVENT_DRAW_MAIN,NULL);
    }
    return icon;
}
lv_obj_t *qh_theme_button(lv_obj_t *parent)
{
    lv_obj_t *button=lv_button_create(parent);qh_button(button,QH_SOFT);
    lv_obj_set_size(button,QH_TOOL_SIZE,QH_TOOL_SIZE);
    lv_obj_set_pos(button,456,QH_TOOL_Y);
    lv_obj_center(qh_icon(button,QH_ICON_THEME));
    lv_obj_add_event_cb(button,qh_theme_toggle,LV_EVENT_CLICKED,NULL);return button;
}
lv_obj_t *qh_back_button(lv_obj_t *parent, lv_event_cb_t callback)
{
    lv_obj_t *button=lv_button_create(parent);qh_button(button,QH_SOFT);
    lv_obj_set_size(button,QH_TOOL_SIZE,QH_TOOL_SIZE);lv_obj_set_pos(button,20,QH_TOOL_Y);
    lv_obj_center(qh_icon(button,QH_ICON_BACK));
    lv_obj_add_event_cb(button,callback,LV_EVENT_CLICKED,NULL);return button;
}
static void header_title_align(lv_event_t *event)
{
    lv_obj_t *label=lv_event_get_target_obj(event);
    const lv_font_t *font=lv_obj_get_style_text_font(label,0);
    const int y=QH_TOOL_Y+(QH_TOOL_SIZE-font->line_height)/2;
    if(lv_obj_get_y(label)!=y)lv_obj_set_y(label,y);
}
lv_obj_t *qh_header(lv_obj_t *parent,const char *title,const lv_font_t *font,
                    lv_event_cb_t back,bool settings)
{
    if(back)qh_back_button(parent,back);
    lv_obj_t *label=qh_label(parent,font,title,QH_INK);
    lv_obj_set_pos(label,92,QH_TOOL_Y+(QH_TOOL_SIZE-font->line_height)/2);lv_obj_set_width(label,284);
    lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
    lv_obj_add_event_cb(label,header_title_align,LV_EVENT_STYLE_CHANGED,NULL);
    qh_theme_button(parent);if(settings)qh_settings_button(parent);return label;
}
/* Static-home phase: restore ordinary synchronous screen ownership.
 * No top-layer gate, animated curtain, deferred deletion or redraw timer. */
void qh_screen_load(lv_obj_t *screen, bool back)
{
    (void)back;
    if (screen && screen != lv_screen_active()) { qh_theme_apply(screen); lv_screen_load(screen); }
}
void qh_screen_load_camera(lv_obj_t *screen, bool back) { qh_screen_load(screen, back); }
bool qh_navigation_busy(void) { return false; }
void qh_reveal(lv_obj_t *object) { (void)object; }
