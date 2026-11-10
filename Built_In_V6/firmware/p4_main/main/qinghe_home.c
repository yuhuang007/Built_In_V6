#define QH_HOME_LAYOUT_DATA
#include "qinghe_home.h"
#include <string.h>
#include "qinghe_home_art.h"
#include "qinghe_ui.h"

extern const lv_image_dsc_t qinghe_logo_64;

static const lv_image_dsc_t *const home_images[QHH_COUNT] = {
    [QHH_LOGO] = &qinghe_logo_64, [QHH_SETTINGS_ICON] = &qh_home_settings,
    [QHH_HELP_ARROW] = &qh_home_arrow_small, [QHH_SLOGAN] = &qh_home_slogan,
    [QHH_MIC] = &qh_home_mic, [QHH_VOICE_ARROW] = &qh_home_arrow,
    [QHH_TRANSCRIPT_ICON] = &qh_home_document, [QHH_TRANSCRIPT_ARROW] = &qh_home_arrow,
    [QHH_CAMERA_ICON] = &qh_home_camera, [QHH_CAMERA_ARROW] = &qh_home_arrow,
    [QHH_USB_ICON] = &qh_home_keyboard,
};

/* All positions in the generated table are absolute logical coordinates.
 * Keep labels inside their actual clickable parent, without independent hits. */
static void place(qh_home_t *home, int id, int x, int y, int w, int h)
{
    const int parent = qhh_items[id].parent;
    if (parent >= 0) {
        lv_obj_t *owner = home->objects[parent];
        lv_area_t owner_box;
        lv_obj_get_coords(owner, &owner_box);
        x -= owner_box.x1;
        y -= owner_box.y1;
        /* LVGL's content origin includes the border but not our zero padding. */
        x -= lv_obj_get_style_border_width(owner, 0);
        y -= lv_obj_get_style_border_width(owner, 0);
    }
    lv_obj_set_size(home->objects[id], w, h);
    lv_obj_set_pos(home->objects[id], x, y);
}

static void waveform_draw(lv_event_t *event)
{
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = 5;
    for (unsigned i = 0; i < 11; ++i) {
        const int h = qhh_wave_heights[i];
        const int x = bounds.x1 + 20 * i;
        const int y = bounds.y1 + (106 - h) / 2;
        const lv_area_t area = {x, y, x + 9, y + h - 1};
        bar.bg_color = qh_color(qhh_wave_colors[i]);
        lv_draw_rect(lv_event_get_layer(event), &bar, &area);
    }
}

/* Compare actual visible glyph bounds, keeping each label's full line box. */
static void ink_bounds(lv_obj_t *label,int *top,int *bottom)
{
    const lv_font_t *font=lv_obj_get_style_text_font(label,0);
    *top=font->line_height;*bottom=0;
    const unsigned char *text=(const unsigned char*)lv_label_get_text(label);
    while(*text){
        uint32_t cp=*text++;unsigned extra=0;
        if(cp>=0xF0){cp&=7;extra=3;}else if(cp>=0xE0){cp&=15;extra=2;}else if(cp>=0xC0){cp&=31;extra=1;}
        while(extra-- && *text)cp=(cp<<6)|(*text++&63);
        lv_font_glyph_dsc_t glyph;
        if(lv_font_get_glyph_dsc(font,&glyph,cp,0) && glyph.box_h){
            int b=font->line_height-font->base_line-glyph.ofs_y;
            int t=b-glyph.box_h;if(t<*top)*top=t;if(b>*bottom)*bottom=b;
        }
    }
    if(*bottom<=*top){*top=0;*bottom=font->line_height;}
}
static int label_width(lv_obj_t *label)
{
    lv_point_t size;
    lv_text_get_size(&size,lv_label_get_text(label),lv_obj_get_style_text_font(label,0),
        lv_obj_get_style_text_letter_space(label,0),0,600,LV_TEXT_FLAG_NONE);
    return size.x;
}
static void align_home_groups(qh_home_t *home)
{
    lv_obj_t *product=home->objects[QHH_PRODUCT],*subtitle=home->objects[QHH_SUBTITLE];
    int pt,pb,st,sb;ink_bounds(product,&pt,&pb);ink_bounds(subtitle,&st,&sb);
    const int group_height=pb-pt+6+sb-st;
    const int visible_top=lv_obj_get_y(home->objects[QHH_LOGO])+(64-group_height)/2;
    lv_obj_set_y(product,visible_top-pt);lv_obj_set_y(subtitle,visible_top+pb-pt+6-st);
    lv_obj_t *connection=home->objects[QHH_CONNECTION];
    const int w=label_width(connection);
    lv_obj_set_width(connection,w);lv_obj_set_x(connection,570-w);
    lv_obj_set_pos(home->objects[QHH_CONNECTION_DOT],570-w-12,
        lv_obj_get_y(connection)+(lv_obj_get_height(connection)-5)/2);
    lv_obj_t *usb=home->objects[QHH_USB_CAPTION];const int text_width=label_width(usb);
    const int start=(600-(24+12+text_width))/2;
    lv_obj_set_width(usb,text_width);lv_obj_set_x(usb,start+36);
    lv_obj_set_x(home->objects[QHH_USB_ICON],start);
}

