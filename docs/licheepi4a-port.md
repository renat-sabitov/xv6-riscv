# xv6 on the Lichee Pi 4A

This document describes the Lichee Pi 4A port of MIT xv6-riscv and every
intentional divergence from upstream commit `35b0884`. It is the authoritative
description of the running TH1520 port. The removable-SD work remains a
separate plan in `docs/licheepi4a-sd-plan.md`.

## Current status

The port boots from the vendor U-Boot on a 16 GB Lichee Pi 4A, releases all
four XuanTie C910 harts, reaches the xv6 shell, and has been tested with
interactive input and process output. Secondary-hart release has also passed
repeated cold-boot testing.

A normal four-hart startup contains:

```text
xv6 kernel is booting

hart 1: reset vector 0x80000000, reset released
hart 1 online
hart 2: reset vector 0x80000000, reset released
hart 2 online
hart 3: reset vector 0x80000000, reset released
hart 3 online
hart online mask: 0xf
init: starting sh
$
```

This demonstrates that all four harts complete machine- and supervisor-mode
initialization and enter `scheduler()`. It does not by itself prove sustained
user execution on every hart; the validation section lists the remaining
stress tests.

## Divergence overview

| Area | Upstream QEMU xv6 | Lichee Pi 4A port | Reason |
|---|---|---|---|
| Platform | QEMU `virt` addresses and devices | TH1520 UART, PLIC, CLINT, reset and RVBA registers | The physical platform is different |
| CPU topology | Up to eight already-started harts | Exactly four C910 harts; hart 0 releases harts 1-3 | U-Boot starts only hart 0 |
| Coherency | Assumed ready before xv6 entry | Vendor C910 cache/coherency sequence on reset secondaries | Reset harts do not inherit U-Boot's C910 CSR state |
| Page tables | Standard Sv39 flags and hardware A/D updates | XTheadMae memory types and software-set A/D bits | Required by the C910 page-table mode used here |
| Timer | `stimecmp` CSR via SSTC | C910 supervisor CLINT MMIO comparator | The board path does not use SSTC |
| External interrupts | UART and VirtIO enabled per hart | UART delivered only to hart 0 | Secondary PLIC context mapping is not validated |
| UART | Byte-spaced QEMU 16550A | TH1520 DesignWare UART with 32-bit-spaced registers | Different integration and interrupt behavior |
| Storage | VirtIO block device | `fs.img` linked into the kernel as a volatile memory disk | No board storage driver is active yet |
| ELF | Keeps `.riscv.attributes` | Discards `.riscv.attributes` | Required for the board's U-Boot `bootelf` path |
| Deployment | QEMU targets | Separate TFTP deployment makefile | Board boots an ELF fetched by U-Boot |

## Boot and deployment model

The vendor U-Boot exposes only `cpu@0` in its control device tree and provides
no usable secondary-core release or SBI HSM command. It loads xv6 as an ELF
application at physical address `0x80000000`, in machine mode. Consequently,
xv6 owns the low-level release of harts 1-3.

The ELF is transferred to the staging address `0x90000000`. `bootelf -p` then
uses its program headers to copy the loadable segment to `0x80000000`, clear
BSS, and jump to the ELF entry point. The saved guarded command is:

```text
if tftp 0x90000000 xv6.elf; then bootelf -p 0x90000000; else echo TFTP failed - xv6 not started; fi
```

Run it with:

```text
env run xv6boot
```

The conditional is important: a failed transfer must not execute a stale ELF
left in RAM. The ordinary Linux `bootcmd` remains unchanged. After xv6 has run,
a warm reset may leave hardware in a state from which the vendor Linux path
does not recover; cold power-cycle the board if its AON firmware check fails.

### Development link

The board and development host use a direct Ethernet connection:

| Setting | Host | Board/U-Boot |
|---|---|---|
| Address | `192.168.77.1/24` | `192.168.77.2/24` |
| Interface/device | `enp1s0` | `ethernet@ffe7070000` |
| Default route | No; Wi-Fi retains it | None required for TFTP |
| TFTP | `tftpd-hpa`, UDP 69, root `/srv/tftp` | Server `192.168.77.1` |

The host's NetworkManager connection is `Wired connection 1`, with manual IPv4,
no gateway or DNS, IPv6 link-local, and the connection excluded from default
route selection. The link has been verified from U-Boot at 1 Gbit/s full
duplex.

