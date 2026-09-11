# MPC5674F E92 RAM Kernel

This project builds a small Book E kernel for the MPC5674F used in the E92 ECU. It is loaded through the stock bootloader, runs entirely from SRAM, and provides UDS services for reading memory and erasing/programming internal flash.

The kernel is volatile. Resetting or power-cycling the ECU returns control to the firmware stored in flash.

While running, the kernel services both the MPC core watchdog and the ON20845-007 companion watchdog.

## Safety

- Use a stable, current-limited power supply.
- Do not reset or remove power during flash erase or programming.
- Keep a known-good 4 MiB image and a working BAM recovery method available.
- Preserve any device-specific shadow/censorship data required for recovery.
- Erase and program complete physical flash blocks. Flash cannot safely be treated as arbitrary byte-addressable storage.

## Build

Requirements:

- CMake 4.0 or newer
- GNU Make
- NXP's `powerpc-eabivle` GCC toolchain in `PATH`
- Git/network access the first time CMake fetches `EmbeddedIOServices`

Example using the S32 Design Studio toolchain:

```bash
export PATH="/home/daniel/NXP/S32DS_Power_v2.1/S32DS/build_tools/powerpc-eabivle-4_9/bin:$PATH"
cd /home/daniel/git/KernelMPC5674F
cmake --preset MPC5674F-Release
cmake --build build/MPC5674F-Release -j"$(nproc)"
```

Build outputs:

| File | Purpose |
| --- | --- |
| `build/Kernel-E92.bin` | Raw binary to load into SRAM |
| `build/Kernel-E92.hex` | Intel HEX representation |
| `build/MPC5674F-Release/firmware.elf` | ELF with symbols for debugging |

The binary is linked for `0x40010000` and must fit inside the 128 KiB `SRAM_CODE` region defined by `MPC5674_RAM.ld`. It is not position independent and must not be loaded at a different address.

## Runtime configuration

| Setting | Value |
| --- | --- |
| MCU | MPC5674F |
| External crystal | 8 MHz |
| System clock | 256 MHz |
| CAN peripheral | FlexCAN A |
| CAN bit rate | 500 kbit/s |
| Request CAN ID | `0x7E0` |
| Response CAN ID | `0x7E8` |
| Transport | ISO-TP |
| Kernel load/entry address | `0x40010000` |

All multibyte UDS fields described below are big-endian.

## Loading and executing the kernel

The kernel does not contain its own loader. A host must use the stock E92 application and bootloader to place `Kernel-E92.bin` in SRAM and branch to it.

The known working sequence is:

1. Establish communication with the stock application on CAN A.
2. Perform the stock security unlock.
3. Optionally suppress normal broadcast traffic.
4. Send the application-to-bootloader programming sequence:

   ```text
   10 02
   A5 01
   A5 03
   ```

5. Use the stock bootloader's RAM download protocol to write every byte of `Kernel-E92.bin`, beginning at `0x40010000`.
6. Execute address `0x40010000` with the stock bootloader command:

   ```text
   36 80 40 01 00 00
   ```

The currently used stock RAM-download framing is:

```text
34 00 <encoded transfer length:3>
36 00 <destination address:4> <data>
...
36 80 40 01 00 00
```

The encoded transfer length passed to `34` includes the six-byte `36 00 <address>` header for every data chunk. The stock loader accepts chunks containing up to 2042 data bytes. Existing working loaders send the final partial chunk first and then the full chunks in descending-address order.

After startup, the kernel sends one unsolicited ISO-TP message:

```text
99
```

Treat `99` as the ready indication. Do not begin memory or flash operations until it is received.

## Supported memory regions

| Region | Read | Write | Notes |
| --- | --- | --- | --- |
| `0x00000000`–`0x003FFFFF` | Yes | Yes | MPC5674F internal flash; ECC holes read as `FF` |
| `0x40000000`–`0x4003FFFF` | Yes | Yes | SRAM; no erase required |
| All other addresses | Returns `FF` | No | Memory outside a configured read region is never accessed |

The following flash doublewords are not accessed because they are not backed by valid ECC data. Read, upload, and hash operations substitute `FF` for each byte:

- `0x0001FFF8`–`0x0001FFFF`
- `0x0002FFF8`–`0x0002FFFF`

Read and upload requests may cross configured-region boundaries, so the entire flash can be requested as one continuous range.

## Reading memory

### Short reads with ReadMemoryByAddress

