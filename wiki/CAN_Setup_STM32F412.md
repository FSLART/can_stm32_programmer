# CAN Setup Guide — STM32F412

This guide covers everything needed to configure the CAN peripheral on the **STM32F412** family for use with this toolchain, with either bootloader:

| Bootloader | Where | CAN | Bitrate | Entry from the application |
|---|---|---|---|---|
| **LART** ([`../firmware/`](../firmware/)) | User flash `0x08000000` | CAN2 PB5/PB13, IDs `0x600`/`0x580`, heartbeat `0x581`, diagnostics `0x582`–`0x589` | **250 kbps** | `ENTER_BOOT` (`0x601`) → RAM flag + reset |
| **ST ROM** (AN3154/AN2606) | System memory `0x1FFF0000` | CAN2 PB5/PB13, AN3154 IDs | **125 kbps** | Software jump to system memory |

---

## Overview

The STM32F412 has **two CAN controllers**: CAN1 and CAN2.

- **CAN1** owns the full filter bank array (banks 0–27).
- **CAN2** uses the banks from `CAN_FMR.CAN2SB` upwards (the `SlaveStartFilterBank` field in HAL). The CAN1 clock must be enabled for CAN2 to work, even if CAN1 carries no traffic, because the filter registers live in CAN1. CubeMX's MSP code does this with the `HAL_RCC_CAN1_CLK_ENABLED` counter.

Both the ST ROM bootloader and the LART bootloader communicate over **CAN2 (PB5/PB13)** on this MCU.

---

## System Clock and APB1 Frequency

CAN peripherals are clocked from **APB1** (max 50 MHz). The bitrate formula is:

```
Bitrate = APB1_CLK / (Prescaler × (1 + TimeSeg1 + TimeSeg2))
```

Clock used by the LART board, the LART bootloader and the original jump example:

| Source | Value |
|---|---|
| HSE | 8 MHz |
| PLL_M | 4 |
| PLL_N | 64 |
| PLL_P | 2 → SYSCLK = 64 MHz |
| APB1 divider | /2 → **APB1 = 32 MHz** |
| Flash latency | 1 WS |

A 100 MHz alternative (HSE 8 MHz, M 4, N 100, P 2, APB1 /4) gives APB1 = 25 MHz. Check your own `.ioc` or `SystemClock_Config()`.

---

## Pin Selection

### CAN2

| Function | Default pins | Alternate |
|---|---|---|
| CAN2_TX | PB13 | PB6 |
| CAN2_RX | PB5 | PB12 |

AF: `GPIO_AF9_CAN2`

> PB5 (RX) and PB13 (TX) are the pins used by the STM32 system memory bootloader on this MCU (see AN2606) and by the LART bootloader.

---

## Bitrate Settings

### 250 kbps on CAN2 (APB1 = 32 MHz) — LART bootloader

| Parameter | Value |
|---|---|
| Prescaler | 8 |
| SJW | 1 TQ |
| TimeSeg1 (BS1) | 13 TQ |
| TimeSeg2 (BS2) | 2 TQ |
| **Total TQ** | 1 + 13 + 2 = **16 TQ** |
| **Bitrate** | 32 MHz / (8 × 16) = **250 kbps** ✓, sample point 87.5 % |

The original jump example used prescaler 16, BS1 2 TQ, BS2 5 TQ (8 TQ, sample point 37.5 %). It also gives 250 kbps, but the sample point is far from the usual 75–87.5 %; prefer the values above for new applications on the same bus.

### 125 kbps on CAN2 (APB1 = 32 MHz) — ROM bootloader bitrate

| Parameter | Value |
|---|---|
| Prescaler | 16 |
| SJW | 1 TQ |
| TimeSeg1 (BS1) | 13 TQ |
| TimeSeg2 (BS2) | 2 TQ |
| **Bitrate** | 32 MHz / (16 × 16) = **125 kbps** ✓ |

(The ROM bootloader configures its own timing; this table is only for an application that must talk at 125 kbps.)

### Common bitrates at APB1 = 32 MHz (sample point 87.5 %)

| Bitrate | Prescaler | BS1 | BS2 | SJW |
|---|---|---|---|---|
| 1 Mbit/s | 2 | 13 TQ | 2 TQ | 1 TQ |
| 500 kbps | 4 | 13 TQ | 2 TQ | 1 TQ |
| 250 kbps | 8 | 13 TQ | 2 TQ | 1 TQ |
| 125 kbps | 16 | 13 TQ | 2 TQ | 1 TQ |

### Common bitrates at APB1 = 25 MHz

