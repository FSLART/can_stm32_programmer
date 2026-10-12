"""Package a raw CubeIDE application binary and upload it to the LART STM32F412 CAN bootloader."""
import argparse
import hashlib
from pathlib import Path
import struct
import time
import zlib
import cantools

DATABASE = cantools.database.load_file(Path(__file__).resolve().parents[1] / "docs/bootloader.dbc", strict=True)
REQUEST = DATABASE.get_message_by_name("BootloaderRequest")
RESPONSE = DATABASE.get_message_by_name("BootloaderResponse")
ENTER_BOOT = DATABASE.get_message_by_name("ENTER_BOOT")
REQUEST_ID, RESPONSE_ID, ENTER_BOOT_ID = REQUEST.frame_id, RESPONSE.frame_id, ENTER_BOOT.frame_id
INFO, BEGIN, DATA, END, RESET = range(5)
APP_ADDRESS = 0x08010200            # Application vector table (primary slot + 512-byte header)
RAM_START, RAM_END = 0x20000000, 0x20040000  # STM32F412 SRAM, 256 KiB
MAX_IMAGE = 0x30000                 # Primary slot: 192 KiB packaged image
ERRORS = ("OK", "bad frame", "bad state", "bad size", "bad offset", "flash error", "invalid image / SHA-256")


def check_image(image):
    """Check the file before allowing any erase on the device."""
    if len(image) < 32:
        raise ValueError("Expected an MCUboot image, not the raw application binary")
    magic, _, header, protected, size, flags = struct.unpack_from("<IIHHII", image)
    if magic != 0x96F3B83D or header != 512 or protected or flags or size < 8:
        raise ValueError("Unsupported image header: require 512-byte header, no protected TLVs/encryption")
    end = header + size
    if end + 4 > len(image):
        raise ValueError("Truncated image")
    magic, total = struct.unpack_from("<HH", image, end)
    if magic != 0x6907 or total < 4 or end + total != len(image):
        raise ValueError("Invalid TLV block; do not use imgtool --pad or --confirm")
    expected = hashlib.sha256(image[:end]).digest()
    found = 0
    pos = end + 4
    while pos < len(image):
        if pos + 4 > len(image):
            raise ValueError("Truncated TLV")
        kind, length = struct.unpack_from("<HH", image, pos)
        pos += 4
        if pos + length > len(image):
            raise ValueError("TLV exceeds image")
        if kind == 0x10:
            found += 1
            if length != 32 or image[pos:pos + length] != expected:
                raise ValueError("File SHA-256 does not match its MCUboot hash")
        pos += length
    if found != 1:
        raise ValueError("Expected exactly one SHA-256 TLV")
    return expected.hex()


def prepare_image(data):
    """Retain packaged images; wrap raw firmware linked at APP_ADDRESS."""
    if len(data) >= 4 and struct.unpack_from("<I", data)[0] == 0x96F3B83D:
        check_image(data)
        payload = data[512:512 + struct.unpack_from("<I", data, 12)[0]]
        packaged = data
    else:
        payload = data
        header = struct.pack("<IIHHIIBBHII", 0x96F3B83D, 0, 512, 0, len(payload), 0,
                             1, 0, 0, 0, 0).ljust(512, b"\0")
        body = header + payload
        packaged = body + struct.pack("<HHHH", 0x6907, 40, 0x10, 32) + hashlib.sha256(body).digest()
    if len(payload) < 8:
        raise ValueError("Application binary must contain a vector table")
    sp, pc = struct.unpack_from("<II", payload)
    if (not RAM_START < sp <= RAM_END or sp & 7 or not pc & 1
            or not APP_ADDRESS <= (pc & ~1) < APP_ADDRESS + len(payload)):
        raise ValueError(f"Invalid application vectors: build the CubeIDE application for 0x{APP_ADDRESS:08X}; "
                         "a bootloader or a binary linked for 0x08000000 cannot be relocated by packaging")
    if len(packaged) > MAX_IMAGE:
        raise ValueError(f"Packaged application exceeds the {MAX_IMAGE}-byte primary slot limit")
    check_image(packaged)
    return packaged


def print_progress(received, total):
    percent = 100 * received // total
    filled = 30 * received // total
    print(f"\rProgramming [{'#' * filled}{'-' * (30 - filled)}] {percent:3}% "
          f"({received}/{total} bytes)", end="\n" if received == total else "", flush=True)


