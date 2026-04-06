# AN2606 — STM32 Microcontroller System Memory Boot Mode

**Document number:** AN2606  
**Title:** STM32 microcontroller system memory boot mode  
**Publisher:** STMicroelectronics  
**Link:** [https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf)

---

## Summary

AN2606 is the master reference for the STM32 system memory (ROM) bootloader. It documents:

- Which communication interfaces are supported on each MCU part number (UART, CAN, USB DFU, SPI, I2C, FDCAN, …)
- The system memory address for each MCU family
- The BOOT pin configuration required to enter bootloader mode on power-on
- The version of the bootloader embedded in each MCU

---

## System Memory Addresses (Selected Families)

| MCU Family | System Memory Base | Bootloader Size |
|---|---|---|
| STM32F0xx | `0x1FFFC800` | 4 KB |
| **STM32F1xx** | **`0x1FFFF000`** | **2 KB** |
| STM32F2xx | `0x1FFF0000` | 30 KB |
| STM32F3xx | `0x1FFFD800` | 8 KB |
| **STM32F4xx** | **`0x1FFF0000`** | **29 KB** |
| STM32G0xx | `0x1FFF0000` | variable |
| STM32G4xx | `0x1FFF0000` | variable |
| STM32L4xx | `0x1FFF0000` | variable |
| STM32H7xx | `0x1FF09800` | variable |

> ⚠️ Always verify using the AN2606 document for your specific part number. Addresses vary within families.

---

## Boot Pin Configuration

Most STM32 MCUs use the `BOOT0` (and sometimes `BOOT1`) pin sampled at reset:

| BOOT0 | BOOT1 | Boot mode |
|---|---|---|
| 0 | × | User flash (normal application) |
| 1 | 0 | System memory bootloader |
| 1 | 1 | Embedded SRAM |

When jumping to the bootloader via software (as done in this project), the boot pin state is irrelevant — the application directly transfers control to the bootloader reset handler.

---

## CAN Interface Support

AN2606 specifies which interfaces are available on each MCU. For the CAN bootloader:

- **STM32F103CBT6** — CAN1 supported (`0x1FFFF000`)
- **STM32F412** — CAN1 and CAN2 supported (`0x1FFF0000`); the bootloader uses the CAN2 pins by default (PB5/PB13)

Always check the AN2606 table for your part before assuming CAN bootloader support.

---

## Software-Triggered Jump (No BOOT Pin Required)

AN2606 §3 describes the requirements for a successful software jump to the bootloader:

1. All peripheral clocks must be properly shut down.
2. The system clock must be reset to the default (HSI or the POR state).
3. SysTick must be disabled.
4. All NVIC pending interrupts must be cleared.
5. The system memory must be mapped to `0x00000000` (via `SYSCFG->MEMRMP` or AFIO on F1).
6. The vector table base register (`SCB->VTOR`) must point to `0x00000000`.
7. The main stack pointer must be loaded from the bootloader's vector table.

The `JumpToBootloader()` function in the example project implements all of these steps.

---

## Related Application Notes

| AN | Protocol |
|---|---|
| AN3154 | CAN |
| AN3155 | USART |
| AN3156 | USB DFU |
| AN4221 | I2C |
| AN4286 | SPI |
| AN5405 | FDCAN |
