#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "boot_update.h"
#include "mbedtls/sha256.h"

#define PRIMARY (memory + BOOT_PRIMARY_OFFSET)
#define STAGING (memory + BOOT_STAGING_OFFSET)
#define RECORD  (memory + BOOT_RECORD_OFFSET)
static const uint32_t sectors[] = {0x00000, 0x04000, 0x08000, 0x0C000, 0x10000,
                                   0x20000, 0x40000, 0x60000, BOOT_FLASH_SIZE};
static uint8_t memory[BOOT_FLASH_SIZE], image[569], reply[8];
static int writes, erases, fail_write, fail_erase, fail_read, reset_after;
static jmp_buf reset;
static struct image_header hdr;

/* Modeled STM32F412 flash: sector erase, single programming of erased words. */
int boot_flash_read(uint32_t off, void *dst, uint32_t len)
{
    assert(off <= sizeof memory && len <= sizeof memory - off);
    if (fail_read) return -1;
    memcpy(dst, memory + off, len);
    return 0;
}
int boot_flash_write(uint32_t off, const void *src, uint32_t len)
{
    assert(off >= BOOT_PRIMARY_OFFSET && off <= sizeof memory && len <= sizeof memory - off);
    assert(off % 4 == 0 && len % 4 == 0);
    if (fail_write) return -1;
    for (uint32_t i = 0; i < len; i += 4) {
        if (reset_after && ++writes == reset_after) {
            memory[off + i] = ((const uint8_t *)src)[i]; /* Power lost mid-word. */
            longjmp(reset, 1);
        }
        static const uint8_t erased[4] = {0xff, 0xff, 0xff, 0xff};
        assert(memcmp(memory + off + i, erased, 4) == 0); /* Never program a word twice. */
        memcpy(memory + off + i, (const uint8_t *)src + i, 4);
    }
    return 0;
}
int boot_flash_erase(uint32_t off, uint32_t len)
{
    unsigned s = 0;
    while (sectors[s] < off) ++s;
    assert(off >= BOOT_PRIMARY_OFFSET && sectors[s] == off && len > 0);
    if (fail_erase) return -1;
    for (; sectors[s] < off + len; ++s) ++erases;
    assert(sectors[s] == off + len);
    memset(memory + off, 0xff, len);
    return 0;
}

static void command(uint8_t cmd, uint32_t value, uint8_t expected)
{
    uint8_t frame[8] = {cmd};
    memcpy(frame + 1, &value, 4);
    boot_update_frame(frame, 8, reply);
    if (reply[1] != expected)
        printf("command %u value %lu: status %u expected %u\n", cmd, (unsigned long)value, reply[1], expected);
    assert(reply[0] == (cmd | 0x80) && reply[1] == expected && reply[6] == 1);
}
static uint32_t crc32(void)
{
    uint32_t c = 0xffffffffU;
    for (unsigned i = 0; i < sizeof image; ++i) {
        c ^= image[i];
        for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ ((0U - (c & 1U)) & 0xedb88320U);
    }
    return ~c;
}
/* 512-byte header, 17-byte payload starting with vectors, 40-byte SHA-256 TLV block. */
static void make_image(uint8_t marker)
{
    struct image_header h = {.ih_magic = IMAGE_MAGIC, .ih_hdr_size = 512, .ih_img_size = 17, .ih_ver = {1, 2, 3, 4}};
    uint32_t v[] = {0x20040000, 0x08010209};
    memset(image, 0, sizeof image);
    memcpy(image, &h, sizeof h);
    memcpy(image + 512, v, sizeof v);
    image[528] = marker;
    struct image_tlv_info t = {IMAGE_TLV_INFO_MAGIC, 40};
    memcpy(image + 529, &t, sizeof t);
    image[533] = IMAGE_TLV_SHA256;
    image[535] = 32;
    assert(mbedtls_sha256_ret(image, 529, image + 537, 0) == 0);
}
static void upload(void)
{
    command(UPDATE_BEGIN, sizeof image, UPDATE_OK);
    for (uint32_t off = 0; off < sizeof image; off += 4) {
        uint8_t f[8] = {UPDATE_DATA, off, off >> 8, off >> 16, 0xff, 0xff, 0xff, 0xff};
        uint32_t n = sizeof image - off;
        if (n > 4) n = 4;
        memcpy(f + 4, image + off, n);
        boot_update_frame(f, 8, reply);
        assert(reply[1] == UPDATE_OK);
        boot_update_frame(f, 8, reply); /* Lost ACK: identical retry must not write again. */
        assert(reply[1] == UPDATE_OK);
    }
}
static void upload_commit(void)
{
    upload();
    command(UPDATE_END, crc32(), UPDATE_OK);
}
static int select_with_reset_after(int words)
{
    writes = 0;
    reset_after = words;
    int rc = setjmp(reset) == 0 ? boot_select(&hdr) : -2;
    reset_after = 0;
    return rc;
}

