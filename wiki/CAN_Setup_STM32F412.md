# CAN Setup Guide — STM32F412

This guide covers everything needed to configure the CAN peripheral on the **STM32F412** family for use with the system memory CAN bootloader (AN3154/AN2606) and this toolchain.

---

## Overview

The STM32F412 has **two CAN controllers**: CAN1 and CAN2.

- **CAN1**  owns the full filter bank array (banks 0–27).
- **CAN2**  shares the filter banks starting from the bank configured in `CAN2->FMR.CAN2SB` (the `SlaveStartFilterBank` field in HAL). Both controllers must be clock-enabled for CAN2 to work, even if CAN1 is unused for message traffic.

The system memory bootloader on the STM32F412 communicates over **CAN2**. The bootloader runs at a fixed **125 kbps**.

---

## System Clock and APB1 Frequency

CAN peripherals are clocked from **APB1**. The bitrate formula is:

```
Bitrate = APB1_CLK / (Prescaler × (1 + TimeSeg1 + TimeSeg2))
```

A typical clock configuration for the STM32F412 @ 100 MHz:

| Source | Value |
|---|---|
| HSE | 8 MHz |
| PLL_M | 4 |
| PLL_N | 100 |
| PLL_P | 2 → SYSCLK = 100 MHz |
| APB1 divider | /4 → **APB1 = 25 MHz** |

> Check your own `.ioc` file or `SystemClock_Config()` to confirm your APB1 frequency. The example project in this repo uses `APB1 = 32 MHz`.

---

## Pin Selection

### CAN2

| Function | Default pins | Alternate |
|---|---|---|
| CAN2_TX | PB13 | PB6 |
| CAN2_RX | PB5 | PB12 |

AF: `GPIO_AF9_CAN2`

> The example project uses **PB5 (RX)** and **PB13 (TX)** for CAN2 — these are the pins used by the STM32 system memory bootloader on this MCU (see AN2606 §4).

---

## Bitrate Settings

### 250 kbps on CAN2 (APB1 = 32 MHz)

This is what the example project uses for the application-side CAN:

| Parameter | Value |
|---|---|
| Prescaler | 16 |
| SJW | 1 TQ |
| TimeSeg1 (BS1) | 2 TQ |
| TimeSeg2 (BS2) | 5 TQ |
| **Total TQ** | 1 + 2 + 5 = **8 TQ** |
| **Bitrate** | 32 MHz / (16 × 8) = **250 kbps** ✓ |

### 125 kbps on CAN2 (APB1 = 32 MHz) — Bootloader bitrate

| Parameter | Value |
|---|---|
| Prescaler | 32 |
| SJW | 1 TQ |
| TimeSeg1 (BS1) | 2 TQ |
| TimeSeg2 (BS2) | 5 TQ |
| **Bitrate** | 32 MHz / (32 × 8) = **125 kbps** ✓ |

### Common bitrates at APB1 = 25 MHz

| Bitrate | Prescaler | BS1 | BS2 | SJW | Sample Point |
|---|---|---|---|---|---|
| 1 Mbit/s | 2 | 11 TQ | 2 TQ | 1 TQ | 85.7% |
| 500 kbps | 4 | 11 TQ | 2 TQ | 1 TQ | 85.7% |
| 250 kbps | 8 | 11 TQ | 2 TQ | 1 TQ | 85.7% |
| 125 kbps | 16 | 11 TQ | 2 TQ | 1 TQ | 85.7% |

