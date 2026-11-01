import hashlib
import json
from pathlib import Path


base = Path(__file__).resolve().parent
meta = json.loads((base / "checksums.json").read_text(encoding="utf-8"))
target = base / meta["archive"]
if target.exists():
    raise SystemExit("Target already exists; refusing to overwrite: " + target.name)
whole = hashlib.sha256()
with target.open("xb") as dst:
    for part in meta["parts"]:
        source = base / part["name"]
        digest = hashlib.sha256()
        count = 0
        with source.open("rb") as src:
            while True:
                block = src.read(8 * 1024 * 1024)
                if not block:
                    break
                digest.update(block)
                whole.update(block)
                dst.write(block)
                count += len(block)
        if count != part["bytes"] or digest.hexdigest() != part["sha256"]:
            raise SystemExit("Invalid volume: " + source.name)
        print("Verified:", source.name, flush=True)
if target.stat().st_size != meta["archive_bytes"] or whole.hexdigest() != meta["archive_sha256"]:
    raise SystemExit("Combined archive hash mismatch")
print("Verified complete ZIP:", target)
