#ifndef BOOT_UPDATE_H
#define BOOT_UPDATE_H
#include <stdint.h>
#include <stdio.h>
#include "bootloader.h" /* Generated from docs/bootloader.dbc by tools/generate_can.py. */

/* Increment for released bootloader changes; board type 2 is this STM32F412RE port. */
#define BOOT_VERSION_MAJOR    1U
#define BOOT_VERSION_MINOR    0U
#define BOOT_VERSION_PATCH    0U
#define BOOT_BOARD_TYPE       2U
#define BOOT_PROTOCOL_VERSION 1U

/* STM32F412RE flash (512 KiB, single bank, sectors 0-3 16 KiB, 4 64 KiB, 5-7 128 KiB):
 *   0x08000000 bootloader       64 KiB  sectors 0-3
 *   0x08010000 primary slot    192 KiB  sectors 4-5  (application executes here)
 *   0x08040000 staging slot    256 KiB  sectors 6-7  (CAN upload target)
 * Offsets are relative to BOOT_FLASH_BASE. */
#define BOOT_FLASH_BASE          0x08000000UL
#define BOOT_FLASH_SIZE          0x00080000UL
#define BOOT_PRIMARY_OFFSET      0x00010000UL
#define BOOT_PRIMARY_SIZE        0x00030000UL
#define BOOT_STAGING_OFFSET      0x00040000UL
#define BOOT_STAGING_SIZE        0x00040000UL
#define BOOT_STAGING_LAST_SECTOR 0x00020000UL /* Holds the commit record. */
#define BOOT_IMAGE_HEADER_SIZE   0x200U
#define BOOT_APP_VECTOR_ADDR     (BOOT_FLASH_BASE + BOOT_PRIMARY_OFFSET + BOOT_IMAGE_HEADER_SIZE)
#define BOOT_RAM_START           0x20000000UL
#define BOOT_RAM_SIZE            0x00040000UL
/* The installed copy must fit the primary slot; staging keeps its record past that. */
#define BOOT_UPLOAD_MAX_SIZE     BOOT_PRIMARY_SIZE
#define BOOT_UPLOAD_MIN_SIZE     (BOOT_IMAGE_HEADER_SIZE + 8U + 40U)
#define BOOT_RECORD_OFFSET       (BOOT_STAGING_OFFSET + BOOT_STAGING_SIZE - 16U)

#define BOOT_CAN_REQUEST_ID         BOOTLOADER_BOOTLOADER_REQUEST_FRAME_ID
#define BOOT_CAN_RESPONSE_ID        BOOTLOADER_BOOTLOADER_RESPONSE_FRAME_ID
#define BOOT_CAN_HEARTBEAT_ID       BOOTLOADER_BCI_BOOT_HEARTBEAT_FRAME_ID
#define BOOT_CAN_DIAGNOSTIC_BASE_ID BOOTLOADER_BCI_BOOT_INFO_FRAME_ID
enum { UPDATE_INFO, UPDATE_BEGIN, UPDATE_DATA, UPDATE_END, UPDATE_RESET };
enum { UPDATE_OK, UPDATE_BAD_FRAME, UPDATE_BAD_STATE, UPDATE_BAD_SIZE,
       UPDATE_BAD_OFFSET, UPDATE_FLASH_ERROR, UPDATE_BAD_IMAGE };
/* Diagnostic "banks": 1 = primary (application) slot, 2 = staging (upload) slot. */
enum { SLOT_NONE, SLOT_PRIMARY, SLOT_STAGING };
enum { DIAG_INFO, DIAG_VERSION, DIAG_DECISION, DIAG_UPLOAD,
       DIAG_ERROR, DIAG_ROLLBACK, DIAG_FLASH, DIAG_REJECTED_VERSION };
enum { UPLOAD_IDLE, UPLOAD_ERASING, UPLOAD_RECEIVING, UPLOAD_VALIDATING,
       UPLOAD_COMMITTED, UPLOAD_FAILED };
