#include "boot_update.h"
#include "mbedtls/sha256.h"
#include <string.h>

/* Final 16 bytes of the staging slot, each word programmed once:
 * BEGIN writes length, END writes ~length (commit), boot writes installed = 0. */
struct staging_record { uint32_t length, length_inv, installed, reserved; };
static uint32_t length, received, crc;
static uint8_t active, complete, last[8];

struct boot_diagnostics boot_diagnostics;

/* The board drains diagnostics between blocking update phases; tests need no CAN. */
__attribute__((weak)) void boot_diag_flush(void) {}

void boot_diag_error(uint8_t code, uint8_t bank, uint8_t source, uint32_t detail)
{
    ++boot_diagnostics.error_sequence;
    boot_diagnostics.error_code = code;
    boot_diagnostics.error_bank = bank;
    boot_diagnostics.error_command = boot_diagnostics.current_command;
    boot_diagnostics.error_source = source;
    boot_diagnostics.error_detail = detail;
    boot_diagnostics.pending |= 1U << DIAG_ERROR;
}

void boot_diag_flash_error(uint8_t code, uint32_t offset, uint32_t hal_error)
{
    boot_diag_error(code, offset < BOOT_STAGING_OFFSET ? SLOT_PRIMARY : SLOT_STAGING, SOURCE_FLASH, offset);
    boot_diagnostics.flash_offset = offset;
    boot_diagnostics.flash_error = hal_error;
    boot_diagnostics.pending |= 1U << DIAG_FLASH;
}

void boot_diag_snapshot(void)
{
    boot_diagnostics.pending |= (1U << DIAG_INFO) | (1U << DIAG_DECISION) | (1U << DIAG_UPLOAD);
    if (boot_diagnostics.selected_bank) boot_diagnostics.pending |= 1U << DIAG_VERSION;
    if (boot_diagnostics.error_code) boot_diagnostics.pending |= 1U << DIAG_ERROR;
    if (boot_diagnostics.error_source == SOURCE_FLASH) boot_diagnostics.pending |= 1U << DIAG_FLASH;
    if (boot_diagnostics.rejected_bank && boot_diagnostics.selected_bank) {
        boot_diagnostics.pending |= 1U << DIAG_ROLLBACK;
        if (boot_diagnostics.rejected_version_known)
            boot_diagnostics.pending |= 1U << DIAG_REJECTED_VERSION;
    }
}

static void upload_status(uint8_t stage)
{
    boot_diagnostics.upload_stage = stage;
    boot_diagnostics.upload_bank = SLOT_STAGING;
    boot_diagnostics.received = received;
    boot_diagnostics.total = length;
    boot_diagnostics.pending |= 1U << DIAG_UPLOAD;
    boot_diag_flush();
}

static const char *slot_name(uint32_t slot)
{
    return slot == BOOT_PRIMARY_OFFSET ? "primary" : "staging";
}

int boot_image_vectors_valid(uint32_t sp, uint32_t pc, uint32_t image_size)
{
    uint32_t start = BOOT_APP_VECTOR_ADDR;
    return sp > BOOT_RAM_START && sp <= BOOT_RAM_START + BOOT_RAM_SIZE && (sp & 7U) == 0 &&
           (pc & 1U) != 0 && (pc & ~1U) >= start && (pc & ~1U) - start < image_size;
}