| Bitrate | Prescaler | BS1 | BS2 | SJW | Sample Point |
|---|---|---|---|---|---|
| 1 Mbit/s | 2 | 11 TQ | 2 TQ | 1 TQ | 85.7% |
| 500 kbps | 4 | 11 TQ | 2 TQ | 1 TQ | 85.7% |
| 250 kbps | 8 | 11 TQ | 2 TQ | 1 TQ | 85.7% |
| 125 kbps | 16 | 11 TQ | 2 TQ | 1 TQ | 85.7% |

Use a [**bittiming calculator**](http://www.bittiming.can-wiki.info/) to verify for your specific APB1.

---

## CubeMX / HAL Initialisation

### CAN2 init (250 kbps @ APB1 32 MHz)

```c
hcan2.Instance               = CAN2;
hcan2.Init.Prescaler         = 8;
hcan2.Init.Mode              = CAN_MODE_NORMAL;
hcan2.Init.SyncJumpWidth     = CAN_SJW_1TQ;
hcan2.Init.TimeSeg1          = CAN_BS1_13TQ;
hcan2.Init.TimeSeg2          = CAN_BS2_2TQ;
hcan2.Init.TimeTriggeredMode = DISABLE;
hcan2.Init.AutoBusOff        = ENABLE;
hcan2.Init.AutoWakeUp        = DISABLE;
hcan2.Init.AutoRetransmission = ENABLE;
hcan2.Init.ReceiveFifoLocked = DISABLE;
hcan2.Init.TransmitFifoPriority = DISABLE;
HAL_CAN_Init(&hcan2);
```

`MX_CAN1_Init()` is only needed if CAN1 carries traffic. For CAN2 alone, the MSP must still enable the CAN1 clock:

```c
__HAL_RCC_CAN2_CLK_ENABLE();
HAL_RCC_CAN1_CLK_ENABLED++;
if (HAL_RCC_CAN1_CLK_ENABLED == 1) {
    __HAL_RCC_CAN1_CLK_ENABLE();
}
```

The LART bootloader enables `AutoBusOff` and disables `AutoRetransmission` (each reply is sent once and checked with a 100 ms timeout; heartbeat/diagnostics are best-effort), so a board alone on the bus does not fill its mailboxes. An application normally keeps retransmission enabled.

---

## Filter Banks

The STM32F412 has **28 filter banks** (0–27). The split between CAN1 and CAN2 is configured by `SlaveStartFilterBank`:

- Banks **0 to (SlaveStartFilterBank−1)** → CAN1
- Banks **SlaveStartFilterBank to 27** → CAN2

With `SlaveStartFilterBank = 14` (used by the LART bootloader and the jump example): banks 0–13 → CAN1, banks 14–27 → CAN2.

### Accept only `0x601` on CAN2 (LART bootloader entry receiver)

```c
CAN_FilterTypeDef f = {
    .FilterBank           = 14,              // First CAN2 bank
    .FilterMode           = CAN_FILTERMODE_IDMASK,
    .FilterScale          = CAN_FILTERSCALE_32BIT,
    .FilterIdHigh         = 0x601 << 5,      // Standard ID in bits 31..21
    .FilterIdLow          = 0x0000,
    .FilterMaskIdHigh     = 0x7FF << 5,      // Compare all 11 ID bits
    .FilterMaskIdLow      = 0x0006,          // ...and IDE/RTR = 0 (standard data frame)
    .FilterFIFOAssignment = CAN_RX_FIFO0,
    .FilterActivation     = ENABLE,
    .SlaveStartFilterBank = 14,
};
HAL_CAN_ConfigFilter(&hcan2, &f);
```

For a pass-all filter set the ID and mask fields to 0. Any filter can feed **FIFO0 or FIFO1**, on either controller; the FIFO is chosen per filter by `FilterFIFOAssignment`. The LART bootloader uses FIFO0 on CAN2, the jump example used FIFO1.

> **Important:** the notification and callback must match the FIFO the filter feeds: FIFO0 → `CAN_IT_RX_FIFO0_MSG_PENDING` → `HAL_CAN_RxFifo0MsgPendingCallback`, FIFO1 → `CAN_IT_RX_FIFO1_MSG_PENDING` → `HAL_CAN_RxFifo1MsgPendingCallback`. Mixing these up is a very common bug. Check `hcan->Instance` in the callback if both controllers share it.

---

## Interrupt Architecture

```c
HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING);   // CAN2 filter → FIFO0
```

```c
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan) {
    if (hcan->Instance != CAN2) return;
    // ...
}
```

---

## LART Bootloader Entry (`ENTER_BOOT`)

The uploader (GUI mode **LART** or `firmware/tools/can_upload.py`) sends one standard frame **ID `0x601`, DLC 8, data `01 00 00 00 00 00 00 00`** at the application bitrate. The application writes a RAM flag and resets; the bootloader sees the flag and stays in programming mode.

```c
#define BOOTLOADER_FLAG_ADDR  0x2003FFFCUL   // last SRAM word, reserved in both linker scripts
#define BOOTLOADER_MAGIC      0xB00710ADUL

volatile uint8_t enterBootloader = 0;

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef rx;
    uint8_t data[8];
    if (hcan->Instance != CAN2 || HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx, data) != HAL_OK)
        return;
    if (rx.IDE == CAN_ID_STD && rx.RTR == CAN_RTR_DATA && rx.StdId == 0x601U &&
        rx.DLC == 8U && (data[0] & 1U) != 0U)
        enterBootloader = 1;   // act in the main loop, not in the IRQ
}

void RequestBootloader(void)   // call from the main loop when enterBootloader is set
{
    *(volatile uint32_t *)BOOTLOADER_FLAG_ADDR = BOOTLOADER_MAGIC;
    __DSB();
    NVIC_SystemReset();
}
```

With the generated codec from the firmware (`Core/Inc/bootloader.h`, `Core/Src/bootloader.c`, built from `docs/bootloader.dbc`) the check becomes `rx.StdId == BOOTLOADER_ENTER_BOOT_FRAME_ID && bootloader_enter_boot_unpack(&entry, data, rx.DLC) == 0 && entry.bci_boot == 1`.

While the bootloader runs, other boards can watch its heartbeat on `0x581` (`55`, tick, version) and diagnostics on `0x582`–`0x589`. Do not reuse `0x580`–`0x589`, `0x600` or `0x601` for application traffic.

Application project settings for the LART bootloader:

- Linker FLASH origin **`0x08010200`**, length ≤ `0x2FDD8`; RAM length `256K - 8` (keeps `0x2003FFFC` free).
- `USER_VECT_TAB_ADDRESS` and `VECT_TAB_OFFSET=0x10200U` in `system_stm32f4xx.c`.
- Upload the normal CubeIDE `.bin`; the tool adds the MCUboot header itself.
- Step-by-step linker/vector guide: [`../firmware/docs/APPLICATION_LINKER.md`](../firmware/docs/APPLICATION_LINKER.md).

Full details: [`../firmware/docs/PORTING.md`](../firmware/docs/PORTING.md#step-9--add-requestbootloader-to-the-application).

---

## ROM Bootloader Jump Implementation

For the GUI mode **ST ROM — AN3154**, the application jumps directly into system memory. Implementation (from the original `bootloader_jumper.c` example):

```c
#define BOOTLOADER_BASE_ADDR  0x1FFF0000U

void JumpToBootloader(void)
{
    uint32_t blStack = *(__IO uint32_t *)BOOTLOADER_BASE_ADDR;
    void (*jump)(void) = (void (*)(void))*(__IO uint32_t *)(BOOTLOADER_BASE_ADDR + 4U);

    HAL_CAN_DeactivateNotification(&hcan2, CAN_IT_RX_FIFO0_MSG_PENDING | CAN_IT_RX_FIFO1_MSG_PENDING);
    HAL_CAN_Stop(&hcan2);
    HAL_CAN_DeInit(&hcan2);
    HAL_DeInit();
    HAL_RCC_DeInit();                     // HAL de-init needs SysTick: do it first

    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    for (uint32_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();   // system flash at 0x00000000
    SCB->VTOR = 0x00000000U;
    __set_CONTROL(0x00U);
    __ISB();
    __set_MSP(blStack);
    __DSB();
    __ISB();
    jump();
    while (1) {}
}
```

Key points:

1. Deactivate CAN notifications, stop and de-init CAN2, then `HAL_DeInit()` and `HAL_RCC_DeInit()` while SysTick still runs.
2. Disable interrupts, zero SysTick, clear all NVIC enable and pending registers.
3. Remap system flash to `0x00000000`, set `SCB->VTOR = 0`, switch to MSP.
4. Load SP from `0x1FFF0000` and jump to the address at `0x1FFF0004` (AN2606).
5. Call it from Thread Mode (main loop) using a flag set in the CAN callback, never from the IRQ.

See [support/gonzabrusco_jump_to_bootloader.md](../support/gonzabrusco_jump_to_bootloader.md) for returning to the application after the ROM bootloader's `Go`.

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

- [ ] CAN1 clock enabled whenever CAN2 is used (filters live in CAN1)
- [ ] CAN2 filter bank ≥ `SlaveStartFilterBank`, same value everywhere
- [ ] Notification/callback match the FIFO the filter feeds
- [ ] 120 Ω termination at both ends of the bus
- [ ] APB1 frequency confirmed before computing prescaler
- [ ] Bootloader entry (RAM flag + reset, or ROM jump) triggered from Thread Mode, not from the IRQ
- [ ] SLCAN adapter at the application bitrate before sending the jump frame
- [ ] LART: application linked at `0x08010200`, VTOR offset `0x10200`, last 8 SRAM bytes reserved