Use [**bittiming calculator**](http://www.bittiming.can-wiki.info/) to verify for your specific APB1.

---

## CubeMX / HAL Initialisation

### CAN1 init (required even if CAN1 carries no traffic — needed to enable filter banks for CAN2)

```c
hcan1.Instance               = CAN1;
hcan1.Init.Prescaler         = 8;
hcan1.Init.Mode              = CAN_MODE_NORMAL;
hcan1.Init.SyncJumpWidth     = CAN_SJW_1TQ;
hcan1.Init.TimeSeg1          = CAN_BS1_2TQ;
hcan1.Init.TimeSeg2          = CAN_BS2_5TQ;
hcan1.Init.TimeTriggeredMode = DISABLE;
hcan1.Init.AutoBusOff        = ENABLE;
hcan1.Init.AutoWakeUp        = DISABLE;
hcan1.Init.AutoRetransmission = ENABLE;
hcan1.Init.ReceiveFifoLocked = DISABLE;
hcan1.Init.TransmitFifoPriority = DISABLE;
HAL_CAN_Init(&hcan1);
```

### CAN2 init (250 kbps @ APB1 32 MHz)

```c
hcan2.Instance               = CAN2;
hcan2.Init.Prescaler         = 16;
hcan2.Init.Mode              = CAN_MODE_NORMAL;
hcan2.Init.SyncJumpWidth     = CAN_SJW_1TQ;
hcan2.Init.TimeSeg1          = CAN_BS1_2TQ;
hcan2.Init.TimeSeg2          = CAN_BS2_5TQ;
hcan2.Init.TimeTriggeredMode = DISABLE;
hcan2.Init.AutoBusOff        = ENABLE;
hcan2.Init.AutoWakeUp        = DISABLE;
hcan2.Init.AutoRetransmission = ENABLE;
hcan2.Init.ReceiveFifoLocked = DISABLE;
hcan2.Init.TransmitFifoPriority = DISABLE;
HAL_CAN_Init(&hcan2);
```

---

## Filter Banks

The STM32F412 has **28 filter banks** (0–27). The split between CAN1 and CAN2 is configured by `SlaveStartFilterBank`:

- Banks **0 to (SlaveStartFilterBank−1)** → CAN1
- Banks **SlaveStartFilterBank to 27** → CAN2

In the example project, `SlaveStartFilterBank = 14`, so:

- Banks 0–13 → CAN1
- Banks 14–27 → CAN2

### Pass-all filter for CAN2 FIFO1 (bootloader jump receiver)

```c
CAN_FilterTypeDef f = {
    .FilterBank           = 14,            // First CAN2 bank
    .FilterMode           = CAN_FILTERMODE_IDMASK,
    .FilterScale          = CAN_FILTERSCALE_32BIT,
    .FilterIdHigh         = 0x0000,
    .FilterIdLow          = 0x0000,
    .FilterMaskIdHigh     = 0x0000,        // Mask = 0 → accept all IDs
    .FilterMaskIdLow      = 0x0000,
    .FilterFIFOAssignment = CAN_RX_FIFO1,  // CAN2 must use FIFO1 on F412
    .FilterActivation     = ENABLE,
    .SlaveStartFilterBank = 14,
};
HAL_CAN_ConfigFilter(&hcan2, &f);
```

> **Important:** On the STM32F412, CAN1 uses FIFO0 (`CAN_IT_RX_FIFO0_MSG_PENDING` → `HAL_CAN_RxFifo0MsgPendingCallback`) and CAN2 uses FIFO1 (`CAN_IT_RX_FIFO1_MSG_PENDING` → `HAL_CAN_RxFifo1MsgPendingCallback`). Mixing these up is a very common bug.

---

## Interrupt Architecture

```c
// CAN1 → FIFO0 → RxFifo0MsgPendingCallback
HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);

// CAN2 → FIFO1 → RxFifo1MsgPendingCallback
HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO1_MSG_PENDING);
```

Callbacks:

```c
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
    // Handle CAN1 messages here
}

void HAL_CAN_RxFifo1MsgPendingCallback(CAN_HandleTypeDef *hcan) {
    // Handle CAN2 messages here
    if (hcan->Instance != CAN2) return;
    // ...
}
```

---

## Bootloader Jump Implementation

See [`../examples/f412_example_jump_to_bootloader/Core/Src/bootloader_jumper.c`](../examples/f412_example_jump_to_bootloader/Core/Src/bootloader_jumper.c) for the full, commented implementation.

Key points:

1. Deactivate CAN notifications, stop and de-init CAN2, then call `HAL_DeInit()` and `HAL_RCC_DeInit()` — in that order (HAL functions require SysTick to still be running).
2. Disable interrupts, zero out SysTick.
3. Clear all NVIC interrupt enable and pending registers.
4. Remap system flash to `0x00000000` via `__HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH()`.
5. Set `SCB->VTOR = 0x00000000`.
6. Switch to MSP (`__set_CONTROL(0x00)`) and issue `__ISB()`.
7. Load SP from `*(uint32_t*)0x1FFF0000` and jump to `*(uint32_t*)0x1FFF0004`.

> The system memory bootloader base for the STM32F412 is **`0x1FFF0000`** (see AN2606 Table 2).

### The Flag Pattern (Thread Mode jump)

Do **not** call `JumpToBootloader()` directly from an IRQ context. Set a volatile flag in the callback and call the jump function from the main loop:

```c
// In callback (IRQ context):
triggerJumpToBootloader = 1;

// In main loop (Thread Mode):
if (triggerJumpToBootloader) {
    JumpToBootloader();
}
```

---

## Physical Layer Wiring

```
STM32F412 Board          CAN Transceiver        SLCAN Adapter / Bus
┌──────────────┐         ┌──────────────┐        ┌─────────────┐
│ CAN2_TX PB13 ├────────►│ TXD      CANH├────────┤ CAN H       │
│ CAN2_RX PB5  │◄────────┤ RXD      CANL├────────┤ CAN L       │
│ 3.3V         ├────────►│ VCC           │        └─────────────┘
│ GND          ├────────►│ GND           │         (120Ω at each end)
└──────────────┘         └──────────────┘
```

Common transceivers: **MCP2562** (3.3/5 V compatible).

---

## Checklist

- [ ] Both `MX_CAN1_Init()` and `MX_CAN2_Init()` called in `main.c`
- [ ] `SlaveStartFilterBank` consistent between filter config and CAN1/CAN2 split
- [ ] CAN2 filter assigned to **FIFO1**, notification on `CAN_IT_RX_FIFO1_MSG_PENDING`
- [ ] 120 Ω termination at both ends of the bus
- [ ] APB1 frequency confirmed before computing prescaler
- [ ] `JumpToBootloader()` called from Thread Mode (main loop), not from IRQ
- [ ] SLCAN adapter running at same bitrate as application CAN before sending jump frame
