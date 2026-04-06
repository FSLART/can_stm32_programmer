# CAN Setup Guide — STM32F103CBT6

> ⚠️ **Important — CAN Bootloader NOT Supported on STM32F103**
>
> According to **AN2606 §21 (Table 45)**, the STM32F10xxx system memory bootloader only supports **USART1** (PA9/PA10). CAN is **not** available in the factory ROM bootloader of any STM32F103 device.
>
> If you need CAN-based firmware update on an STM32F103, you must implement a **custom bootloader** in user flash (e.g. at `0x08000000`) that receives firmware over CAN and then self-programs the application area. This is a significantly different approach from the system memory flow used for the STM32F412.
>
> This guide covers **CAN peripheral setup for application firmware** on the STM32F103CBT6. The bootloader jump section describes jumping to the USART bootloader (the only factory option), or to a custom CAN bootloader you implement yourself.

This guide covers CAN peripheral configuration for the **STM32F103CBT6** for use in application firmware.

---

## Overview

The STM32F103CBT6 has **one CAN controller**: CAN1.

- 14 filter banks (banks 0–13), all belonging to CAN1.
- No CAN2. The slave start filter bank concept does not apply.
- System memory bootloader address: **`0x1FFFF000`** — but the factory bootloader only supports **USART1** (see AN2606 §21, Table 45).
- **CAN bootloader: not available in ROM.** A custom user-flash bootloader is required for CAN-based programming.

The STM32F103CBT6 is in the **medium-density** product line (64 KB flash, 20 KB RAM).

---


## Key Differences vs. STM32F412

| Feature | STM32F412 | STM32F103CBT6 |
|---|---|---|
| CAN controllers | 2 (CAN1 + CAN2) | 1 (CAN1 only) |
| Filter banks | 28 (split between CAN1/CAN2) | 14 (CAN1 only) |
| Bootloader base | `0x1FFF0000` | `0x1FFFF000` |
| **Factory CAN bootloader** | **Yes (AN3154)** | **No — USART only** |
| Default CAN pins | CAN2: PB5/PB13 | CAN1: PA11/PA12 |
| Alternate CAN pins | CAN2: PB6/PB12 | CAN1: PB8/PB9 (AFIO remap) |
| FIFO for app RX | FIFO1 (CAN2) | FIFO0 (CAN1) |
| Memory remap macro | `__HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH()` | Not available on F1 |
| Typical APB1 | up to 50 MHz | 36 MHz |

---

## Checklist

- [ ] **Do not** attempt to use this tool's CAN bootloader flow with an STM32F103 — factory bootloader is USART only
- [ ] For CAN-based OTA on F103, implement a custom user-flash bootloader
- [ ] If using USB alongside CAN in application, remap CAN to PB8/PB9 (enable AFIO clock first)
- [ ] Only CAN1 / FIFO0 / `HAL_CAN_RxFifo0MsgPendingCallback` — there is no CAN2 on F103
- [ ] `SlaveStartFilterBank` field set to 14 by convention (no actual slave on F103)
- [ ] Any bootloader jump called from Thread Mode (main loop), not from IRQ
- [ ] 120 Ω termination at both ends of the bus
