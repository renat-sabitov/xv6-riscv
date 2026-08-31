# Lichee Pi 4A removable SD plan

## Scope and invariants

This plan covers only the removable SD card connected to TH1520 SDIO0 at
physical address `0xFFE7090000`.

The eMMC controller at `0xFFE7080000` is out of scope. The xv6 port must not
map, probe, initialize, read, or write eMMC. SDIO1 is also out of scope.

Storage writes remain disabled until a dedicated SD-card partition or explicit
non-overlapping LBA range has been selected, backed up, and recorded here.

## Phase 1: establish the SD target

Collect these facts read-only from U-Boot:

1. List MMC-class devices and positively identify the removable SD card.
2. Record its capacity, high-capacity addressing mode, and current bus width.
3. Record its complete partition table.
4. Choose a dedicated xv6 partition, preferably identified by a stable GPT
   type/name rather than by a hard-coded device number.
5. Save a recovery copy of the partition table and the selected region.

Before implementation, record:

- U-Boot SD device number: **TBD**
- SD capacity: **TBD**
- xv6 partition identifier: **TBD**
- Starting LBA: **TBD**
- LBA count: **TBD**
- Recovery image location and checksum: **TBD**

## Phase 2: read-only SDIO0 probe

Keep the linked memory disk as the active xv6 filesystem.

1. Map only the SDIO0 64 KiB controller window as non-bufferable device memory.
2. Add a polling-only probe that reports the SDHCI controller version, present
   state, card-inserted state, clock configuration, bus width, normal interrupt
   status, and error interrupt status.
3. Confirm that U-Boot leaves SDIO0 and the card in a usable transfer state.
4. Use bounded timeouts for every inhibit, command, and data wait.

No interrupts or DMA are needed for the first driver. Polling PIO keeps the
implementation small and avoids cache-coherency and descriptor requirements.

## Phase 3: command and read path

1. Implement command-inhibit waits and stale-status clearing.
2. Implement command issue, response extraction, and detailed error decoding.
3. Implement CMD13 status checks for the U-Boot-selected card.
4. Implement CMD17 single-block PIO reads using the SDHCI data buffer.
5. Compare a known sector byte-for-byte with a U-Boot read or host-side image.
6. Test first and last sectors of the future xv6 partition without writing.

The driver must translate byte versus block addressing from the detected SD
high-capacity mode; this must not be inferred only from card size.

## Phase 4: restricted write path

1. Add CMD24 single-block PIO writes with card-ready and transfer-complete
   checks.
2. Enforce the selected xv6 partition bounds in the kernel before issuing a
   write command.
3. Back up one disposable sector, write a test pattern, verify it, restore the
   original data, and verify the restoration.
4. Refuse writes when the partition location is unspecified or the card is
   absent, replaced, write-protected, or reports an error state.

## Phase 5: xv6 integration

1. Serialize controller access with one lock.
2. Translate each xv6 filesystem block into two 512-byte SD sectors relative to
   the dedicated partition's starting LBA.
3. Connect the driver to `bread()` and `bwrite()` through the existing buffer
   cache interface.
4. Add an explicit host-side deployment action that writes `fs.img` only to
   the chosen SD partition and verifies the complete image after writing.
5. Keep a compile-time memory-disk fallback during board validation.

## Phase 6: validation and fallback removal

Run, in order:

1. Repeated cold boots and shell startup.
2. Basic file creation, readback, rename, link, and removal.
3. `usertests` and `stressfs`.
4. Forced reset during logged writes, followed by recovery verification.
5. Multi-hart filesystem stress.
6. Card removal/replacement behavior while no I/O is active.

Remove the linked memory-disk fallback only after the SD path passes repeated
cold boots, recovery tests, and multi-hart stress without data corruption.