Service `0x23` uses the standard address-and-length format:

```text
Request:  23 <ALFID> <address> <length>
Response: 63 <data>
```

The low nibble of ALFID is the address length and the high nibble is the size length. Both may be one through four bytes.

Example: read 16 bytes at `0x00100000` using four-byte address and length fields:

```text
Request:  23 44 00 10 00 00 00 00 00 10
Response: 63 <16 data bytes>
```

A single `0x23` response can contain at most 4094 data bytes.

### Streaming reads with RequestUpload

Use RequestUpload for larger ranges:

```text
35 <format> <ALFID> <address> <length>
```

Supported data formats:

| Format | Meaning |
| --- | --- |
| `00` | Uncompressed |
| `10` | Independent-block LZ4 |

Example: begin an LZ4-compressed read at `0x00100000`:

```text
Request:  35 10 44 00 10 00 00 <length:4>
Response: 75 20 0F FF
```

Request each data block with an incrementing sequence counter:

```text
Request:  36 <sequence>
Response: 76 <sequence> <decoded length:2> <LZ4 block>
```

For an uncompressed upload, the response is:

```text
76 <sequence> <raw data>
```

Sequence counters begin at `01`, increment modulo 256, and may wrap to `00`. Repeating the immediately previous counter returns the same response, allowing a lost response to be retried safely.

After receiving the requested number of decoded bytes:

```text
Request:  37
Response: 77
```

## Flash-block inventory and hashes

Routine `FF00` reports the MPC5674F block map and the current SHA-256 hash of every logical block.

Send one request:

```text
31 01 FF 00
```

The kernel streams 20 separate ISO-TP records:

```text
71 01 FF 00 14 <block ID> <address:4> <length:4> <SHA-256:32>
```

`14` is the hexadecimal block count. The block ID is zero based.

Hashing occurs in 1024-byte background slices so watchdog servicing continues. While an uncached hash is being calculated, the kernel may send:

```text
7F 31 78
```

Continue waiting after response-pending. Block records follow in ID order.

Hashes are cached. An erase invalidates the corresponding hash. The kernel schedules a new background hash after the complete block has been rewritten. If inventory is requested before then, the block is hashed on demand.

## Physical flash-block map

| Block ID | Start address | Length | Physical controller |
| ---: | ---: | ---: | --- |
| 0 | `0x00000000` | `0x00004000` | Flash A low |
| 1 | `0x00004000` | `0x00004000` | Flash A low |
| 2 | `0x00008000` | `0x00004000` | Flash A low |
| 3 | `0x0000C000` | `0x00004000` | Flash A low |
| 4 | `0x00010000` | `0x00004000` | Flash A low |
| 5 | `0x00014000` | `0x00004000` | Flash A low |
| 6 | `0x00018000` | `0x00004000` | Flash A low |
| 7 | `0x0001C000` | `0x00004000` | Flash A low |
| 8 | `0x00020000` | `0x00010000` | Flash A low |
| 9 | `0x00030000` | `0x00010000` | Flash A low |
| 10 | `0x00040000` | `0x00020000` | Flash A mid |
| 11 | `0x00060000` | `0x00020000` | Flash A mid |
| 12 | `0x00080000` | `0x00040000` | Flash B low |
| 13 | `0x000C0000` | `0x00040000` | Flash B mid |
| 14 | `0x00100000` | `0x00080000` | Flash A + B high |
| 15 | `0x00180000` | `0x00080000` | Flash A + B high |
| 16 | `0x00200000` | `0x00080000` | Flash A + B high |
| 17 | `0x00280000` | `0x00080000` | Flash A + B high |
| 18 | `0x00300000` | `0x00080000` | Flash A + B high |
| 19 | `0x00380000` | `0x00080000` | Flash A + B high |

Each high logical block consists of a 256 KiB Flash A block and a 256 KiB Flash B block interleaved in 16-byte halves of each 32-byte logical line. The kernel erases those two physical halves concurrently and waits for both controllers to report success.

In the current E92 layout, the application region begins at block 14 (`0x00100000`). Treat blocks below it as bootloader, calibration, or other stock data unless a verified image layout says otherwise.

## Erasing flash

Erase by logical block ID:

```text
Request:  31 01 FF 01 <block ID>
Response: 71 01 FF 01 <block ID> <status>
```

Status values:

| Status | Meaning |
| --- | --- |
| `00` | Queued |
| `01` | Running |
| `02` | Successful |
| `03` | Failed |

