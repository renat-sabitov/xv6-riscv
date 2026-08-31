#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//

void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32 *)(PLIC + UART0_IRQ * 4) = 1;
#if 0 // upstream QEMU virtio disk
  *(uint32 *)(PLIC + VIRTIO0_IRQ * 4) = 1;
#endif
}

static void
plic_enable_int(int int_num)
{
  int hart = cpuid();
  // The enable bit for interrupt ID N is stored in bit (N mod 32) of word (N/32)
  *((volatile uint32 *)PLIC_SENABLE(hart) + int_num / 32) |=
    1 << (int_num % 32);
}

void
plicinithart(void)
{
  int hart = cpuid();
  // Keep shared UART delivery on hart 0. Every hart still gets its own PLIC
  // threshold/context initialization and its direct CLINT timer interrupt.
  if (hart == 0)
    plic_enable_int(UART0_IRQ);

#if 0 // upstream QEMU enables UART and virtio on every hart
  // set enable bits for this hart's S-mode
  // for the uart and virtio disk.
  *(uint32 *)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);
#endif

  // set this hart's S-mode priority threshold to 0.
  *(uint32 *)PLIC_SPRIORITY(hart) = 0;
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32 *)PLIC_SCLAIM(hart);
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32 *)PLIC_SCLAIM(hart) = irq;
}
