"""Explicit, bounded provisioning of only the public P4 Pinyin dictionary."""
import argparse
import sd_font_server as transfer

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--minutes", type=int, default=10)
    args = parser.parse_args()
    if not args.serve or not 1 <= args.minutes <= 30:
        parser.error("explicit --serve and lifetime 1..30 minutes required")
    transfer.ASSETS = transfer.ROOT / "assets/pinyin"
    transfer.ALLOWED = {"P4PYV1.DAT"}
    try:
        transfer.serve(args.minutes, "pinyin-manifest.json")
    except KeyboardInterrupt:
        print("pinyin_server_closed=1", flush=True)
    except Exception as error:
        print(f"pinyin_server_failed type={type(error).__name__}", flush=True)
        raise SystemExit(1)
