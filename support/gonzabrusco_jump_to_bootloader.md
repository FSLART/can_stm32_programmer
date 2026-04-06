# How to Jump to the STM32 Bootloader and Return to the Application

**Source:** [gonzabrusco/stm32bootloader.md (GitHub Gist)](https://gist.github.com/gonzabrusco/fd47e89e4c6fb302fc54b83637a3a101)  
**Tested on:** STM32G070CB (applicable with minor adjustments to most STM32 families)

---

## Entering the Bootloader

The guide provides a `JumpToBootloader()` function that performs the standard sequence: HAL de-init → IRQ disable → SysTick disable → NVIC clear → memory remap → VTOR → MSP → jump.

Full implementation and community discussion available at the gist link above.

Key community notes from the gist discussion thread:

- HAL and RCC de-init should happen **before** disabling SysTick, since `HAL_RCC_DeInit()` uses the HAL tick.
- Setting `SCB->VTOR = 0` is recommended since the application may have moved it.
- `__ISB()` after `__set_CONTROL(0)` is required by the Cortex-M4 programming manual.
- An `while(1){}` loop after the jump call is essential at medium/high optimisation levels to prevent the compiler from popping the stack before the jump executes.
- Jumping to `0x1FFF0000` (physical bootloader address) is more reliable than jumping to the remapped `0x00000000` on some devices.

---

## Returning to the Application After Programming

A common problem: after the bootloader completes programming and the host sends the `Go` command (jump to `0x08000000`), the MCU may hang because the bootloader leaves `SYSCFG->MEM_MODE = 1` (system flash at `0x00000000`) and `SCB->VTOR = 0x1FFF0000` pointing at the bootloader's vector table.

### Solution A — Fake Vector Table (gonzabrusco's approach)

Create a secondary vector table in flash at a fixed address (e.g. `0x08000200`) whose reset handler simply calls `NVIC_SystemReset()`. Tell the bootloader to `Go` to that address instead of `0x08000000`. The bootloader sets up MSP correctly from that table and jumps to `Force_Reset_Handler()`, which triggers a clean reset, allowing the application to start normally.

Add to linker script (after `isr_vector`):

```ld
.secondary_isr_vector 0x08000200 (READONLY):
{
    . = ALIGN(4);
    KEEP(*(.force_reset_vtable))
    . = ALIGN(4);
} >FLASH
```

Add to firmware:

```c
#include "stm32f4xx.h"

void Force_Reset_Handler(void) {
    while(1) { NVIC_SystemReset(); }
}

extern uint32_t _estack;

void* __attribute__((section(".force_reset_vtable")))
force_reset_vtable[] = {
    [0] = (void*)&_estack,
    [1] = Force_Reset_Handler
};
```

Then send `Go` to `0x08000200` instead of `0x08000000`.

### Solution B — RunAfterProgramming() in main()

An alternative suggested in the gist comments: add a `RunAfterProgramming()` call at the top of `main()` that restores the correct memory map and VTOR before anything else runs:

```c
void RunAfterProgramming(void)
{
    __disable_irq();
    HAL_DeInit();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    for (uint32_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }

    // Remap main flash back to 0x00000000
    __HAL_SYSCFG_REMAPMEMORY_FLASH();

    // Restore VTOR to application vector table
    SCB->VTOR = 0x08000000U;

    __enable_irq();
}
```

Call at the very start of `main()`, before `HAL_Init()`:

```c
int main(void) {
    /* USER CODE BEGIN 1 */
    RunAfterProgramming();
    /* USER CODE END 1 */

    HAL_Init();
    // ...
}
```

This approach works well and requires no changes to the linker script. The tradeoff is that it runs every time the application starts, not just after a bootloader session — which is harmless but slightly less elegant.

---

## Recommendation for This Project

The example firmware in this repository uses `Go` to `0x08000000` and relies on the Python tool sending the `Go` command at the end of programming. If the MCU hangs after programming, implement **Solution A** (fake vector table at `0x08000200`) and configure the Python tool's `go_addr` variable to `0x08000200`.

Alternatively, add **Solution B** to your application firmware as a clean way to always ensure correct state at startup.