/* Header, TLV bounds, vectors, then SHA-256 over header+payload against exactly one hash TLV. */
static int validate_image(uint32_t slot, uint32_t expected, struct image_header *hdr)
{
    struct image_tlv_info info;
    mbedtls_sha256_context sha;
    uint8_t buf[256], hash[32];
    uint32_t vectors[2], end, pos, tlv_end;
    unsigned found = 0;
    uint8_t bank = slot == BOOT_PRIMARY_OFFSET ? SLOT_PRIMARY : SLOT_STAGING;
    if (boot_flash_read(slot, hdr, sizeof *hdr) != 0) {
        boot_diag_error(ERROR_FLASH_READ, bank, SOURCE_VALIDATION, slot);
        return -1;
    }
    if (hdr->ih_magic != IMAGE_MAGIC || hdr->ih_hdr_size != BOOT_IMAGE_HEADER_SIZE ||
        hdr->ih_flags != 0 || hdr->ih_protect_tlv_size != 0 || hdr->ih_img_size < 8 ||
        hdr->ih_img_size > BOOT_UPLOAD_MAX_SIZE - BOOT_IMAGE_HEADER_SIZE - sizeof info) {
        boot_diag_error(ERROR_HEADER, bank, SOURCE_VALIDATION, slot);
        BOOT_LOG_WRN("%s image: invalid header/size", slot_name(slot));
        return -1;
    }
    end = BOOT_IMAGE_HEADER_SIZE + hdr->ih_img_size;
    if (boot_flash_read(slot + end, &info, sizeof info) != 0 ||
        boot_flash_read(slot + BOOT_IMAGE_HEADER_SIZE, vectors, sizeof vectors) != 0) {
        boot_diag_error(ERROR_FLASH_READ, bank, SOURCE_VALIDATION, slot + end);
        return -1;
    }
    if (info.it_magic != IMAGE_TLV_INFO_MAGIC || info.it_tlv_tot < sizeof info ||
        info.it_tlv_tot > BOOT_UPLOAD_MAX_SIZE - end ||
        (expected && end + info.it_tlv_tot != expected)) {
        boot_diag_error(ERROR_TLV, bank, SOURCE_VALIDATION, slot + end);
        BOOT_LOG_WRN("%s image: invalid TLV/length", slot_name(slot));
        return -1;
    }
    if (!boot_image_vectors_valid(vectors[0], vectors[1], hdr->ih_img_size)) {
        boot_diag_error(ERROR_VECTORS, bank, SOURCE_VALIDATION, vectors[1]);
        BOOT_LOG_WRN("%s image: invalid vectors SP=0x%08lx PC=0x%08lx", slot_name(slot),
                     (unsigned long)vectors[0], (unsigned long)vectors[1]);
        return -1;
    }
    mbedtls_sha256_init(&sha);
    int rc = mbedtls_sha256_starts_ret(&sha, 0);
    for (pos = 0; rc == 0 && pos < end; pos += sizeof buf) {
        uint32_t n = end - pos < sizeof buf ? end - pos : sizeof buf;
        rc = boot_flash_read(slot + pos, buf, n) != 0 || mbedtls_sha256_update_ret(&sha, buf, n) != 0;
    }
    if (rc == 0) rc = mbedtls_sha256_finish_ret(&sha, hash);
    mbedtls_sha256_free(&sha);
    tlv_end = end + info.it_tlv_tot;
    for (pos = end + sizeof info; rc == 0 && pos < tlv_end;) {
        uint16_t tlv[2]; /* type, length */
        if (tlv_end - pos < sizeof tlv || boot_flash_read(slot + pos, tlv, sizeof tlv) != 0 ||
            tlv[1] > tlv_end - pos - sizeof tlv) {
            rc = -1;
            break;
        }
        pos += sizeof tlv;
        if (tlv[0] == IMAGE_TLV_SHA256) {
            ++found;
            if (tlv[1] != sizeof hash || boot_flash_read(slot + pos, buf, sizeof hash) != 0 ||
                memcmp(buf, hash, sizeof hash) != 0)
                rc = -1;
        }
        pos += tlv[1];
    }
    if (rc != 0 || found != 1) {
        boot_diag_error(ERROR_HASH, bank, SOURCE_VALIDATION, slot);
        BOOT_LOG_ERR("%s image: SHA-256 validation failed", slot_name(slot));
        return -1;
    }
    return 0;
}

/* Erase the primary slot and copy the staged image into it. A reset midway leaves the
 * staged copy intact, so the next boot simply repeats the installation. */
static int install(uint32_t len)
{
    uint8_t buf[256];
    BOOT_LOG_INF("Installing staged image: %lu bytes", (unsigned long)len);
    if (boot_flash_erase(BOOT_PRIMARY_OFFSET, BOOT_PRIMARY_SIZE) != 0) return -1;
    for (uint32_t pos = 0; pos < len; pos += sizeof buf) {
        uint32_t n = len - pos < sizeof buf ? len - pos : sizeof buf;
        memset(buf, 0xff, sizeof buf);
        if (boot_flash_read(BOOT_STAGING_OFFSET + pos, buf, n) != 0 ||
            boot_flash_write(BOOT_PRIMARY_OFFSET + pos, buf, (n + 3U) & ~3U) != 0)
            return -1;
    }
    return 0;
}

