#include "boot_port.h"
#include "boot_update.h"
#include <string.h>

/* STM32F412RE sector start offsets: 4 x 16 KiB, 64 KiB, 3 x 128 KiB, then the end of flash. */
static const uint32_t sectors[] = {0x00000, 0x04000, 0x08000, 0x0C000, 0x10000,
                                   0x20000, 0x40000, 0x60000, BOOT_FLASH_SIZE};
static int ready;

int boot_flash_init(void)
{
    uint32_t size_kib = *(const uint16_t *)FLASHSIZE_BASE;
    /* Both slots live in sectors 4-7; nWRP=0 would make every erase fail. */
    uint32_t unprotected = FLASH_OPTCR_nWRP_4 | FLASH_OPTCR_nWRP_5 | FLASH_OPTCR_nWRP_6 | FLASH_OPTCR_nWRP_7;
    /* Programming does not update the ART data cache: keep it off so readback sees flash.
     * Done here, not in stm32f4xx_hal_conf.h, because CubeMX regenerates that file. */
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_RESET();
    ready = size_kib >= BOOT_FLASH_SIZE / 1024U && (FLASH->OPTCR & unprotected) == unprotected;
    if (!ready) {
        boot_diag_error(ERROR_FLASH_CONFIG, SLOT_NONE, SOURCE_BOOT, FLASH->OPTCR);
        BOOT_LOG_ERR("Flash requires >= 512 KiB and sectors 4-7 unprotected: size=%lu KiB OPTCR=0x%08lx",
                     (unsigned long)size_kib, (unsigned long)FLASH->OPTCR);
    } else
        BOOT_LOG_INF("Flash ready: %lu KiB, primary 0x%08lx, staging 0x%08lx",
                     (unsigned long)size_kib, (unsigned long)(BOOT_FLASH_BASE + BOOT_PRIMARY_OFFSET),
                     (unsigned long)(BOOT_FLASH_BASE + BOOT_STAGING_OFFSET));
    return ready ? 0 : -1;
}

/* The bootloader sectors are never writable. */
static int writable(uint32_t off, uint32_t len)
{
    return ready && off >= BOOT_PRIMARY_OFFSET && off <= BOOT_FLASH_SIZE && len <= BOOT_FLASH_SIZE - off;
}

int boot_flash_read(uint32_t off, void *dst, uint32_t len)
{
    if (!ready || !dst || off > BOOT_FLASH_SIZE || len > BOOT_FLASH_SIZE - off) {
        boot_diag_flash_error(ERROR_FLASH_READ, off, HAL_FLASH_GetError());
        return -1;
    }
    memcpy(dst, (const void *)(BOOT_FLASH_BASE + off), len);
    return 0;
}

int boot_flash_write(uint32_t off, const void *src, uint32_t len)
{
    if (!src || !writable(off, len) || (off & 3U) || (len & 3U)) {
        boot_diag_flash_error(ERROR_FLASH_WRITE, off, HAL_FLASH_GetError());
        return -1;
    }
    if (HAL_FLASH_Unlock() != HAL_OK) {
        boot_diag_flash_error(ERROR_FLASH_UNLOCK, off, HAL_FLASH_GetError());
        BOOT_LOG_ERR("Flash unlock failed: error 0x%08lx", (unsigned long)HAL_FLASH_GetError());
        return -1;
    }
    int rc = 0;
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t data, addr = BOOT_FLASH_BASE + off + i;
        memcpy(&data, (const uint8_t *)src + i, 4);
        /* Never reprogram a word; all-ones data only needs an erased target. */
        if (*(const volatile uint32_t *)addr != UINT32_MAX ||
            (data != UINT32_MAX &&
             (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data) != HAL_OK ||
              *(const volatile uint32_t *)addr != data))) {
            boot_diag_flash_error(ERROR_FLASH_WRITE, off + i, HAL_FLASH_GetError());
            BOOT_LOG_ERR("Flash program/readback failed: offset 0x%08lx, error 0x%08lx",
                         (unsigned long)(off + i), (unsigned long)HAL_FLASH_GetError());
            rc = -1;
            break;
        }
    }
    if (HAL_FLASH_Lock() != HAL_OK) {
        boot_diag_flash_error(ERROR_FLASH_LOCK, off, HAL_FLASH_GetError());
        BOOT_LOG_ERR("Flash lock failed: error 0x%08lx", (unsigned long)HAL_FLASH_GetError());
        rc = -1;
    }
    return rc;
}

int boot_flash_erase(uint32_t off, uint32_t len)
{
    unsigned first = 0, last;
    while (first < 8 && sectors[first] < off) ++first;
    for (last = first; last < 8 && sectors[last] < off + len; ++last) {}
    if (!writable(off, len) || len == 0 || first >= 8 || sectors[first] != off || sectors[last] != off + len) {
        boot_diag_flash_error(ERROR_FLASH_ERASE, off, HAL_FLASH_GetError());
        return -1;
    }
    if (HAL_FLASH_Unlock() != HAL_OK) {
        boot_diag_flash_error(ERROR_FLASH_UNLOCK, off, HAL_FLASH_GetError());
        BOOT_LOG_ERR("Flash unlock failed: error 0x%08lx", (unsigned long)HAL_FLASH_GetError());
        return -1;
    }
    int rc = 0;
    for (unsigned s = first; s < last && rc == 0; ++s) {
        FLASH_EraseInitTypeDef e = {0};
        uint32_t error = UINT32_MAX;
        e.TypeErase = FLASH_TYPEERASE_SECTORS;
        e.Sector = s;
        e.NbSectors = 1;
        e.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* 2.7-3.6 V, 32-bit parallelism. */
        if (HAL_FLASHEx_Erase(&e, &error) != HAL_OK) {
            boot_diag_flash_error(ERROR_FLASH_ERASE, sectors[s], HAL_FLASH_GetError());
            BOOT_LOG_ERR("Flash erase failed: sector %u, HAL error 0x%08lx", s,
                         (unsigned long)HAL_FLASH_GetError());
            rc = -1;
        }
    }
    if (HAL_FLASH_Lock() != HAL_OK) {
        boot_diag_flash_error(ERROR_FLASH_LOCK, off, HAL_FLASH_GetError());
        BOOT_LOG_ERR("Flash lock failed: error 0x%08lx", (unsigned long)HAL_FLASH_GetError());
        rc = -1;
    }
    return rc;
}
