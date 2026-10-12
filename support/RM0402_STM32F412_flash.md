# RM0402 — STM32F412 Flash Memory Organisation

**Document number:** RM0402  
**Title:** STM32F412 advanced Arm®-based 32-bit MCUs — Reference manual  
**Publisher:** STMicroelectronics  
**Link:** [https://www.st.com/resource/en/reference_manual/rm0402-stm32f412-advanced-armbased-32bit-mcus-stmicroelectronics.pdf](https://www.st.com/resource/en/reference_manual/rm0402-stm32f412-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)

---

## Summary

RM0402 §3 (Embedded flash memory interface) defines how the STM32F412 flash is organised, erased and programmed. These are the facts the LART bootloader in [`../firmware/`](../firmware/) depends on.

---

## Sector Layout (single bank)

| Sector | Address | Size | LART use |
|---|---|---|---|
| 0–3 | `0x08000000`–`0x0800FFFF` | 4 × 16 KiB | Bootloader |
| 4 | `0x08010000`–`0x0801FFFF` | 64 KiB | Application slot |
| 5 | `0x08020000`–`0x0803FFFF` | 128 KiB | Application slot |
| 6 | `0x08040000`–`0x0805FFFF` | 128 KiB | Upload (staging) slot |
| 7 | `0x08060000`–`0x0807FFFF` | 128 KiB | Upload slot + commit record (last 16 bytes) |
| 8–11 | `0x08080000`–`0x080FFFFF` | 4 × 128 KiB | Only on 1 MiB parts (F412xG); unused |

The F412 has **one bank** and no bank-swap/remap bit, unlike the STM32L476 (`SYSCFG_MEMRMP.FB_MODE`). Code runs only at the address it was linked for, which is why the LART bootloader copies an uploaded image into the application slot instead of running it in place.

---

## Erase and Program

| Topic | Rule |
|---|---|
| Erase unit | Whole sector (`FLASH_CR.SER` + `SNB`), or mass erase |
| Program parallelism | `PSIZE` x8/x16/x32/x64; **x32 (word) for VDD 2.7–3.6 V** (`FLASH_VOLTAGE_RANGE_3`) |
| Erased value | `0xFF` |
| ECC | None (unlike the L4); the bootloader still never programs a word twice |
| Typical times (datasheet) | 128 KiB sector erase ≈ 1–2 s, word program ≈ 16 µs |
| Write protection | `FLASH_OPTCR.nWRPi = 0` protects sector *i*; the bootloader requires nWRP4–7 = 1 |
| Flash size register | 16-bit value in KiB at `0x1FFF7A22` |

---

## ART Accelerator

The flash interface has an instruction cache, a data cache and prefetch (`FLASH_ACR.ICEN/DCEN/PRFTEN`). The data cache is not updated by programming, so `boot_flash_init()` disables and resets it (in code, because CubeMX regenerates `stm32f4xx_hal_conf.h`) to make flash readback reliable, and resets both caches (`ICRST/DCRST`) before jumping to the application. `HAL_FLASHEx_Erase()` flushes the caches after each erase.

---

## Related

- [STM32F412RE datasheet](https://www.st.com/resource/en/datasheet/stm32f412re.pdf) — electrical characteristics, erase/program timings
- [`../firmware/docs/PORTING.md`](../firmware/docs/PORTING.md) — how the layout maps to code