/* shortcut: integrity-only; a hash-valid app that hangs needs a watchdog/health-confirmation policy. */
int boot_select(struct image_header *hdr)
{
    struct staging_record rec;
    struct image_header staged_hdr = {0};
    uint32_t reset_flags = boot_diagnostics.reset_flags;
    memset(&boot_diagnostics, 0, sizeof boot_diagnostics);
    boot_diagnostics.reset_flags = reset_flags;
    boot_diagnostics.current_command = 0xff;
    active = complete = 0;
    received = length = 0;
    int read_ok = boot_flash_read(BOOT_RECORD_OFFSET, &rec, sizeof rec) == 0;
    int committed = read_ok && rec.length >= BOOT_UPLOAD_MIN_SIZE && rec.length <= BOOT_UPLOAD_MAX_SIZE &&
                    rec.length_inv == ~rec.length;
    if (!read_ok)
        boot_diag_error(ERROR_FLASH_READ, SLOT_STAGING, SOURCE_VALIDATION, BOOT_RECORD_OFFSET);
    else if (!committed && rec.length != UINT32_MAX)
        boot_diag_error(ERROR_RECORD, SLOT_STAGING, SOURCE_BOOT, BOOT_RECORD_OFFSET);
    int ok = validate_image(BOOT_PRIMARY_OFFSET, 0, hdr) == 0;
    int staged_ok = committed && validate_image(BOOT_STAGING_OFFSET, rec.length, &staged_hdr) == 0;
    if (staged_ok) boot_diagnostics.valid_banks |= 1U << 1;
    /* Install a new upload once; reinstall it whenever the primary copy is damaged. */
    if (committed && (!ok || rec.installed == UINT32_MAX)) {
        if (!staged_ok) {
            BOOT_LOG_WRN("Staged image damaged; %s", ok ? "keeping primary image" : "nothing to install");
            if (ok) { /* Newer upload rejected, older application kept: report as rollback. */
                boot_diagnostics.rejected_bank = SLOT_STAGING;
                boot_diagnostics.reject_reason = boot_diagnostics.error_code;
                boot_diagnostics.rejected_version_known = staged_hdr.ih_magic == IMAGE_MAGIC;
                boot_diagnostics.rejected_version = staged_hdr.ih_ver;
            }
        } else if (install(rec.length) != 0 || validate_image(BOOT_PRIMARY_OFFSET, rec.length, hdr) != 0) {
            BOOT_LOG_ERR("Install failed; staged image kept for the next reset");
            ok = 0;
        } else {
            uint32_t installed = 0;
            ok = 1;
            if (rec.installed == UINT32_MAX &&
                boot_flash_write(BOOT_RECORD_OFFSET + 8U, &installed, sizeof installed) != 0)
                BOOT_LOG_WRN("Install mark not written; image will be copied again at next reset");
            BOOT_LOG_INF("Staged image installed");
        }
    }
    if (ok) {
        boot_diagnostics.valid_banks |= 1U << 0;
        boot_diagnostics.selected_bank = SLOT_PRIMARY;
        boot_diagnostics.selected_version = hdr->ih_ver;
    }
    upload_status(UPLOAD_IDLE);
    boot_diag_snapshot();
    if (!ok) {
        BOOT_LOG_ERR("No valid application; CAN recovery mode");
        return -1;
    }
    BOOT_LOG_INF("Primary image valid: version %u.%u.%u+%lu, %lu bytes", hdr->ih_ver.iv_major,
                 hdr->ih_ver.iv_minor, hdr->ih_ver.iv_revision, (unsigned long)hdr->ih_ver.iv_build_num,
                 (unsigned long)hdr->ih_img_size);
    return 0;
}

int boot_update_active(void) { return active || complete; }

