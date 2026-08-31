// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- qemu's boot ROM loads the kernel here,
//             then jumps here.
// unused RAM after 80000000.

#if 0 // upstream QEMU platform addresses
#define UART0     0x10000000L
#define UART0_IRQ 10

// core-local interrupt controller (CLINT)
#define CLINT_BASE  0x02000000L
#define CLINT(hart) (CLINT_BASE + (hart) * 4)

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC                 0x0c000000L
#endif

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0_PHYS 0xFFE7014000UL
#define UART0_VIRT (UART0_PHYS & VAMASK)
#define UART0_IRQ  (20 + 16)

// virtio mmio interface
#define VIRTIO0     0x10001000
#define VIRTIO0_IRQ 1

#define TH1520_C910_TOP13BITS (0x1FFBUL << 27)

// TH1520 AP clock, reset, and system-control registers. The reset-vector
// registers accept a 40-bit physical address split into low/high words.
#define TH1520_AP_CLK_PHYS 0xFFEF010000UL
#define TH1520_AP_CFG_VIRT (TH1520_AP_CLK_PHYS & VAMASK)
#define TH1520_AP_CFG_SIZE 0x10000UL

#define TH1520_C910_SWRST (TH1520_AP_CFG_VIRT + 0x4004)
#define TH1520_C910_RVBA_L(hart) (TH1520_AP_CFG_VIRT + 0x8050 + 8 * (hart))
#define TH1520_C910_RVBA_H(hart) (TH1520_AP_CFG_VIRT + 0x8054 + 8 * (hart))

#define TH1520_C910_CORE_RST_N(hart) (1U << ((hart) + 1))

// TH1520 PLIC base here.
#define PLIC_PHYS (TH1520_C910_TOP13BITS + 0x0)
#define PLIC_CTRL_PHYS (PLIC_PHYS + 0x01FFFFC)
#define PLIC                 (PLIC_PHYS & VAMASK)
#define PLIC_PRIORITY        (PLIC + 0x0)
#define PLIC_PENDING         (PLIC + 0x1000)
#define PLIC_SENABLE(hart)   (PLIC + 0x2080 + (hart) * 0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart)    (PLIC + 0x201004 + (hart) * 0x2000)

// The C910 CLINT provides separate M-mode and S-mode timer comparators.
// TH1520 exposes each 64-bit comparator as two 32-bit MMIO registers.
#define CLINT_PHYS (TH1520_C910_TOP13BITS + 0x4000000UL)
#define CLINT (CLINT_PHYS & VAMASK)
#define CLINT_STIMECMP_PHYS(hartid)                                         \
  ((volatile uint32 *)(CLINT_PHYS + 0xd000 + 8 * (hartid)))
#define CLINT_STIMECMP(hartid)                                              \
  ((volatile uint32 *)(CLINT + 0xd000 + 8 * (hartid)))

// The TH1520 device tree specifies a 3 MHz RISC-V timebase.
#define TIMER_INTERVAL (3000000UL / 10)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
