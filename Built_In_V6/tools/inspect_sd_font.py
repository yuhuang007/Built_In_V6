"""Inspect actual LVGL binary cmap/metrics, not just requested source ranges."""
from pathlib import Path
import struct


def inspect_font(path):
    data = Path(path).read_bytes()
    tables = {}
    offset = 0
    while offset < len(data):
        length, name = struct.unpack_from("<I4s", data, offset)
        if length < 8 or offset + length > len(data) or name in tables:
            raise ValueError("invalid table")
        tables[name] = data[offset:offset + length]
        offset += length
    head = tables[b"head"]
    cmap = tables[b"cmap"]
    count = struct.unpack_from("<I", cmap, 8)[0]
    mapping = {}
    for index in range(count):
        start, first, span, glyph, entries, kind = struct.unpack_from("<IIHHHB", cmap, 12 + index * 16)
        if kind == 2:
            mapping.update({first + i: glyph + i for i in range(span)})
        elif kind == 0:
            for i in range(entries):
                delta = cmap[start + i]
                if i == 0 or delta:
                    mapping[first + i] = glyph + delta
        elif kind in (1, 3):
            for i in range(entries):
                delta = struct.unpack_from("<H", cmap, start + i * 2)[0]
                gid = i if kind == 3 else struct.unpack_from("<H", cmap, start + entries * 2 + i * 2)[0]
                mapping[first + delta] = glyph + gid
        else:
            raise ValueError("invalid cmap type")
    loca = tables[b"loca"]
    loca_count = struct.unpack_from("<I", loca, 8)[0]
    if max(mapping.values()) >= loca_count:
        raise ValueError("glyph index exceeds loca")
    return {"size_px": struct.unpack_from("<H", head, 14)[0],
            "bpp": head[37], "compression": head[41],
            "ascent": struct.unpack_from("<H", head, 16)[0],
            "descent": struct.unpack_from("<h", head, 18)[0],
            "mapping": mapping, "bytes": len(data)}
