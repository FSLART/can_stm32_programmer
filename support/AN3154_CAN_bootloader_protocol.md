# AN3154 — CAN Protocol Used in the STM32 Bootloader

**Document number:** AN3154  
**Title:** CAN protocol used in the STM32 bootloader  
**Publisher:** STMicroelectronics  
**Link:** [https://www.st.com/resource/en/application_note/an3154-can-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf](https://www.st.com/resource/en/application_note/an3154-can-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)

---

## Summary

AN3154 defines the CAN frame protocol used by the STM32 factory system memory bootloader. The GUI implements it in mode **ST ROM — AN3154**; the LART bootloader in [`../firmware/`](../firmware/) uses its own protocol instead ([`CAN_PROTOCOL.md`](../firmware/docs/CAN_PROTOCOL.md)). Unlike UART or USB DFU, the CAN bootloader communicates entirely via standard 11-bit CAN frames (no extended IDs).

### Key Protocol Points

#### Bitrate
The STM32 CAN bootloader always operates at **125 kbps**. Your host tool must reconnect at this rate after triggering the jump from the application.

#### Synchronisation
Before any command, the host must synchronise with the bootloader by repeatedly sending:

```
CAN ID = 0x079 (121 dec)
DLC    = 1
Data   = [0x00]
```

The bootloader responds on the same ID (`0x079`) with `ACK = 0x79` when ready.

#### Command / Response IDs
Each bootloader command uses a dedicated standard CAN ID:

| Command | CAN Std ID | Notes |
|---|---|---|
| Sync | `0x079` | Host TX; BL responds ACK/NACK on same ID |
| Get | `0x000` | Returns supported command list |
| Get Version | `0x001` | Bootloader version |
| Get ID | `0x002` | MCU Product ID |
| Change Speed | `0x003` | Adjust CAN bitrate |
| Read Memory | `0x011` | Read flash/RAM/OTP |
| Go | `0x021` | Jump to address |
| Write Memory | `0x031` | Write flash/RAM |
| Erase | `0x043` | Erase flash pages |

#### ACK / NACK Codes

| Code | Meaning |
|---|---|
| `0x79` | ACK — command accepted |
| `0x1F` | NACK — command rejected |

#### Write Memory Flow (`CMD_WRITE = 0x031`)

1. Host sends command frame: `CAN ID=0x031, DLC=5, data=[addr_MSB, addr+1, addr+2, addr_LSB, N−1]`
   where `N` = number of bytes to write (1–256).
2. Bootloader replies ACK (`0x79`) on `CAN ID=0x031`.
3. Host sends data in 8-byte frames: `CAN ID=0x004, DLC=8, data=[byte0..byte7]`.
   Bootloader ACKs each data frame on `CAN ID=0x031`.
4. After the last data frame, bootloader sends a final ACK on `CAN ID=0x031` when programming is complete.

> **Data frame ID:** AN3154 recommends `CAN ID = 0x04` for data frames but notes that any value from `0x00` to `0xFF` is valid. This tool uses `0x04`.

#### Global Erase (`CMD_ERASE = 0x043`)

1. Host sends: `CAN ID=0x043, DLC=1, data=[0xFF]`
2. Bootloader ACKs the command, then performs the erase (may take several seconds).
3. Second ACK received when erase is complete.

#### Go Command (`CMD_GO = 0x021`)

1. Host sends: `CAN ID=0x021, DLC=4, data=[addr_MSB, addr+1, addr+2, addr_LSB]`
2. Bootloader ACKs, initialises MSP from `[addr]`, then jumps to `[addr+4]`.

For normal application start: send `addr = 0x08000000`.

---

## Error Recovery

If a previous write session was interrupted, the bootloader may still be waiting for data bytes to complete an interrupted chunk. This tool handles that case in `_sync_bootloader()`:

- If ACK is received on `CAN ID=0x031` (CMD_WRITE) during sync, the bootloader is stuck in write-data mode.
- The tool feeds dummy 8-byte frames (`CAN ID=0x004, data=[0x00…0x00]`) to flush the pending chunk.
- When the pending chunk is flushed, the bootloader returns to command mode.
