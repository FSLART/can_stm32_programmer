# LART STM32F412 CAN bootloader

CAN bootloader for the **STM32F412RE** (512 KiB flash, 256 KiB SRAM). The flash is split in half: the first half holds the bootloader and the running application, the second half receives uploads. A new image is uploaded into the second half, checked with SHA-256 and committed; at the next reset the bootloader copies it into the application slot, verifies the copy and starts it. The image format is MCUboot's (512-byte header + SHA-256 TLV), validated by code in this repository; firmware is unsigned, so SHA-256 detects corruption but does not authenticate its author.

## Guides

- **[Port to another STM32](docs/PORTING.md)** — layout rules, files to edit, application `RequestBootloader()` and CAN entry example.
- **[Configure a new application's linker](docs/APPLICATION_LINKER.md)** — flash origin/size, RAM request reservation, vectors and binary checks for a normal CubeIDE `.bin`.
- **[Use and understand the Python uploader](docs/UPLOADER.md)** — setup, direct CubeIDE `.bin` upload, automatic conversion, entry command, progress, retries and troubleshooting.
- **[CAN protocol](docs/CAN_PROTOCOL.md)** and **[DBC](docs/bootloader.dbc)** — message and signal definitions, [cantools codec regeneration and encode/decode examples](docs/CAN_PROTOCOL.md#dbc-and-generated-c-codec).

The GUI in the repository root (`CAN_STM32_Programmer.py`, mode **Bootloader LART**) uses the same protocol and packaging code as `tools/can_upload.py`.

## Build and program the bootloader

```powershell
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target bootloader
```

CMake, Ninja and Arm GNU tools must be on PATH (STM32CubeIDE ships `arm-none-eabi-gcc` under `plugins/...gnu-tools-for-stm32.../tools/bin`). CubeIDE can also import and build the `MCU_BOOT_F412` project directly. Program `build/bootloader.elf` or `.hex` with ST-LINK; a raw bootloader `.bin` goes at `0x08000000`. The build uses about 20 KiB of the 64 KiB bootloader region.

The CAN frame layouts come from `docs/bootloader.dbc`: `Core/Inc/bootloader.h` and `Core/Src/bootloader.c` are generated from it by `tools/generate_can.py` (cantools 44.2.1). Do not edit them by hand; regenerate after changing the DBC.

Board assumptions (from the LART STM32F412RET6 board): **HSE 8 MHz** → 64 MHz SYSCLK, APB1 32 MHz; **CAN2 on PB5 (RX) / PB13 (TX)**; log UART **USART2 PA2/PA3**; status LED **PA5**; optional entry button **PC13, active-low** with internal pull-up. Change `main.c`, `main.h` and `stm32f4xx_hal_msp.c` (or the `.ioc`) if your board differs. Sectors 4–7 must not be write-protected; the bootloader checks this at boot and logs an error instead of erasing.

## Pass the application binary directly to the uploader

Build your CubeIDE **application** with its vector table at **`0x08010200`** (FLASH origin `0x08010200`, `VECT_TAB_OFFSET=0x10200U` with `USER_VECT_TAB_ADDRESS`). A raw binary linked at `0x08000000` cannot be relocated just by adding a header; the uploader rejects invalid stack/reset vectors before contacting the device. Maximum raw application: **196056 bytes** (`0x2FDD8`); maximum packaged image: **196608 bytes** (`0x30000`, the 192 KiB application slot).

```powershell
python -m venv .venv
.venv\Scripts\python.exe -m pip install -r tools/requirements.txt
.venv\Scripts\python.exe tools/can_upload.py path/to/CubeIDE_application.bin --channel COM5
```

The script creates `CubeIDE_application.mcuboot.bin` beside the input (512-byte header, version 1.0.0, and a SHA-256 TLV) and uploads it. Already packaged, unpadded MCUboot images are accepted as-is. The progress bar shows acknowledged bytes; validation/commit is a separate step reported afterward. The reset at the end installs and starts the new image.

Default adapter interface: SLCAN, classic CAN **250 kbit/s**, request ID `0x600`, reply ID `0x580`. For SocketCAN use `--interface socketcan --channel can0` with a preconfigured 250 kbit/s interface. Only connect one target using these fixed IDs.

The uploader first sends `ENTER_BOOT` on CAN ID `0x601` with `BCI_BOOT=1`. The running application must handle that frame by writing the RAM request `0xB00710AD` at `0x2003FFFC` and resetting. Without that handler, hold the PC13 button during reset. If no valid application exists, the bootloader waits for an upload without needing the button.

## Flash layout

| Region | Address | Size | Sectors |
| --- | --- | --- | --- |
| Bootloader | `0x08000000` | 64 KiB | 0–3 (16 KiB each) |
| Application (primary) slot | `0x08010000` | 192 KiB | 4 (64 KiB) + 5 (128 KiB) |
| Upload (staging) slot | `0x08040000` | 256 KiB | 6–7 (128 KiB each) |
| Staging commit record | `0x0807FFF0` | 16 bytes | end of sector 7 |

The STM32F412 is single-bank and has no bank remapping (the L476 version ran each bank in place through `SYSCFG FB_MODE`). An image can only run at the address it was linked for, so the application always executes from the primary slot and the second half is a staging area.

**Upload.** BEGIN erases sector 7 first (invalidating the old record), then sector 6, and programs the image length into the record. DATA programs one 32-bit word per frame. END checks the transport CRC, rereads the staged image, validates header, vectors, TLVs and SHA-256, and only then programs `~length` to commit it. The running application is never touched during an upload. A receiving or committed session resets after 30 seconds without a successful command.

**Install at reset.** If the record is committed and not yet marked installed, the staged image is validated, sectors 4–5 are erased, the image is copied and the copy is validated again; then the record is marked installed. Power loss during the copy leaves the staged copy intact, so the next reset copies again. If the primary image is later damaged, it is reinstalled from the retained staged copy. A damaged or uncommitted staged image is ignored and the current application keeps running. An image programmed directly into the primary slot with ST-LINK (a packaged `.mcuboot.bin` at `0x08010000`) is accepted and is not overwritten by an already installed staged copy.

This is integrity fallback, not application health confirmation: a hash-valid image that hangs needs a watchdog and a separate confirmation policy. There is no rollback to the version that was replaced, because only one staged copy fits.

## Bootloader heartbeat

While in bootloader mode, the board sends **`BCI_BOOT_HEARTBEAT` on `0x581`** every 500 ms under normal conditions: `55 tick0 tick1 tick2 tick3 major minor patch`. Bytes 1-4 carry the unsigned 32-bit `HAL_GetTick()` millisecond value in little-endian order (resets on MCU startup, wraps modulo 2^32). Bytes 5-7 carry the bootloader version (currently 1.0.0, `BOOT_VERSION_*` in `Core/Inc/boot_update.h`). It is a mode indicator, not an upload acknowledgement, and stops at application handoff. Flash operations (BEGIN's erase) can delay it.

## CAN diagnostics

The [DBC](docs/bootloader.dbc) defines structured diagnostic frames on **`0x582`–`0x589`**: capabilities/board type (2 = this F412 port), application version, slot decision/reset flags, upload stage/counts, last error, rollback (rejected staged upload), flash offset/HAL error and rejected image version. Slot 1 = primary (application), slot 2 = staging (upload). Details and numeric labels: [CAN protocol guide](docs/CAN_PROTOCOL.md#can-diagnostics).

Diagnostic snapshots are sent when CAN starts and after INFO; upload stages, 16 KiB progress and errors update on change, including errors captured while installing at boot. Transmission is best-effort with one mailbox reserved for replies and a bounded 20 ms drain before blocking phases/handoff. CAN2 uses automatic bus-off recovery and no automatic retransmission. These frames never replace programming ACKs.

## Logs and checks

Connect a USB-UART to USART2 (PA2 TX) at **115200 baud, 8N1**. The bootloader prints `LART CAN bootloader (STM32F412) v1.0.0`, reset flags, flash checks, image validation, installation, hold reasons, CAN startup, upload progress every 16 KiB, CRC/SHA failures, flash errors and resets. Immediately before UART shutdown it prints `Boot application` with VTOR, stack pointer and reset handler. `BOOT_LOG_LEVEL` in `Core/Inc/boot_update.h` controls verbosity (0–3). Logging is blocking; successful DATA frames are not logged individually.

```powershell
.venv\Scripts\python.exe -m unittest discover -s tests -v
cmake --build build --target boot_tests boot_port_tests
qemu-system-arm -M mps2-an386 -nographic -semihosting -kernel build/boot_tests.elf
qemu-system-arm -M mps2-an386 -nographic -semihosting -kernel build/boot_port_tests.elf
```

The QEMU test runs the real `boot_update.c` against modeled F412 flash: staging upload, CRC/SHA rejection, verified install, reinstall fallback, torn commit, power loss during installation, direct primary programming, duplicate frames and flash errors. It also checks the diagnostic snapshots. `boot_port_tests` compiles the real `boot_port.c` with mocked HAL CAN calls: heartbeat timing/rollover, every diagnostic payload and mailbox backpressure. `tests/test_dbc.py` checks that the generated codec matches the DBC. None of this emulates the F412 flash controller, CAN, the button or the handoff. Test installation, power interruption, maximum image size and the handoff on the board before deployment.

## CAN hardware

Use a 3.3 V-compatible CAN transceiver (e.g. MCP2562): PB13 to TXD, PB5 to RXD, common ground, CANH/CANL to the bus, 120 Ω at each physical end. CAN2 timing: 32 MHz / (8 × (1 + 13 + 2)) = 250 kbit/s, sample point 87.5 %. CAN2 filters live in CAN1's filter bank array; the bootloader uses bank 14 (`SlaveStartFilterBank = 14`) and enables the CAN1 clock for it.

Reserve CAN IDs `0x580`–`0x589`, `0x600` and `0x601` for this target. There is no firmware authentication or node addressing. Details: [CAN protocol](docs/CAN_PROTOCOL.md), [porting](docs/PORTING.md).
