#include <assert.h>
#include <string.h>
/* Test the real static CAN helper; unrelated board functions are discarded at link. */
#include "../Core/Src/boot_port.c"

static uint32_t tick, free_mailboxes = 3;
static unsigned sends;
static HAL_StatusTypeDef result = HAL_OK;
static uint8_t payload[8];
static uint32_t identifier;

uint32_t HAL_GetTick(void) { return tick; }
uint32_t HAL_CAN_GetTxMailboxesFreeLevel(const CAN_HandleTypeDef *can)
{
    (void)can;
    return free_mailboxes;
}
HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *can,
    const CAN_TxHeaderTypeDef *header, const uint8_t data[], uint32_t *mailbox)
{
    (void)can;
    identifier = header->StdId;
    assert(header->IDE == CAN_ID_STD && header->RTR == CAN_RTR_DATA && header->DLC == 8);
    if (identifier == BOOT_CAN_HEARTBEAT_ID) {
        assert(data[0] == 0x55U);
        for (unsigned i = 1; i <= 4; ++i) assert(data[i] == (uint8_t)(tick >> (8 * (i - 1))));
        assert(data[5] == BOOT_VERSION_MAJOR && data[6] == BOOT_VERSION_MINOR && data[7] == BOOT_VERSION_PATCH);
    } else {
        assert(identifier >= 0x582 && identifier <= 0x589);
    }
    memcpy(payload, data, 8);
    *mailbox = CAN_TX_MAILBOX0;
    ++sends;
    return result;
}
int main(void)
{
    CAN_HandleTypeDef can = {0};
    uint32_t last = 0;
    tick = 499;
    send_heartbeat(&can, &last);
    assert(sends == 0);
    for (unsigned i = 0; i < 257; ++i) {
        tick = 500U * (i + 1);
        send_heartbeat(&can, &last);
        assert(sends == i + 1);
        send_heartbeat(&can, &last);
        assert(sends == i + 1); /* No duplicate at the same tick. */
    }
    unsigned previous_sends = sends;
    for (free_mailboxes = 0; free_mailboxes <= 1; ++free_mailboxes) {
        tick += 500;
        send_heartbeat(&can, &last);
        assert(sends == previous_sends);
    }
    free_mailboxes = 3;
    result = HAL_ERROR;
    tick += 500;
    send_heartbeat(&can, &last);
    assert(sends == previous_sends + 1);
    result = HAL_OK;
    tick = 0x12345678U;
    send_heartbeat(&can, &last);
    const uint8_t expected[] = {0x55, 0x78, 0x56, 0x34, 0x12, BOOT_VERSION_MAJOR, BOOT_VERSION_MINOR, BOOT_VERSION_PATCH};
    assert(memcmp(payload, expected, sizeof expected) == 0);
    struct bootloader_bci_boot_heartbeat_t decoded_heartbeat;
    assert(bootloader_bci_boot_heartbeat_unpack(&decoded_heartbeat, payload, sizeof payload) == 0);
    assert(decoded_heartbeat.timestamp == tick && decoded_heartbeat.boot_version_major == BOOT_VERSION_MAJOR);
    assert(bootloader_bci_boot_heartbeat_unpack(&decoded_heartbeat, payload, 7) != 0);
    ++previous_sends;
    last = UINT32_MAX - 200U;
    tick = 298; /* 499 ms across tick rollover. */
    send_heartbeat(&can, &last);
    assert(sends == previous_sends + 1);
    tick = 299;
    send_heartbeat(&can, &last);
    assert(sends == previous_sends + 2);
    struct boot_diagnostics *d = &boot_diagnostics;
    memset(d, 0, sizeof *d);
    /* F412 rollback: committed staged upload rejected, primary application kept. */
    d->selected_bank = SLOT_PRIMARY; d->valid_banks = 3; d->entry_reason = ENTRY_RAM | ENTRY_ROLLBACK;
    d->selected_version = (struct image_version){1, 2, 0x3456, 0x789abcde};
    d->rejected_bank = SLOT_STAGING; d->reject_reason = ERROR_HASH; d->rejected_generation = 0;
    d->rejected_version_known = 1;
    d->rejected_version = (struct image_version){2, 3, 0x4567, 0x89abcdef};
    d->upload_bank = SLOT_STAGING; d->upload_stage = UPLOAD_VALIDATING;
    d->received = 0x12345; d->total = BOOT_UPLOAD_MAX_SIZE; d->reset_flags = 0x12345678;
    d->current_command = UPDATE_DATA;
    boot_diag_flash_error(ERROR_FLASH_WRITE, 0x41230, 0x01020304); /* Staging slot offset. */
    const uint8_t diagnostics[8][8] = {
        {1, BOOT_BOARD_TYPE, 3, 3, 0, 0, 3, 0},
        {1, 2, 0x56, 0x34, 0xde, 0xbc, 0x9a, 0x78},
        {1, 9, 2, 3, 0x78, 0x56, 0x34, 0x12},
        {2, 3, 0x45, 0x23, 1, 0, 0, 3},
        {ERROR_FLASH_WRITE, 2, UPDATE_DATA, SOURCE_FLASH, 0x30, 0x12, 4, 0},
        {2, 1, ERROR_HASH, 1, 0, 0, 0, 0},
        {0x30, 0x12, 4, 0, 4, 3, 2, 1},
        {2, 3, 0x67, 0x45, 0xef, 0xcd, 0xab, 0x89}
    };
    boot_diag_snapshot();
    assert(d->pending == 0xff);
    for (unsigned kind = 0; kind < 8; ++kind) {
        assert(send_diagnostic(&can) == 1);
        assert(identifier == 0x582U + kind);
        assert(memcmp(payload, diagnostics[kind], 8) == 0);
    }
    assert(d->pending == 0 && send_diagnostic(&can) == 0);
    d->pending = 1U << DIAG_UPLOAD;
    free_mailboxes = 1;
    unsigned before = sends;
    assert(send_diagnostic(&can) == 0 && sends == before && d->pending);
    free_mailboxes = 3; result = HAL_ERROR;
    assert(send_diagnostic(&can) == 0 && d->pending);
    result = HAL_OK; d->received = 42;
    assert(send_diagnostic(&can) == 1 && !d->pending && payload[2] == 42);
    struct bootloader_bci_boot_upload_status_t decoded_upload;
    assert(bootloader_bci_boot_upload_status_unpack(&decoded_upload, payload, sizeof payload) == 0);
    assert(decoded_upload.received_bytes == 42 && decoded_upload.total_bytes == d->total);
    d->selected_bank = d->rejected_bank = d->error_code = 0;
    d->error_source = 0;
    boot_diag_snapshot();
    assert(d->pending == ((1U << DIAG_INFO) | (1U << DIAG_DECISION) | (1U << DIAG_UPLOAD)));
    puts("PASS: heartbeat version/tick, CAN diagnostic payloads, snapshot eligibility, mailbox backpressure/retry");
    return 0;
}
