# MCUboot Image Format

**Title:** MCUboot — Bootloader design, *Image Format*  
**Publisher:** MCUboot project (Linaro / mcu-tools)  
**Link:** [https://docs.mcuboot.com/design.html#image-format](https://docs.mcuboot.com/design.html#image-format)

---

## Summary

The LART bootloader keeps MCUboot's image format so images can be produced by `firmware/tools/can_upload.py` (automatic) or by MCUboot's `imgtool`. No MCUboot library code runs on the MCU: [`boot_update.c`](../firmware/Core/Src/boot_update.c) parses the header and TLVs and checks the hash with the bundled mbed TLS SHA-256.

```text
offset 0      [512-byte header]
offset 512    [raw application: vector table first, linked at 0x08010200]
offset 512+N  [TLV info: magic 0x6907, total length]
              [TLV type 0x10 (SHA-256), length 32, hash of header + application]
```

---

## Header (first 32 bytes, little-endian, rest zero-padded to 512)

| Offset | Field | LART value |
|---|---|---|
| 0 | `ih_magic` | `0x96F3B83D` |
| 4 | `ih_load_addr` | 0 |
| 8 | `ih_hdr_size` | `0x200` (512) |
| 10 | `ih_protect_tlv_size` | 0 (protected TLVs rejected) |
| 12 | `ih_img_size` | Raw application size *N* |
| 16 | `ih_flags` | 0 (encryption/compression rejected) |
| 20 | `ih_ver` | major, minor, revision (u16), build (u32) — packager uses 1.0.0+0 |

---

## Checks Done by the Bootloader

1. Magic, header size 512, flags 0, no protected TLVs, size within the 192 KiB application slot.
2. TLV info magic and total length inside the slot; total file length equals the length announced by BEGIN.
3. Initial SP inside SRAM (`0x20000000`–`0x20040000`, 8-byte aligned) and Thumb reset vector inside the application payload.
4. SHA-256 of header + application equals the **single** SHA-256 TLV.

The image is **unsigned**: the hash detects corruption, not who built the image. Do not pass `imgtool --pad` or `--confirm` (they add trailer data after the TLVs).

---

## Producing an Image with imgtool (optional)

```bash
imgtool sign --header-size 0x200 --pad-header --align 4 --slot-size 0x30000 --version 1.0.0 app.bin app.mcuboot.bin
```

Normally unnecessary: the uploader and the GUI package raw `.bin` files themselves.