int main(void)
{
    memset(memory, 0xff, sizeof memory);
    make_image(0xA1);
    assert(boot_select(&hdr) != 0); /* Blank: CAN recovery. */
    command(UPDATE_INFO, 0, UPDATE_OK);
    assert(reply[7] == 0 && reply[2] == 0 && reply[3] == 0 && reply[4] == 3); /* 0x30000 */
    command(UPDATE_DATA, 0, UPDATE_BAD_STATE);
    command(UPDATE_BEGIN, 0xffffffffU, UPDATE_BAD_SIZE);
    command(UPDATE_BEGIN, BOOT_UPLOAD_MAX_SIZE + 1U, UPDATE_BAD_SIZE);
    command(UPDATE_BEGIN, BOOT_UPLOAD_MIN_SIZE - 1U, UPDATE_BAD_SIZE);
    uint8_t short_frame[8] = {UPDATE_BEGIN};
    boot_update_frame(short_frame, 5, reply);
    assert(reply[1] == UPDATE_BAD_FRAME && erases == 0);
    command(UPDATE_BEGIN, sizeof image, UPDATE_OK);
    assert(erases == 2); /* Both staging sectors; primary untouched. */
    assert(boot_diagnostics.upload_stage == UPLOAD_RECEIVING && boot_diagnostics.total == sizeof image);
    assert(boot_diagnostics.upload_bank == SLOT_STAGING && boot_diagnostics.received == 0);
    command(UPDATE_BEGIN, sizeof image, UPDATE_OK);
    assert(erases == 2); /* Repeated BEGIN before DATA does not erase again. */
    command(UPDATE_DATA, 4, UPDATE_BAD_OFFSET);
    command(UPDATE_END, crc32(), UPDATE_BAD_STATE);

    /* First upload: CRC, SHA and commit; installation happens at the next boot. */
    upload();
    assert(memcmp(STAGING, image, sizeof image) == 0);
    command(UPDATE_END, crc32() ^ 1, UPDATE_BAD_IMAGE);
    assert(boot_diagnostics.error_code == ERROR_CRC && boot_diagnostics.error_source == SOURCE_UPLOAD);
    assert(boot_diagnostics.error_command == UPDATE_END && boot_diagnostics.error_detail == crc32());
    command(UPDATE_INFO, 0, UPDATE_OK);
    assert(boot_diagnostics.error_code == ERROR_CRC && (boot_diagnostics.pending & (1U << DIAG_ERROR)));
    STAGING[528] ^= 1;
    command(UPDATE_END, crc32(), UPDATE_BAD_IMAGE);
    STAGING[528] ^= 1;
    command(UPDATE_END, crc32(), UPDATE_OK);
    command(UPDATE_END, crc32(), UPDATE_OK); /* Lost END ACK retry. */
    assert(reply[7] == 2);
    assert(boot_diagnostics.upload_stage == UPLOAD_COMMITTED && boot_diagnostics.received == sizeof image);
    command(UPDATE_BEGIN, sizeof image, UPDATE_BAD_STATE);
    assert(PRIMARY[0] == 0xff);
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, image, sizeof image) == 0 && hdr.ih_ver.iv_major == 1);
    assert(boot_diagnostics.selected_bank == SLOT_PRIMARY && boot_diagnostics.valid_banks == 3);
    assert(boot_diagnostics.selected_version.iv_major == 1 && boot_diagnostics.selected_version.iv_minor == 2);
    assert(boot_diagnostics.selected_version.iv_revision == 3 && boot_diagnostics.selected_version.iv_build_num == 4);
    assert(boot_diagnostics.rejected_bank == 0 && (boot_diagnostics.pending & (1U << DIAG_VERSION)));
    assert(RECORD[8] == 0); /* Marked installed. */
    erases = 0;
    assert(boot_select(&hdr) == 0 && erases == 0); /* Installed once, not on every boot. */

    /* Damaged primary is reinstalled from the retained staged copy. */
    PRIMARY[528] ^= 1;
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, image, sizeof image) == 0);

    /* Incomplete or uncommitted uploads never touch the running image. */
    uint8_t a[sizeof image];
    memcpy(a, image, sizeof image);
    make_image(0xB2);
    upload();
    erases = 0;
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, a, sizeof a) == 0 && erases == 0);
    upload();
    writes = 0; reset_after = 1;
    if (setjmp(reset) == 0) { command(UPDATE_END, crc32(), UPDATE_OK); assert(0); }
    reset_after = 0;
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, a, sizeof a) == 0); /* Torn commit ignored. */
    assert(boot_diagnostics.error_code == ERROR_RECORD && boot_diagnostics.valid_banks == 1);

    /* Power lost halfway through installation: the next boot copies again. */
    upload_commit();
    assert(select_with_reset_after(40) == -2);
    assert(memcmp(PRIMARY, image, sizeof image) != 0);
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, image, sizeof image) == 0);
    /* Power lost while writing the installed mark: the copy was already verified. */
    upload_commit();
    assert(select_with_reset_after((sizeof image + 3) / 4 + 1) == -2);
    assert(memcmp(PRIMARY, image, sizeof image) == 0 && RECORD[8] == 0 && RECORD[9] == 0xff);
    erases = 0;
    assert(boot_select(&hdr) == 0 && erases == 0);

    /* Committed staged copy damaged before installation: keep the running image. */
    memcpy(a, image, sizeof image);
    make_image(0xC3);
    upload_commit();
    STAGING[528] ^= 1;
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, a, sizeof a) == 0);
    assert(boot_diagnostics.selected_bank == SLOT_PRIMARY && boot_diagnostics.valid_banks == 1);
    assert(boot_diagnostics.rejected_bank == SLOT_STAGING && boot_diagnostics.reject_reason == ERROR_HASH);
    assert(boot_diagnostics.rejected_version_known && boot_diagnostics.rejected_version.iv_major == 1);
    assert(boot_diagnostics.pending & (1U << DIAG_ROLLBACK));
    PRIMARY[528] ^= 1;
    assert(boot_select(&hdr) != 0); /* Both damaged: recovery. */
    assert(boot_diagnostics.selected_bank == 0 && boot_diagnostics.valid_banks == 0);
    assert(boot_diagnostics.rejected_bank == 0 && !(boot_diagnostics.pending & (1U << DIAG_VERSION)));

    /* Image programmed directly into the primary slot (ST-LINK) is kept, and an
     * already installed staged copy does not overwrite it. */
    memset(memory, 0xff, sizeof memory);
    memcpy(PRIMARY, image, sizeof image);
    assert(boot_select(&hdr) == 0);
    make_image(0xD4);
    upload_commit();
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, image, sizeof image) == 0);
    make_image(0xE5);
    memset(PRIMARY, 0xff, sizeof image);
    memcpy(PRIMARY, image, sizeof image);
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, image, sizeof image) == 0);

    /* Flash and read failures. */
    fail_erase = 1;
    command(UPDATE_BEGIN, sizeof image, UPDATE_FLASH_ERROR);
    fail_erase = 0;
    command(UPDATE_BEGIN, sizeof image, UPDATE_OK);
    command(UPDATE_DATA, 0, UPDATE_OK);
    fail_write = 1;
    command(UPDATE_DATA, 4, UPDATE_FLASH_ERROR);
    fail_write = 0;
    command(UPDATE_DATA, 4, UPDATE_BAD_STATE);
    fail_read = 1;
    assert(boot_select(&hdr) != 0);
    fail_read = 0;
    assert(boot_select(&hdr) == 0 && memcmp(PRIMARY, image, sizeof image) == 0);

    assert(boot_image_vectors_valid(0x20040000, 0x08010209, 17));
    assert(!boot_image_vectors_valid(0x20040008, 0x08010209, 17)); /* Stack above SRAM. */
    assert(!boot_image_vectors_valid(0x20040001, 0x08010209, 17)); /* Misaligned stack. */
    assert(!boot_image_vectors_valid(0x20040000, 0x08010208, 17)); /* ARM-state PC. */
    assert(!boot_image_vectors_valid(0x20040000, 0x08000001, 17)); /* Bootloader address. */
    assert(!boot_image_vectors_valid(0x20040000, 0x08010213, 17)); /* Past the payload. */
    puts("PASS: staging upload, verified install, reinstall fallback, torn commit/install, direct primary, CAN retries/errors, diagnostics");
    return 0;
}
