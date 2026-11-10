"""Bounded COM20 observation: emit only fixed counters, never raw serial text."""
import argparse
import json
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM20")
    parser.add_argument("--seconds", type=int, default=60, choices=range(1, 61))
    parser.add_argument("--reset", action="store_true")
    args = parser.parse_args()
    markers = {
        "portrait_600x1024": "portrait: logical=600x1024 rotation=90",
        "complete_frame_scanout": "complete-frame scanout: two LCD buffers, VSYNC retirement",
        "display_underrun": "underrun happens",
        "startup_gif_loaded": "startup_gif loaded=1",
        "home_visible": "startup_home_visible=1",
        "fonts_ready": "home_font_v2 bound=1",
        "wifi_connected": "network: connected to Wi-Fi; address omitted",
        "rtc_requested": "cloud: device registered and RTC session requested",
        "rtc_connected": "cloud: Volcano Engine RTC connected",
        "wifi_reconnecting": "network: disconnected; reconnecting",
        "sntp_failed": "cloud: SNTP time synchronization failed",
        "rtc_dns_failed": "rtc-probe: DNS failed",
        "rtc_tcp_ok": "rtc-probe: TCP/443 reachable",
        "rtc_tcp_failed": "rtc-probe: TCP/443 unavailable",
        "rtc_registration_failed": "cloud: device registration failed",
        "tls_handshake_failed": "mbedtls_ssl_handshake returned",
        "pcm_playback_started": "audio: cloud PCM playback started",
        "welcome_gate_ready": "startup_gate_ready=1",
        "boot_banner": "Built_In_V6 P4X audio bring-up",
        "usb_keyboard_ready": "usb_keyboard: ready=1",
        "text_editor_opened": "text_entry: opened=1",
        "fixed_ui_glyph_probe_ok": "text_entry: fixed_ui_glyphs_visible=2 expected=2",
        "fixed_ui_glyph_probe_failed": "text_entry: fixed_ui_glyphs_visible=0 expected=2",
        "text_editor_completed": "text_entry: completed=1",
        "text_editor_failed": "text_entry: completed=0",
        "pinyin_ready": "pinyin: init_error=0 ready=1",
        "pinyin_asset_installed": "pinyin: asset_error=0",
        "pinyin_query": "pinyin: query_count=",
        "pinyin_ui_result": "text_entry: pinyin_result=1 matched=1",
        "pinyin_ui_timeout": "text_entry: pinyin_timeout=1",
        "pinyin_preloaded": "startup_pinyin state=2",
        "input_dock_collapsed": "text_entry: dock_height=64",
        "input_dock_touch": "text_entry: dock_height=300",
        "local_transcript_message": "speech_text: local_message_appended=1",
        "asr_entry_attempt": "asr_entry wifi=",
        "asr_page_ready": "speech_text: mode_ready=1",
        "panic": "Guru Meditation Error",
        "assertion": "assert failed:",
        "abort": "abort() was called",
        "allocation_failure": "Not enough memory",
        "stack_overflow": "Stack canary watchpoint triggered",
    }
    counts = dict.fromkeys(markers, 0)
    first_seen = {}
    startup_home = None
    preview_samples = []
    preview_stages = []
    preview_awb = []
    panic_diagnostics = []
    editor_health = []
    asr_entries = []
    asr_failures = []
    asr_font_results = []
    network_health = []
    wifi_worker_health = []
    wifi_disconnect_reasons = []
    tls_errors = []
    ui_perf = []
    display_health = []
    display_power_events = []
    counts["error_lines"] = 0
    counts["touch_out_of_bounds"] = 0
    device = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    device.dtr = False
    device.rts = False
    device.port = args.port
    with device:
        if args.reset:
            # Use the same normal-boot reset as esptool, never enter download mode.
            from esptool.reset import HardReset
            HardReset(device)()
        started = time.monotonic()
        pending = b""
        while time.monotonic() - started < args.seconds:
            pending += device.read(min(max(device.in_waiting, 1), 4096))
            while b"\n" in pending:
                raw, pending = pending.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace")
                awb = re.search(r"sign_preview: AWB seq=(\d+) patches=(\d+) measured_r=(\d+\.\d+) measured_b=(\d+\.\d+) applied_r=(\d+\.\d+) applied_b=(\d+\.\d+)", line)
                if awb:
                    sequence, patches, measured_r, measured_b, applied_r, applied_b = awb.groups()
                    preview_awb.append({"sequence": int(sequence), "patches": int(patches),
                                        "measured_r": float(measured_r), "measured_b": float(measured_b),
                                        "applied_r": float(applied_r), "applied_b": float(applied_b),
                                        "observed_s": round(time.monotonic() - started, 3)})
                    flags = re.search(r" applied_b=\d+\.\d+ flags=(\d+)", line)
                    if flags:
                        preview_awb[-1]["flags"] = int(flags.group(1))
                    preview_awb = preview_awb[-60:]
                if "portrait_display: ui_perf " in line:
                    allowed={"scene","ms","frames","chunks","pixels","submits","frame_us","frame_max","layout_us","draw_us","flush_us","copy_us","sync_us","submit_us","reads","read_us","read_max","touch_gap","gaps","hits","misses","evicts","decode_us","bitmap_us","descriptor_us","stack_min"}
                    sample={key:int(value) for key,value in re.findall(r"(\w+)=(\d+)",line) if key in allowed}
                    sample["observed_s"]=round(time.monotonic()-started,3)
                    ui_perf.append(sample);ui_perf=ui_perf[-16:]
                display = re.search(r"display_health dma=(\d+) submitted=(\d+) max_gap_us=(\d+) late=(\d+) late_gap_us=(\d+) late_age_us=(\d+) pwm=(\d+) timeout_s=(\d+)", line)
                if display:
                    sample = dict(zip(("dma", "submitted", "max_gap_us", "late", "late_gap_us", "late_age_us", "pwm", "timeout_s"), map(int, display.groups())))
                    sample["observed_s"] = round(time.monotonic() - started, 3)
                    ui_copy = re.search(r" ui_cpu=(\d+) ui_done=(\d+)", line)
                    if ui_copy:
                        sample.update(zip(("ui_cpu", "ui_done"), map(int, ui_copy.groups())))
                    display_health.append(sample)
                    display_health = display_health[-60:]
                power = re.search(r"screen_idle: (?:gate_busy=\d+ held=\d+ timeout_s=\d+|asleep=\d+ timeout_s=\d+|awake=\d+ brightness=\d+|(?:sleep|wake)_error=-?\d+)|Setting LCD backlight: \d+%", line)
                if power:
                    display_power_events.append({"observed_s": round(time.monotonic() - started, 3), "event": power.group(0)})
                    display_power_events = display_power_events[-60:]
                disconnect = re.search(r"network_disconnect reason=(\d+)", line)
                if disconnect:
                    wifi_disconnect_reasons.append(int(disconnect.group(1)))
                    wifi_disconnect_reasons = wifi_disconnect_reasons[-30:]
                wifi_worker = re.search(r"wifi_settings: health started=(\d+) paused=(\d+) online=(\d+) state=(\d+) saved=(\d+) attempts=(\d+) error=(-?\d+)", line)
                if wifi_worker:
                    wifi_worker_health.append(dict(zip(("started", "paused", "online", "state", "saved", "attempts", "error"), map(int, wifi_worker.groups()))))
                    wifi_worker_health = wifi_worker_health[-10:]
                failure = re.search(r"speech_text: entry_failed stage=(\d+) error=(-?\d+) internal=(\d+) largest=(\d+)", line)
                if failure:
                    asr_failures.append(dict(zip(("stage", "error", "internal", "largest"), map(int, failure.groups()))))
                    asr_failures = asr_failures[-10:]
                fonts = re.search(r"speech_text: page_fonts reused=(\d+) ready=(\d+) stage=(\d+) error=(-?\d+) duration_ms=(\d+)", line)
                if fonts:
                    asr_font_results.append(dict(zip(("reused", "ready", "stage", "error", "duration_ms"), map(int, fonts.groups()))))
                    asr_font_results = asr_font_results[-10:]
                tls = re.search(r"(mbedtls_(?:ctr_drbg_seed|x509_crt_parse|ssl_set_hostname|ssl_config_defaults|ssl_setup|net_connect|ssl_handshake)) error,?\s*return (-?0x[0-9a-fA-F]+)", line)
                if tls:
                    tls_errors.append({"stage": tls.group(1), "code": tls.group(2)})
                    tls_errors = tls_errors[-10:]
                net = re.search(r"network_health wifi=(\d+) rtc=(\d+) engine=(\d+) register=(\d+) recovery=(\d+) suspended=(\d+) clock=(\d+) internal=(\d+) largest=(\d+)", line)
                if net:
                    network_health.append(dict(zip(("wifi", "rtc", "engine", "register", "recovery", "suspended", "clock", "internal", "largest"), map(int, net.groups()))))
                    network_health = network_health[-10:]
                entry = re.search(r"asr_entry wifi=(\d+) mic=(\d+) audio=(\d+) lock=(\d+) capture=(\d+) finish=(\d+) loop=(\d+) register=(\d+) recovery=(\d+) font=(\d+)", line)
                if entry:
                    asr_entries.append(dict(zip(("wifi", "mic", "audio", "lock", "capture", "finish", "loop", "register", "recovery", "font"), map(int, entry.groups()))))
                    asr_entries = asr_entries[-10:]
                health = re.search(r"text_entry: health ticks=(\d+) stage=(\d+) refresh=(\d+) keys=(\d+) draw=(\d+) internal=(\d+) external=(\d+)", line)
                if health:
                    item = dict(zip(("ticks", "stage", "refresh", "keys", "draw", "internal", "external"), map(int, health.groups())))
                    flush = re.search(r" external=\d+ flush=(\d+)", line)
                    if flush: item["flush"] = int(flush.group(1))
                    editor_health.append(item)
                    editor_health = editor_health[-30:]
                panic = re.search(r"Guru Meditation Error: Core\s+(\d+) panic'ed \(([A-Za-z0-9 _/-]+)\)", line)
                if panic:
                    panic_diagnostics.append({"core": int(panic.group(1)), "reason": panic.group(2)})
                registers = re.findall(r"\b(MEPC|RA|MCAUSE|MTVAL)\s*:\s*(0x[0-9a-fA-F]+)", line)
                if registers:
                    panic_diagnostics.append(dict(registers))
                panic_diagnostics = panic_diagnostics[-16:]
                for name, marker in markers.items():
                    counts[name] += marker in line
                    if marker in line and name not in first_seen:
                        first_seen[name] = round(time.monotonic() - started, 2)
                home = re.search(r"startup_home duration_ms=(\d+) fonts_done=(\d+) cloud_ready=(\d+)", line)
                if home:
                    startup_home = dict(zip(("duration_ms", "fonts_done", "cloud_ready"),
                                            map(int, home.groups())))
                preview = re.search(r"timing: shown_fps=(\d+)\.(\d+) cache_us=(\d+) scale_us=(\d+) display_us=(\d+)", line)
                if preview:
                    fps, fraction, cache, scale, display = map(int, preview.groups())
                    preview_samples.append({"shown_fps": round(fps + fraction / 100, 2),
                                            "cache_us": cache, "scale_us": scale,
                                            "display_us": display})
                    preview_samples = preview_samples[-60:]
                direct = re.search(r"timing_direct: shown_fps=(\d+)\.(\d+) cache_us=(\d+) compose_us=(\d+)", line)
                if direct:
                    fps, fraction, cache, compose = map(int, direct.groups())
                    preview_samples.append({"path": "direct_native",
                                            "shown_fps": round(fps + fraction / 100, 2),
                                            "cache_us": cache, "compose_us": compose})
                    preview_samples = preview_samples[-60:]
                counts["error_lines"] += bool(re.search(r"(?:^|\x1b\[[0-9;]*m)E \(\d+\)", line))
                stages = re.search(r"preview_stages: wait_us=(\d+) cache_us=(\d+) ppa_us=(\d+) submit_us=(\d+) dma_retirement=(\d+)", line)
                if stages:
                    preview_stages.append(dict(zip(("wait_us", "cache_us", "ppa_us", "submit_us", "dma_retirement"),
                                                   map(int, stages.groups()))))
                    preview_stages = preview_stages[-60:]
                counts["touch_out_of_bounds"] += "greater than hor. res" in line or "greater than ver. res" in line
            # Discard overlong/unterminated payloads rather than retain private data.
            if len(pending) > 4096:
                pending = b""
        elapsed = time.monotonic() - started
    print(json.dumps({"port": args.port, "elapsed_s": round(elapsed, 2),
                      "reset_requested": args.reset, "counts": counts,
                      "first_seen_s": first_seen, "startup_home": startup_home,
                      "preview_samples": preview_samples,
                      "preview_stages": preview_stages,
                      "preview_awb": preview_awb,
                      "panic_diagnostics": panic_diagnostics,
                      "editor_health": editor_health,
                      "asr_entries": asr_entries,
                      "asr_failures": asr_failures,
                      "asr_font_results": asr_font_results,
                      "network_health": network_health,
                      "wifi_worker_health": wifi_worker_health,
                      "wifi_disconnect_reasons": wifi_disconnect_reasons,
                      "tls_errors": tls_errors,
                      "ui_perf": ui_perf,
                      "display_health": display_health,
                      "display_power_events": display_power_events,
                      "raw_log_saved": False, "port_released": True}, indent=2))


if __name__ == "__main__":
    main()
