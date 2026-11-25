# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

HueLeaf Base is the OSPTEK e-paper development kit repository: an ESP32-C5 (ESP32-C5HF4) board with a 3.7″ six-color EPD037A02 panel (480×720), AXP2101 PMIC/battery charger, PCF8563 RTC, MSA321 accelerometer, 4 MB flash and a microSD slot, plus dual-band Wi-Fi 6. The top level holds hardware PDFs, documentation, factory firmware and one ESP-IDF example; all firmware work happens inside `examples/3.7einkDvb` (C, ESP-IDF 5.5.x, target `esp32c5`).

## Build, flash, run

Use an ESP-IDF 5.5.x environment. From `examples/3.7einkDvb`:

```bash
idf.py set-target esp32c5     # first configure only
idf.py build
idf.py -p COMx flash monitor  # replace COMx with the detected port
idf.py menuconfig             # target / partition / component options
idf.py fullclean              # after partition-table or toolchain changes
```

The tracked `sdkconfig` already targets `esp32c5` with 4 MB flash. `sdkconfig.defaults` is the source of defaults; `partitions.csv` defines the layout (factory app at `0x10000`/`0x2F0000`, plus a 1 MB `image_cache` data partition at `0x300000`). A merged factory image can be written directly with `esptool.py --chip esp32c5 write_flash 0x0 firmware/3.7einkDvb_factory_0x0.bin`.

There is no automated test suite. Verify changes by building, flashing, watching the serial monitor, and exercising the web UI over Wi-Fi.

## Boot and module layout

`app_main` (`main/main.c`) runs `board_init()` (SPI2 + I2C0, power rails, AXP2101), then optional `pcf8563_init`, `msa321_init`, `tf_card_mount`, `nfc_driver_init`, then `screen_driver_init`, `screen_rotation_init`, `wifi_mgr_start`, `web_server_start`, `power_mgr_init`, and finally creates `screen_task` to handle PMIC power-button events and network-page redraws.

### Shared buses and lock ordering

Two buses are shared and guarded by locks in `board/board.c`. SPI2 is used by both the panel and the TF card (`board_spi_lock`/`board_spi_unlock`, `board_tf_cs_idle`); I2C0 is shared by the AXP2101, MSA321 and PCF8563 (`board_i2c_bus()` plus `board_i2c_lock`/`board_i2c_unlock`). New peripherals must reuse these handles and locks instead of creating another controller on the same pins. Lock order is rotation → display → board SPI; never take the rotation lock from a display read or progress callback.

### Display data path

The panel framebuffer is 172,800 bytes (480×720 at 4 bits/pixel): column-major, two vertically adjacent pixels per byte, high nibble on even y. This is not a row-major 4-bit bitmap. Six-color quantization/dithering and frame packing happen in the browser in `web_server/www/eink_pipeline.js` (`makeFrame`, format `otp6-v1`); the device streams the body over `POST /api/frame` and drives the panel over SPI2 with the OTP waveform. The panel profile and LUT in `screen_driver/panel_profile.h` and `panel_lut.h` are verified for the EPD037A02 — do not substitute sequences or LUTs from other controllers. The provisioning page (`screen_ui_show_setup`) is generated on-device and paints a procedural six-color leaf logo from `screen_ui.c` into the same column stream, so the logo follows rotation.

### HTTP job model

`web_server/web_server.c` serves the embedded Vue UI and a REST API under `/api/*`. Screen-changing requests (`/api/frame`, `/api/clear`) do not run on the HTTP handler: the handler validates headers, opens an async handler, and hands the job to a dedicated task so the server stays responsive during long refreshes. Only one screen job runs at a time; concurrent requests get `409 Conflict`. Frame uploads are validated against an FNV-1a checksum (`X-Eink-Checksum`), the rotation epoch (`X-Eink-Rotation-Epoch`) and, for region updates, the display generation (`X-Eink-Base-Generation`). Keep the `/api` contract and `X-Eink-*` header semantics in sync with `eink_pipeline.js`.

### Rotation and image cache

`screen_driver/screen_rotation.*` owns the logical rotation (default from `EINK_ROTATION` in `pin_map.h`; the physical BOOT button on GPIO28 advances it 90° per press), the blank/restore toggle on a power-button short press, and a replay cache stored in the `image_cache` flash partition so a rotated or re-shown frame can be replayed without re-upload. Rotation is a lock-protected job; callers hold `screen_rotation_lock()` for the whole operation.

### Wi-Fi provisioning

`wifi_mgr/` starts an open SoftAP (`Eink E6 3.7`, 5 GHz channel 36, `http://192.168.4.1`) for provisioning, then switches to STA using credentials saved in NVS and reclaims the AP if association fails. The Wi-Fi country code is hardcoded to `CN` in `wifi_mgr.c`; change it (and `WIFI_AP_CHANNEL` in `wifi_mgr.h`) for other regions, since 5 GHz channel availability is regulatory. The radio runs `WIFI_PS_MIN_MODEM` and caps TX power at `WIFI_TX_POWER_QDBM` (10 dBm); raise it only if the phone-to-device link is unreliable.

### Power and peripherals

The AXP2101 power button is a falling-edge IRQ on GPIO25; the ISR only notifies `screen_task`, which reads/acknowledges the PMIC in task context and toggles the screen blank. Never do I2C or display work in the ISR. The RTC (`pcf8563.c`), IMU (`msa321.c`), TF card and NFC (`nfc_driver.c`, not populated on shipped boards) are driver-ready but their application features are not implemented; note the RTC currently writes compile time at boot rather than network time.

`power_mgr/` enables DFS (80–160 MHz) and automatic light sleep. When the provisioning AP has had no client, no screen job and no rotation job for `POWER_AP_IDLE_TIMEOUT_MS` (3 minutes), it stops Wi-Fi, parks the rotate-button poll task, and enters `esp_light_sleep_start()`. BOOT (GPIO28), the PMIC IRQ (GPIO25) and the RTC IRQ (GPIO23) are armed as low-level GPIO wake sources; any edge wakes the chip, reopens the provisioning AP and resumes the button task. Deep sleep is not used because ESP32-C5 deep-sleep GPIO wake only covers GPIO0–5, while this board's wake pins are 23/25/28.

## Hardware conventions

`main/pin_map.h` is the authoritative GPIO map (panel/SPI, I2C0, IRQ lines, rotate button). Panel SPI runs at 2 MHz and TF at 10 MHz on the same SPI2 host; the display driver stages every transfer through a fixed DMA buffer. Before allocating a new GPIO, check ESP32-C5 boot straps, the schematic in `hardware/`, and current firmware use — GPIO28 is both the rotate button and a boot strap. The 4 MB flash budget is tight; re-check offsets and totals when changing `partitions.csv`.

## Documentation

`README.md` / `README_EN.md` cover the board and web UI; `HueLeaf_Eink_DevKit开发者说明.md` is the Chinese developer guide with pin/bus/power detail and a recommended extension order. Hardware schematics and placement drawings are in `hardware/`.