Status updates are separate ISO-TP messages. Continue accepting updates until successful or failed is reported for that block.

The erase queue can hold 20 requests. Erases and writes receive a shared operation sequence when accepted, so they execute in submission order. Only the two physical halves belonging to the same high logical block erase concurrently; separate queued logical blocks remain sequential.

## Writing flash

The intended update sequence for each changed block is:

1. Obtain the block map and SHA-256 hashes with routine `FF00`.
2. Compare the desired complete block image against the reported hash.
3. Skip the block if its hash matches.
4. Queue its erase with routine `FF01`.
5. Immediately open a RequestDownload for that block.
6. Send TransferData chunks until the complete block image has been queued.
7. Retry TransferExit until programming finishes.

It is not necessary to wait for erase-success before opening the download. The shared operation ordering preserves erase-before-write execution.

### RequestDownload

Begin an LZ4-compressed download:

```text
Request:  34 10 44 <address:4> <length:4>
Response: 74 20 0F FA
```

Use format `00` instead of `10` for uncompressed data.

For internal flash:

- The download address and total length must be 8-byte aligned.
- Each decoded TransferData chunk must have an 8-byte-aligned length.
- TransferData chunks should remain within the logical block being updated.
- The intended transfer covers one complete erased logical block.

### Compressed TransferData

```text
Request:  36 <sequence> <decoded length:2> <LZ4 block>
Response: 76 <sequence>
```

The maximum decoded LZ4 block is 4096 bytes. Each block is compressed independently; no dictionary is carried between TransferData messages.

### Uncompressed TransferData

```text
Request:  36 <sequence> <raw data>
Response: 76 <sequence>
```

Sequence counters begin at `01` and increment modulo 256. Repeating the immediately previous accepted sequence counter is idempotent and returns `76` again.

The flash writer owns one active and one waiting program buffer. When both are occupied, the kernel responds:

```text
7F 36 21
```

Retry the exact same sequence counter and payload. Do not increment the counter until `76 <sequence>` is received.

`76` means the chunk was accepted into the program queue. It does not mean the bytes have already reached flash.

### Programming behavior

Programming operates on 8-byte ECC segments within 16-byte pages.

- For a segment known to remain erased, an all-`FF` requested value is skipped and any other value is programmed without first reading flash.
- For a segment not known to remain erased, an exact existing-data match is accepted as a no-op.
- A differing segment that is not known erased is rejected. It is not incrementally programmed even if its visible data would only change from 1 to 0, because hidden ECC bits might require a 0-to-1 transition.

### Completing the download

After all bytes have received `76` acknowledgements:

```text
Request:  37
```

If writes are still running:

```text
Response: 7F 37 21
```

Retry `37` until the final response arrives:

```text
Response: 77
```

Do not reset the ECU before receiving `77`.

## Writing SRAM

RequestDownload also supports SRAM at `0x40000000`–`0x4003FFFF`. SRAM does not require an erase and does not have the 8-byte flash-alignment restriction.

Use the same `34`/`36`/`37` sequence. Data is copied directly when its TransferData request is handled.

## Other supported UDS services

| Request | Response | Purpose |
| --- | --- | --- |
| `27 01` | `67 01 00 00` | Stub security-access seed used by the current tooling |
| `10 02` | `50` | Exit the RAM kernel through the stock bootloader upload-entry path |

After `10 02`, assume the kernel is no longer running and repeat the complete load/execute procedure before sending more kernel UDS requests.

## Negative response codes

| NRC | Meaning in this kernel |
| --- | --- |
| `11` | Service not supported |
| `12` | Subfunction not supported |
| `13` | Incorrect request length or format |
| `21` | Busy; retry the same request |
| `22` | Required callback or condition is unavailable |
| `24` | Transfer sequence is not active or not complete |
| `31` | Address, length, format, alignment, routine, or block ID is out of range |
| `71` | TransferData would exceed the requested transfer size |
| `72` | Erase, programming, hashing, or LZ4 operation failed |
| `73` | Wrong TransferData sequence counter |
| `78` | Response pending; operation is still active |

## Recovery behavior

If communication is lost during an update, keep power applied until the current flash operation has had time to finish. Reload the RAM kernel if necessary, request the block inventory again, and resend the complete affected block. Hash comparison identifies blocks that already match the intended image.

A normal reset or power cycle exits the kernel and starts whatever firmware remains in flash.