Equivalent U-Boot network settings are:

```text
env set ipaddr 192.168.77.2
env set serverip 192.168.77.1
env set netmask 255.255.255.0
env set ethact ethernet@ffe7070000
ping 192.168.77.1
```

### Build separation

Plain `make` retains normal build-only behavior:

```sh
make -j4
```

Board deployment is deliberately isolated in `Makefile.th1520`:

```sh
make -f Makefile.th1520 -j4
```

Its default `deploy` target installs `kernel/kernel` as
`/srv/tftp/xv6.elf`. The destination is configurable without changing the
normal build:

```sh
make -f Makefile.th1520 \
  TFTP_DIR=/path/to/tftp \
  TFTP_KERNEL=/path/to/tftp/xv6.elf
```

## ELF attribute removal

`kernel/kernel.ld` discards `.riscv.attributes`. This is not cosmetic. A test
ELF containing that section transferred successfully through TFTP, but the
Lichee Pi 4A U-Boot did not start it. Restoring the linker `/DISCARD/` rule
restored boot.

Do not remove this rule merely to match the upstream linker script. The
loadable kernel contents are unchanged; only metadata that this U-Boot path
cannot tolerate is omitted.

## TH1520 address space and page tables

The TH1520 peripherals used by xv6 live above the ordinary Sv39 low virtual
address range. `VAMASK` folds their physical addresses into usable kernel
virtual addresses, while page-table entries retain the full physical address.
All peripheral mappings use strongly ordered, non-bufferable XTheadMae device
attributes.

| Resource | Physical address or region | Port use |
|---|---:|---|
| UART0 | `0xFFE7014000` | Console at IRQ 36 |
| AP clock/config base | `0xFFEF010000` | Base of a 64 KiB folded mapping |
| C910 software reset | `0xFFEF014004` | Release reset bits for harts 1-3 |
| Hart RVBA registers | `0xFFEF018050` onward | Program each hart's reset vector |
| C910 interrupt-controller window | 128 MiB starting at the TH1520 PLIC base | PLIC plus supervisor CLINT comparators |

The kernel maps:

1. One page for UART0.
2. A 128 MiB interrupt-controller window containing the PLIC and CLINT.
3. A 64 KiB AP configuration window spanning the reset and RVBA registers.
4. Kernel text, data, RAM, user pages, trapframes, and trampolines as cacheable
   normal memory.

### XTheadMae divergence

The C910 uses XTheadMae bits in PTE bits 60-63. The port defines cacheable
normal memory as `C | B | T` and device memory as `SO | T`. This requires
several coordinated changes:

- Mapping permission arguments and saved PTE flags are `uint64`, not `int` or
  `uint`, so the high attribute bits survive.
- `PTE_FLAGS_MASK`, `PTE2PA`, and `PTE_FLAGS` account for the upper attribute
  bits.
- Every new leaf mapping includes `PTE_A | PTE_D`; this C910 configuration does
  not use upstream's `MENVCFG_ADUE` hardware updates.
- Kernel, user, trampoline, and trapframe memory is explicitly cacheable.
- MMIO is explicitly strongly ordered and non-bufferable.
- `MXSTATUS.MAEE` enables interpretation of the extended PTE attributes.

The upstream standard-Sv39 PTE helpers remain beside the replacements in a
disabled block for comparison. Re-enabling them on this platform would lose
the XTheadMae memory type bits.

### Physical-to-virtual UART transition

Machine mode begins with paging disabled, so `uart_addr` initially contains
the physical UART address. After each hart installs the kernel page table,
`kvminithart()` changes it to the folded virtual address. This lets the same
small UART register helpers work before and after paging is enabled.

## Four-hart startup

`NCPU` is fixed at four, matching the TH1520 C910 cluster. There is no
configurable hart mask and no forced object rebuilding. Hart 0 is entered by
U-Boot; xv6 explicitly releases exactly harts 1, 2, and 3. The upstream
`entry.S` stack selection is retained, with one 4 KiB boot stack per hart.

The required sequence is:

1. Hart 0 initializes all shared xv6 subsystems.
2. Hart 0 publishes the initialized state and marks itself online.
3. Hart 0 writes hart 1's reset-vector base address and releases its reset.
4. Hart 1 initializes its C910 cache and coherency state, enters supervisor
   mode, enables paging and traps, and marks itself online.
