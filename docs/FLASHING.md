# 固件刷写 / Firmware flashing

Release `v1.0.0` includes a public-source snapshot, newly built firmware, boot files and SHA-256 checksums. It does not contain the original private backup, NVS data or personal Wi-Fi credentials.

该固件只针对 `firmware-source/platformio.ini` 指定的 ESP32-C3、4 MB Flash 和 GPIO4 灯板接线。其他芯片、Flash 布局和屏幕走线需要重新配置编译。刷写前停止电脑取音后台、串口监视器以及其他占用目标板的程序。

The prebuilt image targets the configured ESP32-C3 with 4 MB flash and GPIO4. Rebuild for different chips, layouts or wiring. Stop audio bridges and serial monitors before flashing. The public build was compiled and checked; it was not flashed to the original user's controller during publication.

## 校验 / Verify download

Compare the downloaded ZIP hash with the accompanying `SHA256SUMS.txt`. After extraction, `manifest.json` records individual file hashes.

```powershell
Get-FileHash .\desktop-led-panel-v1.0.0.zip -Algorithm SHA256
```

## 确认目标 / Identify the target

Replace `COMx` with the intended controller's actual port. Read the chip identity and MAC, and compare with your physical device before any write.

```powershell
py -3 -m pip install esptool==4.11.0
py -3 -m esptool --chip esp32c3 --port COMx read_mac
```

## 已有同配置设备 / Existing matching installation

Only update the application at `0x10000` when the existing partition table matches this release. This command does not deliberately erase NVS, but retain your own configuration backups.

```powershell
py -3 -m esptool --chip esp32c3 --port COMx --baud 460800 write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x10000 firmware/firmware.bin
```

## 新的同配置 ESP32-C3 / Fresh matching controller

From the extracted release root:

```powershell
py -3 -m esptool --chip esp32c3 --port COMx --baud 460800 write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x0 firmware/bootloader.bin 0x8000 firmware/partitions.bin 0xe000 firmware/boot_app0.bin 0x10000 firmware/firmware.bin
```

Wait for write verification. Open a serial console at 115200 baud and press RESET to read the generated AP password, then configure your own network in the phone browser.

An 8-row display and brightness-limited firmware do not establish the electrical current rating of your specific panel or supply. Select wiring and power independently of the software brightness percentage.
