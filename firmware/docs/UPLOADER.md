# Python uploader: easy step-by-step guide

**Give the script your application's normal CubeIDE `.bin`. It packages the file, requests bootloader entry, uploads it and requests reset.**

Run commands below from the `firmware` folder. This guide describes the actual [can_upload.py](../tools/can_upload.py); it does not require a separate manual imgtool step. The GUI `CAN_STM32_Programmer.py` in the repository root (mode **Bootloader LART**) imports this same script, so packaging, checks and protocol are identical.

## Quick start

After the one-time setup:

```powershell
.venv\Scripts\python.exe tools/can_upload.py "C:\path\to\YourApplication\Debug\YourApplication.bin" --channel COM5
```

Replace the quoted path and COM port with yours. The input must be an **application** built for the correct application address, not this bootloader's binary.

For the LART STM32F412 setup: application vectors at **`0x08010200`**, CAN **250 kbit/s**, one target, IDs `0x600`, `0x580` and `0x601`. For another MCU, first adapt the [porting guide](PORTING.md#step-8--adapt-the-python-target-assumptions) settings.

## 1 — One-time setup

1. Install Python and open PowerShell in the project root.
2. Create the environment and install dependencies:

   ```powershell
   python -m venv .venv
   .venv\Scripts\python.exe -m pip install -r tools/requirements.txt
   ```

3. Flash the bootloader once using ST-LINK/CubeProgrammer. Use the built `.elf`/`.hex`; a raw bootloader `.bin` starts at `0x08000000`.
4. Connect the CAN transceiver/adapter with correct power, enable state, common ground and termination. STM32 CAN TX/RX pins do not connect directly to CANH/CANL.
5. Find the adapter's COM port in Device Manager. Close other software using that adapter/port.

You do not need to activate `.venv` when using its Python executable as shown.

**Done when:** the bootloader is programmed and you know which adapter port to use.

## 2 — Build the application binary

For a new CubeIDE project, first follow the [application linker guide](APPLICATION_LINKER.md). It explains the required flash origin/limit, RAM request reservation and startup vector configuration.

Use the **application project** in CubeIDE. Generate its raw binary using that project's binary-output/post-build setting, then pass the actual output path to the script.

For this STM32F412 configuration:

| Requirement | Value |
| --- | --- |
| Application FLASH origin/vector table | `0x08010200` |
| Application FLASH length | at most `0x2FDD8` |
| Initial stack | Eight-byte aligned, in SRAM `0x20000000`–`0x20040000` |
| Startup VTOR | Must preserve `0x08010200` (`USER_VECT_TAB_ADDRESS`, `VECT_TAB_OFFSET=0x10200U`) |
| Maximum raw unsigned application | 196056 bytes, `0x2FDD8` |
| Maximum packaged upload | 196608 bytes, `0x30000` |

Every upload uses the same linker origin: the bootloader receives it in the second half of flash and copies it to `0x08010000` before starting it.

**Packaging does not relocate code.** A binary linked at `0x08000000` is not fixed by inserting a header. The uploader checks SP/PC and rejects a wrong-address application before contacting the device.

**Done when:** the raw application's first words are its valid stack/reset vectors, and it fits the payload limit.

## 3 — Make bootloader entry work

Before requesting INFO, script sends:

```text
Message: ENTER_BOOT
ID:      0x601, standard CAN
DLC:     8
Data:    01 00 00 00 00 00 00 00
Signal:  BCI_BOOT=1, bit 0
```

The **running application** must handle that frame by calling `RequestBootloader()`. It writes the RAM flag and resets; the bootloader then stays active. The function, RAM reservation and application receive-loop example are in [porting step 9](PORTING.md#step-9--add-requestbootloader-to-the-application).

The script sends the entry frame **once**, then waits up to 30 seconds for bootloader INFO. It does not resend entry while probing, and there is no separate entry ACK.

If the application has no handler, or there is no application yet, enter manually: hold the PCB entry button during reset while script is waiting. Current PCB behavior is active-low. With no valid image, bootloader stays in recovery without a button. Release the button before the final reset if you want the application to start.

## 4 — Run the uploader

### Default: SLCAN adapter

```powershell
.venv\Scripts\python.exe tools/can_upload.py "C:\firmware\MyApplication.bin" --channel COM5
```

### SocketCAN adapter on Linux

First configure/bring up the SocketCAN interface at the same **250000 bit/s** as the target. Then, using your Linux Python environment with the requirements installed:

```bash
python tools/can_upload.py MyApplication.bin --interface socketcan --channel can0
```

The interface must exist and be configured outside this script; it does not bring SocketCAN up for you.

### Available arguments

| Argument | Required? | Meaning |
| --- | --- | --- |
| `image` | Yes | Raw application `.bin`, or already packaged unpadded MCUboot image |
| `--channel` | Yes | Adapter channel, such as `COM5` or `can0` |
| `--interface` | No | python-can backend; default `slcan` |
| `-h`, `--help` | No | Show usage |

There is currently **no `--bitrate`, `--version`, `--dbc` or `--output` argument**. Bitrate is hardcoded to `250000`; raw packaging uses version 1.0.0; IDs and payload layouts are loaded from `docs/bootloader.dbc` with cantools at startup. Keep that file with the repository when running the script. See [target assumptions below](#target-assumptions-to-change-when-porting) before using a different MCU/network.

## 5 — Understand conversion and progress

For raw `MyApplication.bin`, the script creates **`MyApplication.mcuboot.bin` beside it** and uploads that converted file's contents. The original input is not rewritten. Running again replaces the generated sibling file if it already exists; the input folder must be writable.

The format is:

```text
[512-byte MCUboot header][raw application][40-byte SHA-256 TLV block]
```

SHA covers header + application. Transport CRC covers the **entire packaged file**, including its TLV. No signature key is used. If input already has a valid compatible MCUboot header, script validates and uses it without wrapping it again.

Typical stages are:

| Output/stage | What is happening |
| --- | --- |
| `Packaged ...` / `Using packaged image ...` | Local checks and conversion, before device communication |
| `Sent ENTER_BOOT ...` | One application-entry request sent |
| Waiting for bootloader | INFO probes until a matching reply or deadline |
| Device preparing slot | BEGIN erases the staging half (2 × 128 KiB, a few seconds) and prepares its record; bar appears afterward |
| `Programming [...] N%` | Acknowledged packaged bytes transferred/programmed |
| `Transfer complete; validating and committing image...` | END checks CRC, flash image/hash/vectors and commits |
| `Device SHA-256 validation passed` | Device accepted the image; not proof that the application has run |
| `Reset acknowledged...` | Device reset requested; bootloader installs the staged image into the application slot and starts it |

The progress bar updates when the integer percentage changes, approximately **1% steps**, and shows accepted bytes/total. It includes header/TLV bytes. 100% means transfer complete; validation/commit happens afterward and can still fail.

On startup, UART prints `Installing staged image`, `Staged image installed` and `Boot application`. Observe the application's own behavior/version to confirm the intended firmware is running. A successful installation alone does not prove application health.

## What the script does, in order

1. Read and validate the input; package raw firmware if needed.
2. Open the adapter at the configured bitrate.
3. Send `ENTER_BOOT.BCI_BOOT=1` on `0x601` once.
4. Poll INFO on `0x600`, accepting matching replies on `0x580`.
5. Read the device maximum and reject a larger packaged image before BEGIN.
6. Send BEGIN with packaged length and wait for inactive-slot preparation.
7. Send four payload bytes per DATA frame, each with a 24-bit byte offset. Wait for its matching accepted-count ACK before advancing.
8. Pad a short final DATA payload with `FF`; only declared image bytes count.
9. Send END with `zlib.crc32(image)` and wait for device validation/commit.
10. Send RESET once, then close the bus through its context manager.

The MCU always programs its staging half. The Python script does not erase arbitrary flash addresses.

### Timeouts and retries

| Stage | Reply deadline per attempt | Attempts |
| --- | --- | --- |
| ENTER_BOOT send | 1 second send timeout; no reply expected | One send |
| Waiting INFO probes | 0.3 seconds | Repeated probes within a 30-second wait |
| Upload INFO / DATA | 1 second | Up to 3 |
| BEGIN / END | 20 seconds | Up to 3 |
| RESET | 1 second | One |

Only TimeoutError in the initial INFO wait causes another probe; explicit device/protocol errors are not silently retried. During upload, wrong-ID/opcode/length/type replies are ignored. Device status errors fail immediately. DATA replies must match the expected accepted byte count, avoiding stale ACK advancement.

Device session timeout is separate: receiving/committed states reset after 30 seconds without a successful command. Flash operations and bus load affect timing; measure before increasing deadlines.

## Common problems: what to do next

| Message/symptom | Next check |
| --- | --- |
| Cannot open/read `.bin` | Correct path, quotes around spaces, build output exists |
| Invalid application vectors | Application origin `0x08010200`, initial SP alignment/RAM, correct startup; do not upload bootloader `.bin` |
| Cannot save converted file | Input directory writable; generated file not locked |
| Missing `can`/`serial`/`cantools` module | Use `.venv` Python and install `tools/requirements.txt` |
| Cannot open adapter | COM/channel/backend, driver, unplug/replug, other program holding the port |
| ENTER_BOOT sent, no INFO | Application handler, same bitrate, valid RAM request/reset path; try manual button/reset entry |
| No matching ACK | Wiring/transceiver/termination, fixed IDs, target in bootloader, bitrate and bus load |
| `BAD_SIZE` | Packaged file exceeds device limit or invalid minimum size |
| `BAD_OFFSET` | Transfer offset/session synchronization; reset into bootloader and restart |
| `FLASH_ERROR` | UART details, write protection on sectors 4–7 (option bytes), supply voltage (word programming needs 2.7–3.6 V) |
| `BAD_IMAGE` / SHA failure | Correct format/vectors, damaged image, CRC/hash, flash readback; inspect UART |
| Unsupported protocol version | Host and flashed bootloader wire versions must match |
| Reset ACK missing after acceptance | Image was accepted; reset the board manually to install it |
| Upload succeeds, app does not start | Button still held, startup/VTOR, `Install failed` in the UART log and application faults |

If transfer was interrupted, reset/enter the bootloader and run the command again from the beginning. Host does not resume partial uploads. The staging half is erased on a new BEGIN; the running application is preserved. After successful END, reset before starting another upload.

## Sharing a CAN bus

Other CAN traffic is supported, but reserve **`0x600`, `0x601` and `0x580`–`0x589`**. Only one active target may use these fixed IDs. Same-ID traffic can be mistaken for a command/reply; there is no node addressing or authentication.

The bootloader sends heartbeat `0x581` with tick/version plus structured diagnostics `0x582`–`0x589` ([layouts](CAN_PROTOCOL.md#can-diagnostics)); the uploader ignores those frames and waits for command replies on `0x580`. Heartbeat pauses can occur during flash operations.

A busy bus can slow programming or trigger timeouts because every DATA frame waits for an ACK. Test representative load instead of assuming an unloaded-bus test proves network behavior. The script exposes no bus-load or node-selection option.

## Target assumptions to change when porting

| Script location | Current assumption |
| --- | --- |
| `docs/bootloader.dbc` | IDs/layout loaded by cantools: `0x600`, `0x580`, `0x601` |
| `can.Bus()` | 250 kbit/s, default SLCAN |
| `APP_ADDRESS`, `RAM_START`/`RAM_END`, `MAX_IMAGE` | `0x08010200`, F412 SRAM `0x20000000`–`0x20040000`, 196608-byte packaged limit |
| Header construction / `check_image()` | 512-byte header, no protected TLVs/flags; SHA-only raw wrapper |
| Raw header version | 1.0.0; no CLI version parameter |
| DATA framing | Classic standard-ID frames, four image bytes per frame, protocol 1 |
| Deadlines | Values shown above |

Keep MCU configuration, application/linker/startup, host validation and [DBC](bootloader.dbc) consistent. INFO's device limit does not update the host-side constants automatically. Full procedure: [porting guide](PORTING.md).

## Code map for maintainers

| Function | Responsibility |
| --- | --- |
| `check_image(image)` | Check packaged header/TLV bounds and exactly one matching SHA TLV; return image-content digest |
| `prepare_image(data)` | Detect packaged vs raw input; add header/hash if raw; check vectors/size |
| `print_progress(received, total)` | Render percentage and byte count |
| `request(bus, payload, ...)` | Send/retry one command, filter replies, check version/status/expected count |
| `command(cmd, value)` | Build zero-padded 8-byte command with a little-endian value |
| `wait_for_bootloader(bus, timeout)` | Probe INFO until the bootloader answers (application reset time) |
| `upload(bus, image, progress, log)` | INFO -> BEGIN -> DATA -> END -> RESET; callbacks default to the console (the GUI passes its own) |
| `main()` | CLI, read/save packaging, open bus, application entry and initial INFO wait |

Regression tests are in [test_uploader.py](../tests/test_uploader.py) (entry frame/order, conversion/vector rejection, ACK filtering including heartbeat/diagnostic IDs, retries, status errors and percentage updates) and [test_dbc.py](../tests/test_dbc.py) (generated C codec matches the DBC; every message round-trips; diagnostic payloads match the MCU test fixtures).

```powershell
.venv\Scripts\python.exe -m unittest discover -s tests -v
```

These use modeled replies; they do not program real hardware. CRC/SHA here detect corruption, not firmware authorship. Runtime health confirmation/watchdog is separate from the current integrity-based installation.