def request(bus, payload, expected_offset=None, timeout=1.0, retries=3):
    import can
    for _ in range(retries):
        bus.send(can.Message(arbitration_id=REQUEST_ID, is_extended_id=False, data=payload), timeout=timeout)
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            msg = bus.recv(remaining)
            if msg is None:
                break
            if (msg.arbitration_id != RESPONSE_ID or msg.is_extended_id or msg.is_remote_frame
                    or msg.is_error_frame or msg.is_fd or len(msg.data) != 8):
                continue
            response = RESPONSE.decode(msg.data, decode_choices=False)
            if response["ReplyCommand"] != payload[0] | 0x80:
                continue
            if response["ProtocolVersion"] != 1:
                raise RuntimeError("Unsupported bootloader protocol version")
            status = response["Status"]
            if status:
                error = ERRORS[status] if status < len(ERRORS) else f"error {status}"
                raise RuntimeError(f"Command {payload[0]} rejected: {error}")
            value = response["ValueBytes"]
            if expected_offset is None or value == expected_offset:
                return value
    raise TimeoutError(f"No matching ACK for command {payload[0]}; reset into bootloader and retry")


def command(cmd, value=0):
    field = {INFO: "InfoReserved", BEGIN: "ImageLength", END: "ImageCRC32", RESET: "ResetReserved"}[cmd]
    return REQUEST.encode({"Command": cmd, field: value})


def wait_for_bootloader(bus, timeout=30):
    """Probe INFO until the bootloader answers; the application needs time to reset."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return request(bus, command(INFO), timeout=0.3, retries=1)
        except TimeoutError:
            if time.monotonic() >= deadline:
                raise


def upload(bus, image, progress=print_progress, log=print):
    digest = check_image(image)
    maximum = request(bus, command(INFO))
    if len(image) > maximum:
        raise ValueError(f"Image is {len(image)} bytes; device accepts at most {maximum}")
    request(bus, command(BEGIN, len(image)), expected_offset=0, timeout=20)
    progress(0, len(image))
    last_percent = 0
    for offset in range(0, len(image), 4):
        chunk = image[offset:offset + 4].ljust(4, b"\xff")
        payload = REQUEST.encode({"Command": DATA, "ImageOffset": offset,
                                  **{f"PayloadByte{i}": byte for i, byte in enumerate(chunk)}})
        request(bus, payload, expected_offset=min(offset + 4, len(image)))
        received = min(offset + 4, len(image))
        percent = 100 * received // len(image)
        if percent != last_percent:
            progress(received, len(image))
            last_percent = percent
    log("Transfer complete; validating and committing image...")
    request(bus, command(END, zlib.crc32(image)), expected_offset=len(image), timeout=20)
    log(f"Uploaded {len(image)} bytes. Device SHA-256 validation passed.\nImage SHA-256: {digest}")
    try:
        request(bus, command(RESET), timeout=1, retries=1)
    except TimeoutError:
        log("Image accepted; reset ACK not received. Reset the board to install it.")
        return
    log("Reset acknowledged. Bootloader will install the staged image and start the application.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help=f"Raw CubeIDE application .bin linked at 0x{APP_ADDRESS:08X}, "
                        "or an unpadded MCUboot image")
    parser.add_argument("--interface", default="slcan", help="python-can backend (default: slcan)")
    parser.add_argument("--channel", required=True, help="e.g. COM5 or can0")
    args = parser.parse_args()
    raw = args.image.read_bytes()
    image = prepare_image(raw)
    if image != raw:
        packaged = args.image.with_suffix(".mcuboot.bin")
        packaged.write_bytes(image)
        print(f"Packaged {args.image} -> {packaged} ({len(image)} bytes)")
    else:
        print(f"Using packaged image {args.image} ({len(image)} bytes)")
    import can
    with can.Bus(interface=args.interface, channel=args.channel, bitrate=250000) as bus:
        bus.send(can.Message(arbitration_id=ENTER_BOOT_ID, is_extended_id=False,
                             data=ENTER_BOOT.encode({"BCI_BOOT": 1})), timeout=1.0)
        print("Sent ENTER_BOOT (0x601), BCI_BOOT=1. Waiting up to 30 seconds for the bootloader.")
        print("If the application does not handle ENTER_BOOT, reset into the bootloader manually.")
        wait_for_bootloader(bus)
        upload(bus, image)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
        raise SystemExit(str(exc)) from exc