void boot_update_frame(const uint8_t *data, uint8_t len, uint8_t reply[8])
{
    uint8_t status = UPDATE_OK;
    uint8_t cmd = len ? data[0] : 0x7f;
    uint32_t n, off, value = 0;
    struct bootloader_bootloader_request_t request = {0};
    boot_diagnostics.current_command = cmd;
    uint32_t previous_error_sequence = boot_diagnostics.error_sequence;
    memset(reply, 0, 8);
    if (len != 8 || bootloader_bootloader_request_unpack(&request, data, len) != 0) {
        status = UPDATE_BAD_FRAME; goto respond;
    }
    cmd = request.command;
    switch (cmd) {
    case UPDATE_INFO:
        BOOT_LOG_INF("CAN INFO: max upload %lu bytes", (unsigned long)BOOT_UPLOAD_MAX_SIZE);
        boot_diag_snapshot();
        value = BOOT_UPLOAD_MAX_SIZE;
        break;
    case UPDATE_BEGIN:
        if (complete) { status = UPDATE_BAD_STATE; break; }
        n = request.image_length;
        if (n < BOOT_UPLOAD_MIN_SIZE || n > BOOT_UPLOAD_MAX_SIZE) {
            status = UPDATE_BAD_SIZE; break;
        }
        if (active && received == 0 && n == length) break;
        BOOT_LOG_INF("Upload BEGIN: %lu bytes; erasing staging slot", (unsigned long)n);
        active = complete = 0;
        received = 0;
        length = n;
        upload_status(UPLOAD_ERASING);
        /* Invalidate the old record (last sector) before touching its payload. */
        if (boot_flash_erase(BOOT_STAGING_OFFSET + BOOT_STAGING_SIZE - BOOT_STAGING_LAST_SECTOR,
                             BOOT_STAGING_LAST_SECTOR) != 0 ||
            boot_flash_erase(BOOT_STAGING_OFFSET, BOOT_STAGING_SIZE - BOOT_STAGING_LAST_SECTOR) != 0 ||
            boot_flash_write(BOOT_RECORD_OFFSET, &n, sizeof n) != 0) {
            status = UPDATE_FLASH_ERROR; break;
        }
        crc = 0xffffffffU;
        active = 1;
        upload_status(UPLOAD_RECEIVING);
        BOOT_LOG_INF("Upload ready");
        break;
    case UPDATE_DATA:
        if (!active) { status = UPDATE_BAD_STATE; break; }
        off = request.image_offset;
        /* A lost ACK must never program the same flash word twice. */
        if (received && memcmp(last, data, 8) == 0) break;
        if (off != received || received == length) { status = UPDATE_BAD_OFFSET; break; }
        n = length - received;
        if (n > 4) n = 4;
        const uint8_t chunk[] = {request.payload_byte0, request.payload_byte1,
                                 request.payload_byte2, request.payload_byte3};
        uint8_t word[4] = {0xff, 0xff, 0xff, 0xff};
        memcpy(word, chunk, n);
        for (uint32_t i = 0; i < n; ++i) {
            crc ^= chunk[i];
            for (unsigned j = 0; j < 8; ++j)
                crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xedb88320U);
        }
        if (boot_flash_write(BOOT_STAGING_OFFSET + off, word, sizeof word) != 0) {
            active = 0; status = UPDATE_FLASH_ERROR; break;
        }
        received += n;
        memcpy(last, data, 8);
        boot_diagnostics.received = received;
        if (received % 16384U == 0 || received == length) {
            upload_status(UPLOAD_RECEIVING);
            BOOT_LOG_INF("Upload progress: %lu/%lu bytes", (unsigned long)received, (unsigned long)length);
        }
        break;
    case UPDATE_END:
        if (complete && request.image_crc32 == (crc ^ 0xffffffffU)) break;
        if (!active || received != length) { status = UPDATE_BAD_STATE; break; }
        upload_status(UPLOAD_VALIDATING);
        if (request.image_crc32 != (crc ^ 0xffffffffU)) {
            boot_diag_error(ERROR_CRC, SLOT_STAGING, SOURCE_UPLOAD, crc ^ 0xffffffffU);
            BOOT_LOG_WRN("Upload CRC mismatch: host 0x%08lx, device 0x%08lx",
                         (unsigned long)request.image_crc32, (unsigned long)(crc ^ 0xffffffffU));
            status = UPDATE_BAD_IMAGE; break;
        }
        struct image_header hdr;
        if (validate_image(BOOT_STAGING_OFFSET, length, &hdr) != 0) { status = UPDATE_BAD_IMAGE; break; }
        uint32_t commit = ~length;
        if (boot_flash_write(BOOT_RECORD_OFFSET + 4U, &commit, sizeof commit) != 0) {
            active = 0; status = UPDATE_FLASH_ERROR; break;
        }
        active = 0;
        complete = 1;
        upload_status(UPLOAD_COMMITTED);
        BOOT_LOG_INF("Upload committed; reset to install");
        break;
    case UPDATE_RESET:
        break;
    default:
        status = UPDATE_BAD_FRAME;
        break;
    }
respond:
    if (status != UPDATE_OK) {
        if (boot_diagnostics.error_sequence == previous_error_sequence) {
            const uint8_t codes[] = {ERROR_NONE, ERROR_FRAME, ERROR_STATE, ERROR_SIZE,
                                     ERROR_OFFSET, ERROR_FLASH_OPERATION, ERROR_HASH};
            uint32_t detail = received;
            if (status == UPDATE_BAD_FRAME) detail = len;
            else if (status == UPDATE_BAD_SIZE) detail = request.image_length;
            else if (status == UPDATE_BAD_OFFSET) detail = request.image_offset;
            boot_diag_error(codes[status], SLOT_STAGING, SOURCE_UPLOAD, detail);
        }
        upload_status(status == UPDATE_BAD_IMAGE || status == UPDATE_FLASH_ERROR || (!active && !complete)
                      ? UPLOAD_FAILED : complete ? UPLOAD_COMMITTED : UPLOAD_RECEIVING);
        BOOT_LOG_WRN("CAN command 0x%02x rejected: status %u, received %lu/%lu, DLC %u",
                     cmd, status, (unsigned long)received, (unsigned long)length, len);
    }
    if (cmd != UPDATE_INFO) value = received;
    const struct bootloader_bootloader_response_t response = {
        cmd | 0x80U, status, value, BOOT_PROTOCOL_VERSION, complete ? 2 : active
    };
    bootloader_bootloader_response_pack(reply, &response, 8);
}
