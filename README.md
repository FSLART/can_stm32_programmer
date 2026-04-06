# STM32 CAN Bootloader Programmer

A Python-based GUI tool to program STM32 microcontrollers over CAN bus, using the STM32 bootloader and the CAN protocol described in **AN3154**.

---

## How It Works

STM32 microcontrollers include a factory-programmed **system memory bootloader** that supports several communication interfaces — including CAN. When the MCU enters bootloader mode (either via the BOOT0 pin or by a software-triggered jump from the application), it listens for commands over CAN at **125 kbps**.

This tool automates the full programming flow:

1. **Jump trigger** — Sends a configurable CAN frame to the running application to request a jump to the system bootloader (e.g. CAN ID `0x0DC`, data `0x01`).
2. **Reconnect at 125 kbps** — Closes the SLCAN interface and reopens it at the bootloader's fixed bitrate.
3. **Sync** — Exchanges the synchronisation frame (`CAN ID = 0x079`, data `0x00`) until the bootloader acknowledges.
4. **Get ID** — Reads the MCU product ID (`CMD_GET_ID`, `CAN ID = 0x02`) for sanity checking.
5. **Global Erase** — Sends the erase command (`CMD_ERASE`, `CAN ID = 0x43`, data `0xFF`).
6. **Write** — Programs the binary image in 256-byte chunks using `CMD_WRITE` (`CAN ID = 0x31`) followed by data frames (`CAN ID = 0x04`), with ACK/NACK checking after every frame.
7. **Go** — Sends the jump-to-application command (`CMD_GO`, `CAN ID = 0x21`) pointing to the flash base address (`0x08000000`).

The tool communicates with the CAN bus via an **SLCAN-compatible USB adapter** (CANable, Lawicel, etc.).

### Protocol Reference

All CAN bootloader commands follow **AN3154** (*CAN protocol used in the STM32 bootloader*). The system memory bootloader entry points for each MCU family are documented in **AN2606** (*STM32 microcontroller system memory boot mode*).

---

## Requirements
 
```bash
pip install pyqt5 python-can pyserial cantools
```
 
| Package | Purpose |
|---|---|
| `pyqt5` | GUI framework |
| `python-can` | CAN bus abstraction (SLCAN backend) |
| `pyserial` | Serial port enumeration for SLCAN adapters |
| `cantools` | DBC file parsing and CAN message encoding/decoding |
 
---

## Usage

```bash
python CAN_STM32_Programmer.py
```

### GUI Fields

| Field | Description |
|---|---|
| **COM Port** | Serial port of your SLCAN adapter (e.g. `COM7`, `/dev/ttyUSB0`). Click **Refresh** to detect. |
| **CAN ID (Hex)** | ID of the CAN frame sent to trigger the bootloader jump in your application. Default: `0x0DC`. |
| **Data (Hex, space-separated)** | Payload of the jump trigger frame. Default: `01`. |
| **Bitrate (normal CAN)** | Bitrate of your application CAN bus (1 Mbit/s, 500k, 250k, 125k). The tool automatically switches to 125 kbps for the bootloader phase. |
| **Firmware file** | Path to the `.bin` file to flash. |

Press **Iniciar Programação do STM32** to start. The log console shows every step in real time.

> **Note:** If the jump trigger ID/data fields are left as configured, they will match the defaults used in the provided STM32F412 example project (`CAN ID = 0x0DC`, `data[0] bit 0 = 1`).

---

## Repository Structure

```
.
├── CAN_STM32_Programmer.py      # Main application — GUI CAN flasher
├── README.md
├── examples/
│   └── f412_example_jump_to_bootloader/   # STM32F412 example (STM32CubeIDE project)
│       ├── Core/
│       │   ├── Src/
│       │   │   ├── main.c
│       │   │   └── bootloader_jumper.c    # CAN RX handler + JumpToBootloader()
│       │   └── Inc/
│       │       └── main.h
│       ├── STM32F412RETX_FLASH.ld
│       ├── STM32F412RETX_RAM.ld
│       └── f412_example_jump_to_bootloader.ioc
├── wiki/
│   ├── CAN_Setup_STM32F412.md             # CAN peripheral setup guide for STM32F412
│   └── CAN_Setup_STM32F103CBT6.md         # CAN peripheral setup guide for STM32F103CBT6
└── support/
    ├── AN3154_CAN_bootloader_protocol.md   # Summary + link — CAN bootloader protocol
    ├── AN2606_system_memory_boot_mode.md   # Summary + link — Bootloader entry per MCU family
    └── gonzabrusco_jump_to_bootloader.md   # Jump-to-bootloader guide (via gonzabrusco gist)
```

---

## STM32F412 Example Project

See [`examples/f412_example_jump_to_bootloader/`](examples/f412_example_jump_to_bootloader/).

This is a minimal STM32CubeIDE project for the **STM32F412RETx** that:

- Initialises CAN1 and CAN2 in HAL
- Configures a CAN2 receive filter (FIFO1, filter bank 14, pass-all mask) via `CAN_Setup_Bootloader_Jumper()`
- In the FIFO1 RX callback, checks for `CAN ID = 0x0DC` with `data[0] & 0x01 == 1` and sets a flag
- In the main loop, checks the flag and calls `JumpToBootloader()` from Thread Mode

**CAN2 configuration (250 kbps @ 32 MHz APB1):**

| Parameter | Value |
|---|---|
| Prescaler | 16 |
| TimeSeg1 | 2 TQ |
| TimeSeg2 | 5 TQ |
| SJW | 1 TQ |
| Resulting bitrate | 250 kbps |

---

## Wiki

Step-by-step CAN peripheral setup guides:

- [STM32F412 CAN Setup](wiki/CAN_Setup_STM32F412.md) — Dual CAN, filter banks, FIFO assignment, SLCAN adapter wiring
- [STM32F103CBT6 CAN Setup](wiki/CAN_Setup_STM32F103CBT6.md) — Single CAN, pin remapping, bootloader address differences

---

## Support / References

- [AN3154 — CAN protocol used in the STM32 bootloader](support/AN3154_CAN_bootloader_protocol.md)
- [AN2606 — STM32 microcontroller system memory boot mode](support/AN2606_system_memory_boot_mode.md)
- [How to Jump to the STM32 Bootloader and Return to the Application (gonzabrusco)](support/gonzabrusco_jump_to_bootloader.md)

---

## Hardware Setup

```
  STM32 Board          SLCAN Adapter       PC
  ┌──────────┐        ┌────────────┐      ┌──────┐
  │ CAN2_TX  ├──┐  ┌──┤ CAN H      │      │      │
  │ (PB13)   │  └──┘  │            ├──────┤ USB  │
  │ CAN2_RX  ├──┐  ┌──┤ CAN L      │      │      │
  │ (PB5)    │  └──┘  │            │      └──────┘
  └──────────┘        └────────────┘
               
```

> Replace `PB5`/`PB13` with your actual CAN RX/TX pins. Add a 120 Ω termination resistor at each end of the CAN bus if your adapter does not include one.

---

