#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

static uint32 started;
static uint32 hart_online_mask;
uint32 hart_boot_stage[NCPU];

extern char _entry[];

static void
hart_mark_online(int hartid)
{
  __atomic_fetch_or(&hart_online_mask, 1U << hartid, __ATOMIC_RELEASE);
}

static int
hart_wait_online(int hartid)
{
  uint32 bit = 1U << hartid;
  uint64 start = r_time();

  while ((__atomic_load_n(&hart_online_mask, __ATOMIC_ACQUIRE) & bit) == 0) {
    if (r_time() - start > 10 * TIMER_INTERVAL)
      return -1;
  }
  return 0;
}

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if (cpuid() == 0) {
    consoleinit();
    printkinit();
    printk("\n");
    printk("xv6 kernel is booting\n");
    printk("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
#if 0 // upstream QEMU virtio disk
    virtio_disk_init(); // emulated hard disk
#endif
    userinit();      // first user process

    hart_mark_online(0);
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);

    // Release one secondary at a time. The C910 cluster must not release the
    // next hart until the previous hart has enabled coherence and paging.
    for (int hart = 1; hart < NCPU; hart++) {
      if (th1520_hart_start(hart, (uint64)_entry) < 0)
        continue;
      if (hart_wait_online(hart) < 0)
        printk("hart %d: online timeout at stage %d\n", hart,
               __atomic_load_n(&hart_boot_stage[hart], __ATOMIC_ACQUIRE));
    }
    printk("hart online mask: 0x%x\n",
           __atomic_load_n(&hart_online_mask, __ATOMIC_ACQUIRE));
  } else {
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
      ;

    __atomic_store_n(&hart_boot_stage[cpuid()], 2, __ATOMIC_RELEASE);
    kvminithart();
    __atomic_store_n(&hart_boot_stage[cpuid()], 3, __ATOMIC_RELEASE);
    trapinithart();
    __atomic_store_n(&hart_boot_stage[cpuid()], 4, __ATOMIC_RELEASE);
#if 0 // upstream QEMU initializes a PLIC context on every hart
    plicinithart(); // ask PLIC for device interrupts
#endif
    hart_mark_online(cpuid());
    __atomic_store_n(&hart_boot_stage[cpuid()], 5, __ATOMIC_RELEASE);
    printk("hart %d online\n", cpuid());
  }

  scheduler();
}
