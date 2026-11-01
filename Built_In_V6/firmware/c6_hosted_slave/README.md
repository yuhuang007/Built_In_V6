# P4X board-mounted C6 ESP-Hosted firmware

This is a build wrapper for the ESP32-C6FH4 Wi-Fi/Bluetooth co-processor on
the ESP32-P4X Function EV Board. It deliberately references the `esp_hosted`
source already managed by `../p4_main`, whose version is locked to `2.12.13`.

The C6 uses its fixed SDIO slave pins (`CMD=18`, `CLK=19`, `D0..D3=20..23`)
and its 4 MB internal flash. The P4 host resets it through P4 GPIO54.

Build artifacts remain in this directory's `build/` folder. Building is safe;
flashing COM16 is a separate, explicitly approved hardware action.

Before a serial flash, put the C6 into ROM download mode and use the
`PROG_C6` header. Never connect the USB-TTL adapter's 5 V or 3.3 V pin.