void qh_home_set_fonts(qh_home_t *home, const lv_font_t *body, const lv_font_t *heading)
{
    if (!home->screen || !body || !heading) return;
    const bool large = body->line_height >= 54;
    for (int i=0;i<QHH_COUNT;++i) {
        const qhh_item_t *item=&qhh_items[i];
        lv_obj_update_layout(home->screen);
        place(home,i,large?item->x28:item->x,large?item->y28:item->y,
              large?item->w28:item->w,large?item->h28:item->h);
    }
    if(home->theme)lv_obj_set_pos(home->theme,456,large?58:48);
    for (int i = 0; i < QHH_COUNT; ++i) {
        const qhh_item_t *item = &qhh_items[i];
        if (item->kind != QHH_LABEL) continue;
        const lv_font_t *font = item->font == QHH_FONT_HEADING ? heading :
                               item->font == QHH_FONT_LATIN ? LV_FONT_DEFAULT : body;
        lv_obj_t *label = home->objects[i];
        lv_obj_set_style_text_font(label, font, 0);
        const int x=large?item->x28:item->x,y=large?item->y28:item->y;
        const int w=large?item->w28:item->w,h=large?item->h28:item->h;
        /* An LVGL label has no vertical alignment: center its natural line box
         * inside the design allocation, including the actual 27/54px metrics. */
        place(home, i, x, y, w, LV_SIZE_CONTENT);
        lv_obj_update_layout(label);
        place(home, i, x, y + (h - lv_obj_get_height(label)) / 2,
              w, LV_SIZE_CONTENT);
    }
    align_home_groups(home);
}

void qh_home_create(qh_home_t *home, const lv_font_t *body, const lv_font_t *heading,
                    lv_event_cb_t voice, lv_event_cb_t transcript, lv_event_cb_t camera,
                    lv_event_cb_t settings, lv_event_cb_t help)
{
    home->screen = lv_obj_create(NULL);
    home->readiness_cache = -1;
    qh_screen(home->screen);
    for (int i = 0; i < QHH_COUNT; ++i) {
        const qhh_item_t *item = &qhh_items[i];
        lv_obj_t *parent = item->parent < 0 ? home->screen : home->objects[item->parent];
        lv_obj_t *object;
        if (item->kind == QHH_LABEL) {
            object = qh_label(parent, body, item->text, item->color);
            lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(object, item->align == QHH_ALIGN_CENTER ? LV_TEXT_ALIGN_CENTER :
                item->align == QHH_ALIGN_RIGHT ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        } else if (item->kind == QHH_IMAGE) {
            object = lv_image_create(parent);
            lv_image_set_src(object, home_images[i]);
            if (i != QHH_LOGO) {
                lv_obj_set_style_image_recolor(object, qh_color(item->color), 0);
                lv_obj_set_style_image_recolor_opa(object, LV_OPA_COVER, 0);
            }
        } else if (item->kind == QHH_BUTTON) {
            object = lv_button_create(parent);
            if(i==QHH_HELP){
                /* A transparent hit area, with no default button shadow/border. */
                lv_obj_remove_style_all(object);
                lv_obj_set_style_bg_opa(object,LV_OPA_TRANSP,0);
            } else qh_button(object, item->color);
            lv_obj_set_style_radius(object, item->radius, 0);
        } else {
            object = lv_obj_create(parent);
            lv_obj_remove_style_all(object);
            if (item->kind == QHH_WAVE)
                lv_obj_add_event_cb(object, waveform_draw, LV_EVENT_DRAW_MAIN, NULL);
            else qh_panel(object, item->color, item->radius);
        }
        home->objects[i] = object;
        lv_obj_set_style_pad_all(object, 0, 0);
        lv_obj_set_scrollable(object, false);
        lv_obj_set_clickable(object, item->kind == QHH_BUTTON);
        if (item->border) {
            lv_obj_set_style_border_width(object, 1, 0);
            lv_obj_set_style_border_color(object, qh_color(item->border), 0);
        }
        if (item->font == QHH_FONT_LATIN) lv_obj_add_flag(object, LV_OBJ_FLAG_USER_1);
        /* Resolve parent coordinates before converting the next child's box. */
        lv_obj_update_layout(home->screen);
        place(home, i, item->x, item->y, item->w, item->h);
    }
    const struct { int id; lv_event_cb_t callback; } actions[] = {
        {QHH_VOICE, voice}, {QHH_TRANSCRIPT, transcript}, {QHH_CAMERA, camera},
        {QHH_SETTINGS, settings}, {QHH_HELP, help},
    };
    for (unsigned i = 0; i < sizeof(actions)/sizeof(actions[0]); ++i) {
        if (actions[i].callback)
            lv_obj_add_event_cb(home->objects[actions[i].id], actions[i].callback, LV_EVENT_CLICKED, NULL);
        else lv_obj_add_state(home->objects[actions[i].id], LV_STATE_DISABLED);
    }
    home->theme=qh_theme_button(home->screen);
    qh_home_set_fonts(home, body, heading);
}

static void set_label(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

void qh_home_set_readiness(qh_home_t *home, bool wifi, bool cloud, bool welcome_playing)
{
    if (!home->screen) return;
    const int state = (wifi ? 1 : 0) | (cloud ? 2 : 0) | (welcome_playing ? 4 : 0);
    if (state == home->readiness_cache) return;
    home->readiness_cache = state;
    set_label(home->objects[QHH_CONNECTION], !wifi ? "未联网" : !cloud ? "服务准备中" : "语音已就绪");
    set_label(home->objects[QHH_STATE], !wifi ? "请先连接 Wi-Fi" : !cloud ? "语音准备中" :
              welcome_playing ? "欢迎播报中" : "准备好交流");
    align_home_groups(home);
    const lv_color_t dot = qh_color(wifi && cloud ? 0x718264 : 0xA47548);
    lv_obj_set_style_bg_color(home->objects[QHH_CONNECTION_DOT], dot, 0);
    lv_obj_set_style_bg_color(home->objects[QHH_STATE_DOT], dot, 0);
}
