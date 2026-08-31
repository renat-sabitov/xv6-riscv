#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

void main();
void timerinit();

// entry.S needs one stack per CPU.
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// Match the TH1520 vendor boot path for a C910 core released from reset.
// MCCR2 contains SoC-specific RAM/cache latency values, so this sequence is
// deliberately TH1520-specific rather than the generic C910 example values.
static void
c910_secondary_init()
{
  w_mcor(0x70013);      // clear L1 cache, BTB, and BHT state
  w_msmpr(MSMPR_SMPEN);
  w_mccr2(0xe2490009);
  w_mxstatus(0x638000);
  w_mhint(0x66e30c);
  w_mhcr(0x17f);
  w_mhint2(0x420000);
  w_mhint4(0x410);
  asm volatile("fence rw, rw" ::: "memory");
}

// entry.S jumps here in machine mode on stack0.
void
start()
{
  int id = r_mhartid();

  if(id == 0){
    // Preserve the boot hart's U-Boot-established cache configuration.
    w_msmpr(r_msmpr() | MSMPR_SMPEN);
    asm volatile("fence rw, rw" ::: "memory");
  } else {
    c910_secondary_init();
  }

  // set M Previous Privilege mode to Supervisor, for mret.
  unsigned long x = r_mstatus();
  x &= ~MSTATUS_MPP_MASK;
  x |= MSTATUS_MPP_S;
  w_mstatus(x);

  // set M Exception Program Counter to main, for mret.
  // requires gcc -mcmodel=medany
  w_mepc((uint64)main);

  // disable paging for now.
  w_satp(0);

  // delegate all interrupts and exceptions to supervisor mode.
  w_medeleg(0xffff);
  w_mideleg(0xffff);
  // Timers are per hart. Keep external interrupts on hart 0 so secondary
  // harts do not depend on unverified TH1520 PLIC context mappings.
  uint64 sie = r_sie() | SIE_STIE;
  if(id == 0)
    sie |= SIE_SEIE;
  else
    sie &= ~SIE_SEIE;
  w_sie(sie);

#if 0 // upstream QEMU enables external and SSTC timer interrupts on every hart
  w_sie(r_sie() | SIE_SEIE | SIE_STIE);
#endif

  // Enable the C910's extended memory attributes and its direct
  // supervisor-mode CLINT software/timer interrupts.
  w_mxstatus(r_mxstatus() | MXSTATUS_MAEE | MXSTATUS_CLINTEE);

  // configure Physical Memory Protection to give supervisor mode
  // access to all of physical memory.
  w_pmpaddr0(0x3fffffffffffffull);
  w_pmpcfg0(0xf);

  if (id == 0)
    *(volatile uint32 *)PLIC_CTRL_PHYS |= 1; // enable S-mode PLIC writes

#if 0 // upstream QEMU hardware updates page-table accessed/dirty bits
  // enable hardware updates of page table A and D bits
  w_menvcfg(r_menvcfg() | MENVCFG_ADUE);
#endif

  // ask for clock interrupts.
  timerinit();

  // keep each CPU's hartid in its tp register, for cpuid().
  w_tp(id);
  __atomic_store_n(&hart_boot_stage[id], 1, __ATOMIC_RELEASE);

  // switch to supervisor mode and jump to main().
  asm volatile("mret");
}

// Ask each hart to generate supervisor-mode timer interrupts using the
// C910's memory-mapped S-mode CLINT comparator.
void
timerinit()
{
  int id = r_mhartid();

#if 0 // upstream QEMU uses the SSTC extension
  // enable the sstc extension (i.e. stimecmp).
  w_menvcfg(r_menvcfg() | MENVCFG_STCE);
#endif

  // Allow supervisor mode to read the time CSR.
  w_mcounteren(r_mcounteren() | 2);

  // Paging is disabled here, so use the physical comparator address.
  timecmp_write32(CLINT_STIMECMP_PHYS(id), r_time() + TIMER_INTERVAL);

#if 0 // upstream QEMU programs the SSTC CSR directly
  // ask for the very first timer interrupt.
  w_stimecmp(r_time() + 1000000);
#endif
}
