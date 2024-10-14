//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart.
// called by printf(), and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  /*Here we call usrtputc_sync, it */
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input
  /*we read form cons.r and write to cons.w in buffer, we also
  need a cons.e, since...
  The buffer we mentioned here refers to the buffer hold by OS, it's not the UART_TX_BUF OS 
  use buffer to accumulate characters to the buffer size or when a line ended, the read/write them
  from/to UART port. These indexes are maintained throughout all functions that involves them, for 
  example*/

  /*if cons.r == cons.w, it indicates that there is nothing to read. When there is something to read,
  cons.w should be bigger than cons.r(so that when anything wants to write anything, it won't overwrite
  content that haven't been read), in order to let the buffer machanism to work, it should be keeped 
  that cons.w - cons.r <= INPUT_BUF_SIZE.*/

  /*why we need cons.e? Because UART transmit by byte, so there is one interrupt per byte, leading to 
  consoleintr beening called, however, consoleread accumulate characters, it shouldn't be copying the character
  into user addr space unless buffer is full or a line has finished. Cons.e helps to keep track of the next 
  position to write into in buffer through a sequence of interrupts without modifying cons.w. */
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

//
// user write()s to the console go here.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  for(i = 0; i < n; i++){
    char c;
    if(either_copyin(&c, user_src, src+i, 1) == -1)
      break;
    uartputc(c);
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dist indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      /*to-do, sleep. But at least we now know that a driver program initiatively yield itself when 
      there is nothing for it to do. Actually, if all apps acts this way, we may not need OS
      in the case of sharing a cpu bewteen processes*/
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    /*the name is either_copyout, since it handle copying from device to
    both user/kernel address space, thus is "either" */
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
/**/
void
consoleintr(int c)
{
  acquire(&cons.lock);

  switch(c){
    /*to-do, what is C('P')? And what is C('U')*/
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    /*the while moves cons.e back to the beginning of the line that is beening killed*/
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      //
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      // else we can see what we are typing...
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        //here we might infer what cons.e, cons.r, cons.w do
        cons.w = cons.e;
        wakeup(&cons.r);
      }
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