5. Hart 0 observes that acknowledgement before repeating the process for harts
   2 and 3.

The serialization is required. Releasing secondaries without waiting for each
preceding hart to finish coherency, paging, and trap initialization repeatedly
froze startup immediately after hart 1 was released.

### Reset and RVBA control

The cold-boot values observed under U-Boot were:

| Register | Observed value | Meaning |
|---|---:|---|
| `C910_BROM_SWRST` | `0x00000001` | C910 BootROM reset deasserted |
| `C910_SWRST` | `0x00000003` | C910 top and hart 0 released; harts 1-3 held |
| `C910_CORE_CLK_CFG` | `0x30303030` | All four core clocks enabled |
| Hart 0-3 RVBA | `0xffffd00000` | Reset vectors initially point to BootROM |

`th1520_hart_start()` performs only the checks proven useful:

1. Accept only hart IDs 1-3.
2. Require a four-byte-aligned entry address below 40 bits.
3. Refuse to rewrite a hart already out of reset.
4. Write the low and high RVBA words for `_entry` at `0x80000000`.
5. Publish initialized memory with a full barrier and `fence.i`.
6. Set only the selected hart's active-low reset-release bit.
7. Read the reset register back and confirm that the bit is set.

On a cold boot, the reset register progresses as follows:

```text
0x03 -> 0x07 -> 0x0f -> 0x1f
          h1      h2      h3
```

Earlier versions also checked U-Boot-established clock state, top-reset state,
and RVBA writeback. Repeated cold boots showed those states to be invariant,
so the redundant checks were removed. Argument validation, duplicate-release
protection, ordering barriers, and final reset-release readback remain.

The code never asserts reset and never changes the C910 top or hart 0 reset
bits. Do not put a hart back into reset after it may have begun executing
without a separately designed and tested stop protocol.

### C910 cache and coherency initialization

A secondary released from reset does not inherit hart 0's U-Boot-configured
C910 machine CSRs. Before it consumes shared xv6 state or enables its MMU, the
secondary mirrors the TH1520 vendor sequence:

| CSR | Number | Value | Purpose |
|---|---:|---:|---|
| `MCOR` | `0x7c2` | `0x70013` | Clear reset L1 cache, BTB, and BHT state |
| `MSMPR` | `0x7f3` | `0x1` | Enable snooping with `SMPEN` |
| `MCCR2` | `0x7c3` | `0xe2490009` | TH1520 cache/RAM timing |
| `MXSTATUS` | `0x7c0` | `0x638000` | Vendor machine-mode feature configuration |
| `MHINT` | `0x7c5` | `0x66e30c` | TH1520 C910 hint configuration |
| `MHCR` | `0x7c1` | `0x17f` | Cache and prediction features |
| `MHINT2` | `0x7cc` | `0x420000` | Secondary hint configuration |
| `MHINT4` | `0x7ce` | `0x410` | Secondary hint configuration |

The sequence ends with `fence rw, rw`. Hart 0 keeps the broader configuration
established by U-Boot and only ensures that its `MSMPR.SMPEN` bit is set.

`MCCR2` is SoC-specific and must not be replaced with a generic OpenC910 manual
example. Testing with only `MSMPR.SMPEN` allowed hart 1 to execute, but hart 0
retained a stale online-mask cache line and never released hart 2. The complete
vendor sequence made all acknowledgements visible and brought up all harts.

### Online handshake and diagnostics

Hart 0 publishes `started` with release ordering. Each secondary observes it
with acquire ordering, installs the shared kernel page table and trap vector,
then atomically publishes its online bit. Hart 0 waits up to
`10 * TIMER_INTERVAL`, approximately one second, before reporting a timeout
and proceeding.

`hart_boot_stage[]` narrows a failed startup:

| Stage | Last completed action |
|---:|---|
| 0 | No machine-mode progress observed |
| 1 | Machine-mode setup complete; about to execute `mret` |
| 2 | Supervisor mode observed `started` |
| 3 | Kernel paging enabled |
| 4 | Supervisor trap vector installed |
| 5 | Online bit published |

The timeout path reports the last stage but never destructively resets the
target hart.

## Timers and external interrupts

