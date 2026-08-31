#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

// Program a stopped secondary C910 hart's reset vector and release reset.
// The caller must have enabled paging and mapped TH1520_AP_CFG_VIRT.
int
th1520_hart_start(int hartid, uint64 entry_pa)
{
  uint32 reset;
  uint32 bit;

  if (hartid <= 0 || hartid >= NCPU)
    return -1;
  if ((entry_pa >> 40) != 0 || (entry_pa & 3) != 0)
    return -1;

  bit = TH1520_C910_CORE_RST_N(hartid);
  reset = readl((volatile void *)TH1520_C910_SWRST);

  if (reset & bit) {
    printk("hart %d: already out of reset\n", hartid);
    return 1;
  }

  writel((uint32)entry_pa, (volatile void *)TH1520_C910_RVBA_L(hartid));
  writel((uint32)(entry_pa >> 32),
         (volatile void *)TH1520_C910_RVBA_H(hartid));

  // Publish the complete kernel image and initialized global state before the
  // target hart can fetch from its reset vector.
  __sync_synchronize();
  icache_fence();

  writel(reset | bit, (volatile void *)TH1520_C910_SWRST);
  if ((readl((volatile void *)TH1520_C910_SWRST) & bit) == 0) {
    printk("hart %d: reset release readback failed\n", hartid);
    return -1;
  }

  printk("hart %d: reset vector 0x%lx, reset released\n", hartid, entry_pa);
  return 0;
}
