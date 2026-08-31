//
// driver for memdisk

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

extern uchar _binary_fs_img_start[][BSIZE];

void
mem_disk_rw(struct buf *b, int write)
{
  if (write) {
    memmove(_binary_fs_img_start[b->blockno], b->data, BSIZE);
  } else {
    memmove(b->data, _binary_fs_img_start[b->blockno], BSIZE);
  }
}