enum { ERROR_NONE, ERROR_HEADER, ERROR_TLV, ERROR_HASH, ERROR_VECTORS,
       ERROR_CRC, ERROR_FRAME, ERROR_STATE, ERROR_SIZE, ERROR_OFFSET,
       ERROR_FLASH_READ, ERROR_FLASH_WRITE, ERROR_FLASH_ERASE,
       ERROR_FLASH_UNLOCK, ERROR_FLASH_LOCK, ERROR_CAN_QUEUE,
       ERROR_CAN_TIMEOUT, ERROR_HANDOFF, ERROR_FLASH_CONFIG, ERROR_INACTIVITY,
       ERROR_RECORD, ERROR_FLASH_OPERATION };
enum { SOURCE_VALIDATION = 1, SOURCE_UPLOAD, SOURCE_FLASH, SOURCE_CAN, SOURCE_BOOT };
enum { ENTRY_RAM = 1, ENTRY_BUTTON = 2, ENTRY_RECOVERY = 4, ENTRY_ROLLBACK = 8 };

/* MCUboot image format (unsigned, SHA-256 TLV only). */
#define IMAGE_MAGIC          0x96f3b83dU
#define IMAGE_TLV_INFO_MAGIC 0x6907U
#define IMAGE_TLV_SHA256     0x10U
struct image_version { uint8_t iv_major, iv_minor; uint16_t iv_revision; uint32_t iv_build_num; };
struct image_header {
    uint32_t ih_magic, ih_load_addr;
    uint16_t ih_hdr_size, ih_protect_tlv_size;
    uint32_t ih_img_size, ih_flags;
    struct image_version ih_ver;
    uint32_t pad;
};
struct image_tlv_info { uint16_t it_magic, it_tlv_tot; };

/* Main-loop-owned snapshots sent on 0x582-0x589; repeated changes coalesce while CAN is busy. */
struct boot_diagnostics {
    uint16_t pending;
    uint8_t selected_bank, valid_banks, rejected_bank, reject_reason;
    uint8_t rejected_version_known, entry_reason, upload_bank, upload_stage;
    uint8_t error_code, error_bank, error_command, error_source;
    uint8_t current_command;
    struct image_version selected_version, rejected_version;
    uint32_t reset_flags, rejected_generation, received, total;
    uint32_t error_detail, flash_offset, flash_error, error_sequence;
};
extern struct boot_diagnostics boot_diagnostics;
void boot_diag_error(uint8_t code, uint8_t bank, uint8_t source, uint32_t detail);
void boot_diag_flash_error(uint8_t code, uint32_t offset, uint32_t hal_error);
void boot_diag_snapshot(void);
void boot_diag_flush(void); /* Bounded CAN drain before blocking phases (boot_port.c). */

/* 0 off, 1 errors, 2 warnings, 3 info. Output goes to printf (USART2 on target). */
#ifndef BOOT_LOG_LEVEL
#define BOOT_LOG_LEVEL 3
#endif
#define BOOT_LOG(level, tag, fmt, ...) \
    do { if (BOOT_LOG_LEVEL >= (level)) printf("[" tag "] " fmt "\r\n", ##__VA_ARGS__); } while (0)
#define BOOT_LOG_ERR(...) BOOT_LOG(1, "ERR", __VA_ARGS__)
#define BOOT_LOG_WRN(...) BOOT_LOG(2, "WRN", __VA_ARGS__)
#define BOOT_LOG_INF(...) BOOT_LOG(3, "INF", __VA_ARGS__)

/* Flash access, offsets relative to BOOT_FLASH_BASE; 0 on success (boot_flash.c). */
int boot_flash_read(uint32_t off, void *dst, uint32_t len);
int boot_flash_write(uint32_t off, const void *src, uint32_t len); /* Word-aligned. */
int boot_flash_erase(uint32_t off, uint32_t len);                  /* Whole sectors. */

/* Each call consumes one classic CAN data frame and creates an 8-byte reply. */
void boot_update_frame(const uint8_t *data, uint8_t len, uint8_t reply[8]);
/* Install a committed staged image if needed; 0 when the primary slot is bootable. */
int boot_select(struct image_header *hdr);
int boot_update_active(void);
int boot_image_vectors_valid(uint32_t sp, uint32_t pc, uint32_t image_size);
#endif
