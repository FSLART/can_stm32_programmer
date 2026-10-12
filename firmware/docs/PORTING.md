# Port this bootloader to another STM32

**Follow the steps in order. Test each “Done when” checkpoint before continuing.** The goal is one application binary, one fixed application linker address, CAN programming and interruption-safe installation.

You build two separate programs: the **bootloader** receives, validates and installs updates; the **application** is your normal firmware. The PC runs [can_upload.py](../tools/can_upload.py) or the GUI; see the [Python uploader guide](UPLOADER.md).

The reference target is the **STM32F412RET6** on the LART board. Other STM32s may need different flash operations, CAN APIs or memory maps. There is no universal set of register values for every STM32.

For the detailed application-project procedure, see [Configure a new application's linker](APPLICATION_LINKER.md): address/size calculations, CubeIDE settings, RAM/stack reservation, vectors and binary checks.

## Start here: choose your route

| What changes? | Your route |
| --- | --- |
| Same STM32F412, different board/pins/clock | Steps 3, 6 and 9, then the board tests. Keep the layout. |
| STM32F412xG (1 MiB) and you want bigger slots | Step 4: enlarge both slots on sector boundaries; update the sector table in `boot_flash.c` and the tests. |
| Another single-bank STM32 with bxCAN | Follow every step. Replace the sector table, flash calls and clocks. |
| Only need application-to-bootloader entry | Go to [step 9](#step-9--add-requestbootloader-to-the-application). |

## How the current project works

```text
Application running
  -> PC sends ENTER_BOOT (0x601), BCI_BOOT=1
  -> application writes a RAM request and resets
  -> bootloader sees request and waits for programming
  -> PC uploads into the staging half of flash
  -> bootloader checks CRC, vectors and SHA-256, then commits
  -> PC requests reset
  -> bootloader copies the committed image into the application slot, verifies it, marks it installed
  -> bootloader starts the application at 0x08010200
```

The application always runs from the primary slot, so it is linked once for that address. The staging half keeps a copy: an interrupted installation or a later corrupted primary image is reinstalled from it at the next reset. Two invalid images lead to recovery mode. A hash-valid program that hangs needs a separate watchdog/health-confirmation policy.

The image format is MCUboot's (header, payload, TLVs). Validation lives in [boot_update.c](../Core/Src/boot_update.c) using the bundled mbed TLS SHA-256; no MCUboot library code is compiled.

## Terms in plain language

| Term | Meaning |
| --- | --- |
| Primary slot | Flash region the application executes from. |
| Staging slot | Flash region uploads are written to before installation. |
| Raw `.bin` | Application code starting with its vector table. |
| Packaged image | Header + application + hash information; the script creates it. |
| Vector table | Startup and interrupt-handler addresses. |
| Erase sector | Smallest flash region erased in one operation. |
| Programming unit | Size/alignment of one legal flash write (F412: 32-bit word at 2.7–3.6 V). |
| Commit record | Words written last to mark an upload ready to install. |

## Step 1 — Fill in the target worksheet

Use the **datasheet, reference manual and errata for the exact chip and silicon revision**.

| Find this | Your target | F412RE reference |
| --- | --- | --- |
| Exact chip/board | | STM32F412RET6, LART board |
| CPU/FPU | | Cortex-M4F, hard-float build |
| Total flash | | 512 KiB, single bank |
| Erase geometry | | Sectors 0–3: 16 KiB, 4: 64 KiB, 5–7: 128 KiB |
| Programming unit | | 32-bit word (PSIZE x32, VDD 2.7–3.6 V); no ECC |
| SRAM / reset retention | | 256 KiB at `0x20000000`, retained across software reset |
| Clock | | HSE 8 MHz → 64 MHz SYSCLK, APB1 32 MHz |
| CAN peripheral / pins | | bxCAN CAN2, PB5 RX / PB13 TX (filters in CAN1, bank 14) |
| CAN rate / logging | | 250 kbit/s / USART2 PA2/PA3, 115200 8N1 |
| Button / LED | | PC13 active-low with pull-up / PA5 |
| Protection | | Sectors 4–7 must have nWRP=1 |

Keep ST-LINK/SWD recovery available. Read existing option bytes before changing them.

**Done when:** you know the real flash/RAM map, legal erase/write operations and CAN hardware.

## Step 2 — Check the update strategy fits

The bootloader needs room for **bootloader + primary slot + staging slot**, where staging is at least as large as the primary slot plus one erasable unit for the record (or the record fits after the maximum image, as here). If the target has dual-bank remapping, the older A/B bank-swap design avoids the copy; this repository no longer implements it. If there is no room for two copies, the image must be uploaded straight into the primary slot, which loses interruption safety.

**Done when:** the three regions fit on sector boundaries.

## Step 3 — Generate the target project and bring up UART

1. Open [MCU_BOOT_F412.ioc](../MCU_BOOT_F412.ioc) or create a CubeMX project for the exact target.
2. Configure clocks, SWD, UART, CAN/transceiver pins, LED and optional button.
3. Generate HAL/device headers, startup, interrupts and linker script; keep `boot_*.c/h`, the generated codec `bootloader.c/h` and `Drivers/mbedtls_sha256`.
4. First run a simple UART/LED program to prove the hardware.
5. Call `boot_run()` after peripheral initialization in a CubeMX USER CODE section:

```c
/* USER CODE BEGIN 2 */
boot_run(&hcan2);
/* USER CODE END 2 */
```

`boot_run()` accepts CAN1 or CAN2; it picks filter bank 0 for CAN1 or 14 for CAN2. For FDCAN, change `boot_run()`'s handle type and the RX/TX calls; renaming alone is insufficient.

### Exact files to review

| File/settings | Adapt |
| --- | --- |
| [boot_port.h](../Core/Inc/boot_port.h), `main.h`, `stm32f4xx_it.h` | HAL include, pins, handle types |
| `main.c`, `stm32f4xx_hal_msp.c` | Clocks, CAN timing, alternate functions |
| `system_stm32f4xx.c`, `startup_stm32f412retx.s` | Target reset/vector implementation |
| [boot_update.h](../Core/Inc/boot_update.h) | Flash/slot layout, RAM range, bootloader version, board type, log level |
| [bootloader.dbc](bootloader.dbc) → `bootloader.c/h` | CAN IDs/layouts; regenerate with `tools/generate_can.py` |
| [boot_flash.c](../Core/Src/boot_flash.c) | Sector table, protection check, programming unit |
| [boot_port.c](../Core/Src/boot_port.c) | RAM flag address, CAN filter, button, handoff |
| [STM32F412RETX_FLASH.ld](../STM32F412RETX_FLASH.ld) | Bootloader flash size, RAM length (flag reserved) |
| [CMakeLists.txt](../CMakeLists.txt), CubeIDE settings | Sources, device macro, CPU/FPU flags, linker script |
| [can_upload.py](../tools/can_upload.py), [DBC](bootloader.dbc), tests | Addresses, limits, IDs, rate, fixtures |

**Done when:** the project builds and logs are readable on the board.

## Step 4 — Draw the layout and configure linkers once

```text
[bootloader B][primary slot P]           [staging slot S ............ record]
```

Current F412RE layout (the flash split in half):

| Region | Start | Size |
| --- | --- | --- |
| Bootloader | `0x08000000` | 64 KiB (sectors 0–3) |
| Primary slot | `0x08010000` | 192 KiB (sectors 4–5) |
| Staging slot | `0x08040000` | 256 KiB (sectors 6–7) |
| Commit record | `0x0807FFF0` | last 16 bytes of staging |

```text
H = header size                      (512)
T = hash TLV block                   (40)
M = maximum packaged upload = P      (0x30000 = 196608)
Maximum raw application = M - H - T  (196056 = 0x2FDD8)
```

The record must lie past `M` inside staging and inside an erase sector that BEGIN erases **first**. On a 1 MiB F412xG you could use primary = sectors 4–7 (448 KiB) and staging = sectors 8–11 (512 KiB); update `boot_update.h`, the sector table in `boot_flash.c` (and the test copy), `MAX_IMAGE` in the script and the DBC/docs.

Application linker: FLASH origin `0x08010200`, length ≤ raw maximum; startup with `USER_VECT_TAB_ADDRESS` and `VECT_TAB_OFFSET=0x10200U`. VTOR needs 512-byte alignment on the F412 (113 vectors); `0x08010200` satisfies it. Reserve the RAM flag as in step 9.

**Done when:** maps show no overlap; the raw application starts with valid SP/Thumb reset vectors inside its payload.

## Step 5 — Implement flash operations and commit records

`boot_flash.c` provides three functions used by `boot_update.c` (offsets relative to `0x08000000`, return 0 on success):

| Function | Required behavior |
| --- | --- |
| `boot_flash_read()` | Range check, copy bytes |
| `boot_flash_write()` | Word-aligned, outside the bootloader, target word erased, program, read back |
| `boot_flash_erase()` | Exact whole sectors only, outside the bootloader, report failure |

Rules kept by the design: never program a word twice (an exact DATA retry is acknowledged without programming); skip all-ones words (already erased); the record has separately programmed words for prepare (`length`), commit (`~length`) and installed (`0`). `boot_flash_init()` disables and resets the ART data cache so readback after programming sees flash, not a stale cache line (done in code because CubeMX regenerates `stm32f4xx_hal_conf.h`); the instruction cache is reset before the handoff. Every flash failure also calls `boot_diag_flash_error()` so the offset and raw HAL error are published on `0x588`.

**Done when:** staging erase/write/readback preserves the bootloader and primary slot, and partial or failed commit writes cannot install.

## Step 6 — Adapt CAN, UART and the button

Current MCU and script both use **250 kbit/s**:

```text
32 MHz / (prescaler 8 * (1 + BS1 13 + BS2 2)) = 250 kbit/s, sample point 87.5 %
```

Use the actual APB1 clock. On dual-CAN parts, CAN2 filters live in CAN1: CAN1's clock must be enabled (the generated MSP does this) and the CAN2 bank must be ≥ `SlaveStartFilterBank`.

| ID | Direction | Meaning |
| --- | --- | --- |
| `0x600` | PC -> bootloader | Programming commands |
| `0x580` | Bootloader -> PC | ACK/status |
| `0x601` | PC -> application | `ENTER_BOOT.BCI_BOOT=1` |
| `0x581` | Bootloader -> other boards | `BCI_BOOT_HEARTBEAT`: `0x55`, millisecond tick and mandatory bootloader version |

Structured diagnostics additionally use `0x582`–`0x589`; see the [diagnostic layouts](CAN_PROTOCOL.md#can-diagnostics). Set `BOOT_VERSION_MAJOR/MINOR/PATCH`, `BOOT_BOARD_TYPE` and `BOOT_PROTOCOL_VERSION` in `boot_update.h`. Version snapshots and HAL error details depend on the port; adapt reset flags, slot identification and the CAN drain hook (`boot_diag_flush`) along with flash/CAN code.

Reserve these IDs. Change IDs/signals in the DBC, regenerate `bootloader.h`/`bootloader.c` with `tools/generate_can.py`, then rebuild MCU and application. Python loads the saved DBC automatically. See [codec regeneration](CAN_PROTOCOL.md#dbc-and-generated-c-codec). Use an external transceiver, common ground and 120 Ω termination at both ends.

The heartbeat is best-effort every 500 ms and keeps a TX mailbox free for replies; diagnostics coalesce and drain for at most 20 ms before blocking phases. CAN2 runs with automatic bus-off recovery (`AutoBusOff = ENABLE`) and without automatic retransmission, so a lone node does not clog its mailboxes. Adapt these HAL calls when porting to FDCAN.

Replace `huart2` for your board and keep log flushing before UART shutdown. Measure button levels and update the pull and every hold test together; the current code treats low as pressed. Provide another entry mechanism if there is no button.

**Done when:** INFO replies, logs work and releasing the button lets a valid application start.

## Step 7 — Adapt the handoff

`jump()` in `boot_port.c` validates SP/PC, prints and flushes, stops CAN/UART, resets clocks (`HAL_RCC_DeInit`, `HAL_DeInit`), disables SysTick and all NVIC lines, resets the ART caches, sets `VTOR = 0x08010200` and calls `enter_app()`, which loads MSP, clears CONTROL, enables interrupts and branches to the reset handler. No memory remap is needed because the application runs where it was linked.

**Done when:** a small known-good app starts after installation, with interrupts working.

## Step 8 — Adapt the Python target assumptions

| Location in `can_upload.py` | Change/review |
| --- | --- |
| `APP_ADDRESS`, `RAM_START`, `RAM_END`, `MAX_IMAGE` | Application address, SRAM range, primary slot size |
| `check_image()` | Accepted header/TLV format if changed |
| DBC / `can.Bus()` | IDs/layout come from the DBC; current rate is hardcoded `250000` |
| `request()` / `upload()` | Deadlines if measured delays require changes |

**Done when:** valid images pass host checks; wrong-address/corrupt/oversized images fail; MCU and host limits agree.

## Step 9 — Add RequestBootloader to the application

**Put this function in the application project.** It resets into this bootloader, not ST's factory ROM bootloader.

### 9.1 Reserve the RAM flag

Use the same address/magic in the application and the bootloader's `BootloaderRequested()`. The word must survive software reset and be excluded from `.data`, `.bss`, heap and stack in **both linkers**. On the F412 the flag is `0x2003FFFC`, inside the final eight bytes of SRAM. In the application linker, inside `MEMORY { ... }`:

```ld
/* F412 SRAM: reserve the last 8 bytes, including the request word at 0x2003FFFC. */
RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 256K - 8
```

and keep `_estack = ORIGIN(RAM) + LENGTH(RAM);` (8-byte aligned, `0x2003FFF8`). The bootloader linker already uses `LENGTH = 0x3FFF8`.

### 9.2 Copy this function

```c
#include "main.h"  /* Application's target HAL/CMSIS declarations. */
#include <stdint.h>

#define BOOTLOADER_FLAG_ADDR  0x2003FFFCUL
#define BOOTLOADER_MAGIC      0xB00710ADUL

void RequestBootloader(void)
{
    volatile uint32_t *flag =
        (volatile uint32_t *)BOOTLOADER_FLAG_ADDR;

    *flag = BOOTLOADER_MAGIC;

    __DSB();
    NVIC_SystemReset();

    while (1) {}
}
```

Sequence: **write flag -> reset -> bootloader consumes flag -> bootloader waits**. After programming, the script requests a second reset; without a new RAM request the bootloader installs the upload and starts the application.

### 9.3 Call it when ENTER_BOOT arrives

The script sends one standard frame on `0x601`, DLC 8: `01 00 00 00 00 00 00 00`. The **application** must start CAN at the same rate and accept this ID. Copy generated `Core/Inc/bootloader.h` and `Core/Src/bootloader.c` into the application and compile the source. For a bxCAN application on CAN2 (FIFO0 filter in bank ≥ 14):

```c
#include "bootloader.h"

if (HAL_CAN_GetRxFifoFillLevel(&hcan2, CAN_RX_FIFO0) != 0U)
{
    CAN_RxHeaderTypeDef rx;
    uint8_t data[8];
    struct bootloader_enter_boot_t entry;

    if (HAL_CAN_GetRxMessage(&hcan2, CAN_RX_FIFO0, &rx, data) == HAL_OK &&
        rx.IDE == CAN_ID_STD && rx.RTR == CAN_RTR_DATA &&
        rx.StdId == BOOTLOADER_ENTER_BOOT_FRAME_ID && rx.DLC == 8U &&
        bootloader_enter_boot_unpack(&entry, data, rx.DLC) == 0 && entry.bci_boot == 1U)
    {
        RequestBootloader();
    }
}
```

Integrate into an existing dispatcher if it already drains the FIFO; two consumers can steal each other's frames. If the frame arrives in an interrupt callback, set a flag there and call `RequestBootloader()` from the main loop.

**Done when:** `0x601` causes a reset and logs show `RAM magic=1` / `Application held: RAM bootloader request`.

## Step 10 — Build and check the port

```powershell
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target bootloader
cmake --build build --target boot_tests boot_port_tests
qemu-system-arm -M mps2-an386 -nographic -semihosting -kernel build/boot_tests.elf
qemu-system-arm -M mps2-an386 -nographic -semihosting -kernel build/boot_port_tests.elf
.venv\Scripts\python.exe -m unittest discover -s tests -v
```

`test_boot.c` models the F412 sector table and word programming (and checks the diagnostic snapshots); `test_heartbeat.c` compiles the real `boot_port.c` against mocked HAL CAN calls to check heartbeat timing, diagnostic payloads and mailbox backpressure. Update both with the layout. QEMU models shared logic on an M4 machine, not real flash/CAN/option bytes.

**Done when:** builds succeed and the software tests cover the changed geometry.

## Step 11 — Test the board in this order

Use two small distinguishable apps A/B (different LED patterns).

| Test | Action | Expected result |
| --- | --- | --- |
| Logs | Program bootloader only | Banner/flash checks visible; recovery (fast blink) |
| CAN | Send INFO | Reply ID `0x580`, version 1, maximum `0x30000`; heartbeat on `0x581` every 500 ms; diagnostics `0x582`–`0x589` |
| First upload | Upload A's raw `.bin` | Conversion/progress/validation/commit succeed |
| Install | Reset | `Installing staged image`, `Staged image installed`, A runs with interrupts |
| Application entry | Run script while A runs | `0x601` -> flag/reset -> INFO |
| Second upload | Upload B | B installed and running |
| Power cut during install | Cut power while the install log runs | Next boot reinstalls B |
| Damaged primary | Corrupt one primary payload byte with the programmer | Next boot reinstalls from staging |
| Damaged staging | Upload, then corrupt staging before reset | Old application keeps running |
| Bounds | Wrong vectors/oversized image | Rejected; bootloader intact |
| Maximum size | Valid near-limit image | Installs and runs |
| Busy CAN | Unrelated traffic | Correct filtering/ACKs |

Use sector erase, not mass erase, for corruption tests.

## Troubleshooting

| Symptom | First checks |
| --- | --- |
| No logs | UART pins/baud, HSE present, `__io_putchar()` |
| `Flash requires ...` error | Flash size, write protection on sectors 4–7 |
| App runs only with button pressed | Inverted hold polarity |
| No entry after `0x601` | App handler/filter, bitrate, flag reservation |
| INFO works, BEGIN fails | Protection, supply voltage, sector alignment |
| Install loops every reset | Install mark not written (see log); flash error on the record |
| First interrupt faults | VTOR/startup, pending interrupts |
| Hash-valid app hangs | No runtime-health policy; add watchdog/confirmation |

## Security and library limits

Images are unsigned. CRC checks transport; SHA checks against the supplied hash, which someone replacing an image can recompute. `ENTER_BOOT` has no authentication. For authenticated firmware, add a signature algorithm, key handling, matching packaging and tests; the [bundled SHA subset](../Drivers/mbedtls_sha256/README.md) is not a signature library.

## References

- [ST RM0402 — STM32F412 reference manual](https://www.st.com/resource/en/reference_manual/rm0402-stm32f412-advanced-armbased-32bit-mcus-stmicroelectronics.pdf) (flash sectors, bxCAN, option bytes).
- [STM32F412 datasheet](https://www.st.com/resource/en/datasheet/stm32f412re.pdf) (erase/program times, voltage ranges).
- [MCUboot image format](https://docs.mcuboot.com/design.html#image-format).
