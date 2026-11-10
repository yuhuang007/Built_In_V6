"""Generate the native 64px P4 icon; offline asset tool, requires Pillow."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "assets/brand"
image = Image.open(BRAND / "native-user-icon.png").convert("RGBA")
image = image.resize((64, 64), Image.Resampling.LANCZOS)
image.save(BRAND / "native-icon-64.png")
color, alpha = bytearray(), bytearray()
for r, g, b, a in image.getdata():
    value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    color.extend((value & 255, value >> 8))
    alpha.append(a)
data = color + alpha
lines = ['#include "lvgl.h"\n', 'static const uint8_t native_icon_data[] = {\n']
for offset in range(0, len(data), 24):
    lines.append(",".join(f"0x{value:02x}" for value in data[offset:offset + 24]) + ",\n")
lines.extend(['};\n', 'const lv_image_dsc_t qinghe_logo_64 = {\n',
              ' .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565A8,\n',
              ' .w=64,.h=64,.stride=128},.data_size=sizeof(native_icon_data),.data=native_icon_data\n',
              '};\n'])
(ROOT / "firmware/p4_main/main/qinghe_logo.c").write_text("".join(lines), encoding="utf-8")