Upstream uses the SSTC extension and writes the `stimecmp` CSR. This port
enables `MXSTATUS.CLINTEE` and programs the C910's per-hart supervisor CLINT
comparator through MMIO. Each 64-bit comparator is exposed as two 32-bit
registers, so `timecmp_write32()` first writes an all-ones low word, then the
high word, then the final low word. This prevents a transient comparator value
in the past while it is being updated.

The TH1520 device tree specifies a 3 MHz timebase. `TIMER_INTERVAL` is 300,000
ticks, giving the existing xv6 tick cadence of approximately 100 ms. Every
hart enables supervisor timer interrupts and rearms its own comparator in
`clockintr()`.

External-device interrupts deliberately remain on hart 0:

- Hart 0 enables `SIE.SEIE`, supervisor PLIC writes, and the UART PLIC source.
- Secondary harts enable `SIE.STIE` but keep `SIE.SEIE` clear.
- Secondary harts do not call `plicinithart()`.
- VirtIO priority, enable, and dispatch paths are disabled.

This policy avoids relying on unverified TH1520 supervisor PLIC context
mappings. It does not prevent timer preemption or scheduling on secondary
harts. The PLIC code calculates an interrupt's enable word and bit rather than
assuming that every source fits in the first 32-bit enable word; UART0 is IRQ
36.

## UART console

The UART driver is a focused TH1520 DesignWare implementation rather than a
copy of both the upstream and platform paths. Its important differences are:

- UART0 is at `0xFFE7014000`, with IRQ 36.
- Registers are 32 bits wide and spaced four bytes apart.
- The input clock is 100 MHz and the driver configures 115200 baud, eight data
  bits, no parity.
- Initialization waits for DesignWare `USR.BUSY` before changing line control
  and clears any bootloader busy-detect condition afterward.
- Receive-available and receive-timeout interrupts drain all available bytes.
- A busy-detect interrupt is cleared by reading `USR`.
- An empty receive-timeout condition is cleared explicitly.

Process output is serialized across harts by a sleeping lock. When the
transmit holding register is busy, the writer enables the level-triggered
TX-empty interrupt and sleeps. Hart 0 handles that interrupt, disables the
source before it can storm, and wakes the writer. TX-empty stays disabled when
no writer is waiting.

Kernel `printk`, panic output, and console echo use the synchronous polling
path because they may run where sleeping is unsafe. Both paths translate `\n`
to `\r\n`; without this conversion the Lichee Pi 4A serial terminal advances
lines without returning to column zero.

An unknown UART interrupt source disables UART interrupts rather than allowing
a level-triggered interrupt storm. The current sleeping TX path and CR-LF
behavior have been confirmed on hardware with shell input and process output.

## Temporary memory disk

The TH1520 port does not use QEMU's VirtIO block device. The build converts
`fs.img` into `fs.img.o` and links it into the kernel ELF. `mem_disk_rw()`
copies xv6 blocks between the buffer cache and that linked image.

This is intentionally temporary:

- Filesystem reads and writes work through the normal buffer cache interface.
- Writes modify only the in-memory image and are lost on reboot.
- The simple copy path needs no global disk lock because buffer-cache locking
  already serializes access to each buffer and there is no shared controller
  state.
- The untouched upstream `kernel/virtio_disk.c` remains in the tree.
- The upstream `virtio_disk_rw()` call sites remain disabled beside the active
  memory-disk calls.

The next storage target is removable SD through SDIO0 at `0xFFE7090000`; see
`docs/licheepi4a-sd-plan.md`. eMMC at `0xFFE7080000` and SDIO1 are explicitly
out of scope and must not be mapped, probed, initialized, read, or written.

## Upstream-code retention

Platform replacements are kept small, and useful upstream alternatives remain
locally visible in disabled blocks:

| File | Disabled upstream alternative retained |
|---|---|
| `kernel/bio.c` | VirtIO block reads and writes |
| `kernel/main.c` | VirtIO initialization and per-hart PLIC initialization |
| `kernel/memlayout.h` | QEMU UART, CLINT, and PLIC addresses |
| `kernel/plic.c` | VirtIO priority and QEMU per-hart enable mask |
| `kernel/riscv.h` | Standard Sv39 PTE extraction and flag helpers |
| `kernel/start.c` | Common per-hart external interrupts, ADUE, and SSTC setup |
| `kernel/trap.c` | SSTC rearm and VirtIO interrupt dispatch |
| `kernel/vm.c` | QEMU UART, VirtIO, and PLIC mappings |

