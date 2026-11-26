# Third-party notices / 第三方说明

## Fusion Pixel Font

- Project: https://github.com/TakWolf/fusion-pixel-font
- Source: 8px monospaced zh_Hans BDF, release `2026.11.10`.
- The firmware contains a generated 7,496-glyph subset. The source BDF is retained in `font-tools/`.
- SIL Open Font License and component notices are preserved under `firmware-source/FONT-LICENSES/`, including Boutique Bitmap 7x7, Galmuri, Misaki and Miseki Bitmap notices.
- Generated preview images use this font.

## Firmware dependencies

- Arduino-ESP32: https://github.com/espressif/arduino-esp32 — package metadata identifies LGPL-2.1-or-later.
- ESP-IDF, SDK libraries, bootloader and tooling: https://github.com/espressif/esp-idf — upstream component-specific notices apply.
- The exact PlatformIO platform and Arduino package are pinned in `firmware-source/platformio.ini`; building resolves the corresponding upstream dependencies.
- TLS trust anchors in `weather_ca.h` identify public Let's Encrypt ISRG roots, not private keys.

## Desktop dependencies and services

- NumPy: https://numpy.org/
- PyAudioWPatch / PortAudio: https://github.com/s0d3s/PyAudioWPatch
- pySerial: https://github.com/pyserial/pyserial
- Python and Tkinter: https://www.python.org/
- Weather is retrieved from wttr.in; time uses configured public NTP services. These are external services with their own availability and terms.

Original project code has no separately assigned open-source license. This statement does not replace any upstream license or restrict rights already granted by upstream licensors.
