"""Prepare user-supplied GIF and a small Chinese bootstrap font; no installs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from PIL import Image, ImageDraw, ImageOps

ROOT = Path(__file__).resolve().parents[1]
STATUS_TEXT = (
    "正在启动正在加载首页字库正在校验14和28字号字库"
    "正在加载14字号字库已加载14字号字库正在加载28字号字库"
    "已加载字库字库未就绪暂用基础字库"
    "正在联网获取资源网络准备完成进入首页后启动语音服务"
    "资源准备完成正在进入首页准备超时未完成资源将继续后台加载"
    "动画未能加载请稍候；，：...%0123456789"
)


def generate(source):
    out = ROOT / "assets/startup"
    out.mkdir(parents=True, exist_ok=True)
    original = Image.open(source)
    original_frames, durations, frames = [], [], []
    original_size = original.size
    for index in range(original.n_frames):
        original.seek(index)
        image = original.convert("RGB")
        # Invert neutral white/black/gray only: black backdrop and visible map
        # lines while leaving the supplied colorful animation subject intact.
        saturation = image.convert("HSV").getchannel("S")
        neutral = saturation.point(lambda value: 255 if value <= 20 else 0)
        original_frames.append(Image.composite(ImageOps.invert(image), image, neutral))
        durations.append(original.info.get("duration", 30))
    # Keep every other composited frame, retaining cycle time and aspect ratio.
    samples = original_frames[::10]
    palette_sheet = Image.new("RGB", (64 * len(samples), 64), "black")
    for column, sample in enumerate(samples):
        palette_sheet.paste(sample.resize((64, 64)), (64 * column, 0))
    palette = palette_sheet.quantize(colors=32, method=Image.Quantize.MEDIANCUT)
    colors = palette.getpalette()
    colors[30 * 3:32 * 3] = [255, 255, 255, 0, 0, 0]
    palette.putpalette(colors)  # Keep neutral map outlines white, not pastel.
    for index in range(0, len(original_frames), 2):
        image = original_frames[index].copy()
        image.thumbnail((320, 320), Image.Resampling.LANCZOS)
        frames.append(image.quantize(palette=palette, dither=Image.Dither.NONE))
    output_durations = [sum(durations[i:i + 2]) for i in range(0, len(durations), 2)]
    asset = out / "loading.gif"
    frames[0].save(asset, save_all=True, append_images=frames[1:],
                   duration=output_durations, loop=0, disposal=1, optimize=True)
    encoded = asset.read_bytes()
    if len(encoded) > 220_000:
        raise RuntimeError("Derived GIF exceeds the firmware asset budget")
    result = {
        "source_sha256": hashlib.sha256(Path(source).read_bytes()).hexdigest(),
        "source_size": original_size, "source_frames": len(original_frames),
        "width": frames[0].width, "height": frames[0].height,
        "frames": len(frames), "cycle_ms": sum(output_durations),
        "frame_durations_ms": output_durations, "loop": 0,
        "bytes": len(encoded), "sha256": hashlib.sha256(encoded).hexdigest(),
        "bootstrap_font_size": 24, "bootstrap_symbols": STATUS_TEXT,
        "background": "black", "neutral_tones_inverted": True,
    }
    # Check actual output after GIF optimization, rather than trusting inputs.
    checked = Image.open(asset)
    if checked.n_frames != len(frames) or checked.info.get("loop") != 0:
        raise RuntimeError("Frame count or loop changed unexpectedly")
    sheet = Image.new("RGB", (960, 360), "black")
    draw = ImageDraw.Draw(sheet)
    for column, index in enumerate([0, len(frames) // 3, 2 * len(frames) // 3]):
        sheet.paste(frames[index].convert("RGB"), (column * 320, 0))
        draw.text((column * 320 + 10, 330), f"Frame {index}", fill="white")
    sheet.save(out / "preview.png")
    (out / "manifest.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n",
                                       encoding="utf-8")
    converter = ROOT / ".font-tools/node_modules/lv_font_conv/lv_font_conv.js"
    font = ROOT / "firmware/p4_main/managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf"
    if not converter.exists():
        raise RuntimeError("E:-local lv_font_conv is required; do not install globally")
    subprocess.run([shutil.which("node"), str(converter), "--no-compress", "--no-prefilter",
                    "--no-kerning", "--size", "24", "--bpp", "4", "--format", "lvgl",
                    "--font", str(font), "-r", "0x20-0x7e", "--symbols", STATUS_TEXT,
                    "--lv-include", "lvgl.h", "--lv-font-name", "lv_font_startup_zh",
                    "-o", str(ROOT / "firmware/p4_main/main/font_startup_zh.c")], check=True)
    print(f"startup_gif bytes={len(encoded)} frames={len(frames)} size={frames[0].size} cycle_ms={sum(output_durations)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    generate(parser.parse_args().source)