`kernel/virtio_disk.c` itself is not modified or deleted. The Makefile notes
the upstream VirtIO object next to the active TH1520 memory-disk objects.

UART is the deliberate exception: preserving a second, structurally
incompatible byte-register driver inside `kernel/uart.c` would obscure the
active code. Git history and the upstream commit remain the reference for that
implementation.

## File-level map

| File | Port responsibility |
|---|---|
| `Makefile` | Link SMP support and the embedded filesystem image |
| `Makefile.th1520` | Publish the built ELF to TFTP without changing normal `make` |
| `kernel/kernel.ld` | Discard U-Boot-incompatible RISC-V attributes |
| `kernel/param.h` | Set the physical four-hart topology |
| `kernel/memlayout.h` | Define TH1520 UART, PLIC, CLINT, reset, and RVBA layout |
| `kernel/riscv.h` | C910 CSRs, XTheadMae PTEs, ordered MMIO, and split comparator writes |
| `kernel/vm.c` | Map TH1520 devices and apply C910 memory attributes |
| `kernel/proc.c` | Apply cacheable attributes to trapframe and trampoline mappings |
| `kernel/start.c` | Initialize C910 coherency, privilege state, and per-hart timers |
| `kernel/th1520_smp.c` | Program RVBA and release secondary resets |
| `kernel/main.c` | Serialize releases and track boot stages and online harts |
| `kernel/plic.c` | Route UART external interrupts to hart 0 |
| `kernel/trap.c` | Rearm CLINT timers and omit inactive VirtIO dispatch |
| `kernel/uart.c` | Drive the TH1520 DesignWare console |
| `kernel/mem_disk.c` | Expose the linked filesystem image as a block device |
| `kernel/bio.c` | Select the memory disk behind the upstream buffer cache |

## Validation and troubleshooting

The following checks have passed:

- A warning-clean kernel build.
- A second build with the kernel already up to date.
- `git diff --check 35b0884`.
- ELF entry point at `0x80000000`.
- Two-hart and four-hart bring-up during development.
- Four-hart online mask `0xf` and an interactive shell.
- UART shell input and interrupt-driven process output.
- Repeated cold boots after simplifying reset-register checks.

QEMU does not model TH1520 reset/RVBA control, C910 vendor CSRs, or this device
map and therefore cannot validate the port.

### Failure guide

- **ELF transfers but nothing starts:** confirm `.riscv.attributes` is absent
  and the entry point remains `0x80000000`.
- **Output forms a staircase:** confirm both UART output paths still translate
  line feed to carriage-return plus line-feed.
- **Reset is released but no online message appears:** use
  `hart_boot_stage[]`; stages 0-1 implicate machine-mode entry, C910 setup,
  timer/PMP setup, or `mret`, while stages 2-4 isolate paging and traps.
- **Hart 1 comes online but hart 0 does not advance:** verify the full C910
  vendor CSR sequence and the serialized per-hart release. `SMPEN` alone is
  insufficient.
- **Output stops after one character:** verify TX-empty is enabled only while a
  writer waits and masked by hart 0 before the wakeup.
- **External-interrupt storm on a secondary:** confirm secondaries keep
  `SIE.SEIE` disabled and do not initialize PLIC contexts.

For stronger SMP and filesystem confidence, still run full `usertests`,
`stressfs`, `grind`, fork/exit and pipe stress, a per-hart scheduling counter,
and simultaneous console/filesystem activity under CPU load. Persistent
storage validation belongs to the separate SD plan.

## Source references

- [XuanTie OpenC910 User Manual](https://occ-intl-prod.oss-ap-southeast-1.aliyuncs.com/resource/XuanTie-OpenC910-UserManual.pdf)
- [TH1520 vendor U-Boot secondary entry](https://github.com/revyos/th1520-vendor-uboot/blob/th1520/arch/riscv/cpu/mtrap.S)
- [C9xx vendor feature setup](https://github.com/revyos/th1520-vendor-uboot/blob/th1520/arch/riscv/cpu/c9xx/feature.c)
- [TH1520 board-specific C910 settings](https://github.com/revyos/th1520-vendor-uboot/blob/th1520/board/thead/light-c910/spl.c)
