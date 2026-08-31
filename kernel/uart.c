//
// low-level driver for 16550a UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"

volatile uint64 uart_addr = UART0_PHYS; // we start without virtual memory

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers (4 bytes registers on TH1520).
#define Reg(reg) ((volatile uint32 *)((uart_addr) + (reg<<2)))

#define LSP_DEFAULT_FREQ  100000000UL

#define ReadReg(reg)     (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the UART control registers.
// some have different meanings for read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define IIR 2                 // interrupt identification register
#define IIR_NO_PENDING (1<<0)
#define IIR_ID_MASK 0xf
#define IIR_TX_EMPTY 0x2
#define IIR_RX_AVAILABLE 0x4
#define IIR_BUSY 0x7
#define IIR_RX_TIMEOUT 0xc
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send
#define USR 31                // DesignWare UART status register (offset 0x7c)
#define USR_BUSY (1<<0)

// Serialize process output from all harts. A writer sleeps while the transmit
// FIFO is busy and hart 0 wakes it from the UART TX-empty interrupt.
static struct sleeplock tx_lock;
static int tx_chan;

extern volatile int panicking; // from printk.c
extern volatile int panicked;  // from printk.c

void
uartinit(void)
{
  // disable interrupts.
  WriteReg(IER, 0x00);

  // DesignWare rejects line-control writes while the UART is busy and
  // raises a busy-detect interrupt. U-Boot may still be finishing output.
  while(ReadReg(USR) & USR_BUSY)
    ;

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // baud rate = serial clock frequency / (16 * divisor).
  uint32 divisor = ((LSP_DEFAULT_FREQ / 115200) >> 4);
  // LSB for baud rate of 115200
  WriteReg(0, divisor);

  // MSB for baud rate of 115200.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // Reading USR clears any busy-detect condition left by the bootloader
  // or by an earlier rejected line-control write.
  (void)ReadReg(USR);

  initsleeplock(&tx_lock, "uart");

  // TX-empty must remain disabled until software has queued a byte.
  // Otherwise the TH1520 continuously asserts its PLIC input.
  WriteReg(IER, IER_RX_ENABLE);
}

static void
uartputc_raw(int c)
{
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);
}

static void
uartputc_sleep(int c)
{
  for (;;) {
    sleep_prepare(&tx_chan);
    if (ReadReg(LSR) & LSR_TX_IDLE) {
      WriteReg(THR, c);
      return;
    }

    // TX-empty is level-triggered. Enable it only while a writer is waiting;
    // the interrupt handler masks it again before waking the writer.
    WriteReg(IER, ReadReg(IER) | IER_TX_ENABLE);
    sleep();
  }
}

// Transmit process output without spinning while the UART is busy.
void
uartwrite(char buf[], int n)
{
  acquiresleep(&tx_lock);
  for (int i = 0; i < n; i++) {
    if (buf[i] == '\n')
      uartputc_sleep('\r');
    uartputc_sleep(buf[i]);
  }
  releasesleep(&tx_lock);
}

// write a byte to the uart without using
// interrupts, for use by kernel printk() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  if (panicking == 0)
    push_off();

  if (panicked) {
    for (;;)
      ;
  }

  if (c == '\n')
    uartputc_raw('\r');
  uartputc_raw(c);

  if (panicking == 0)
    pop_off();
}

// try to read one input character from the UART.
// return -1 if none is waiting.
static int
uartgetc(void)
{
  // is input ready?
  if (ReadReg(LSR) & LSR_RX_READY) {
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from devintr().
void
uartintr(void)
{
  uint32 id = ReadReg(IIR) & IIR_ID_MASK;

  if (id & IIR_NO_PENDING)
    return;

  if (id == IIR_BUSY) {
    // Reading USR clears the DesignWare busy-detect interrupt.
    (void)ReadReg(USR);
    return;
  }

  if (id == IIR_TX_EMPTY) {
    // Mask the level-triggered source before waking the waiting writer.
    WriteReg(IER, ReadReg(IER) & ~IER_TX_ENABLE);
    wakeup(&tx_chan);
    return;
  }

  if (id == IIR_RX_AVAILABLE || id == IIR_RX_TIMEOUT) {
    int received = 0;
    int c;

    while ((c = uartgetc()) != -1) {
      received = 1;
      consoleintr(c);
    }

    // DesignWare can leave an empty receive-timeout interrupt asserted.
    if (id == IIR_RX_TIMEOUT && received == 0)
      (void)ReadReg(RHR);
    return;
  }

  // Disable an unexpected source rather than allowing a level-triggered
  // interrupt storm.
  WriteReg(IER, 0);
}
