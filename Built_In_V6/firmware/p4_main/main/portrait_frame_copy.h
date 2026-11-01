#pragma once

#include <stdint.h>

/* Same 90-degree mapping as LVGL's touch/area transform. Small tiles keep
 * both source and destination cache lines local during the transpose. */
static inline void portrait_frame_copy(uint16_t *dst, int panel_width,
                                      int panel_height, const uint16_t *src,
                                      int src_stride, int x1, int y1,
                                      int x2, int y2)
{
    for (int ty = y1; ty <= y2; ty += 16) {
        for (int tx = x1; tx <= x2; tx += 16) {
            const int end_y = ty + 15 < y2 ? ty + 15 : y2;
            const int end_x = tx + 15 < x2 ? tx + 15 : x2;
            for (int x = tx; x <= end_x; ++x) {
                uint16_t *out = dst + (panel_height - 1 - x) * panel_width;
                for (int y = ty; y <= end_y; ++y) {
                    out[y] = src[(y - y1) * src_stride + x - x1];
                }
            }
        }
    }
}
