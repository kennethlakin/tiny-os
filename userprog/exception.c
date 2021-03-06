#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/pte.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "threads/malloc.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      exit(-1); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      exit (-1);
    }
}

static const char *(strs[]) = {"false", "true"};
static inline const char* b(bool val) {return strs[val];}
static inline bool
is_stack_access (void * address, void * esp) {
  return (address < PHYS_BASE) 
      && (address > STACK_BOTTOM) 
      && (address + 32 >= esp);
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */
  struct special_page_elem *gen_page;
  void * esp;
  uint32_t fault_page;
  struct thread *cur = thread_current();
  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));
  fault_page = 0xfffff000 & ((uint32_t) fault_addr);
  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  esp = f->cs == SEL_KCSEG ? cur->esp : f->esp;
  gen_page = find_lazy_page (cur, fault_page);

  bool stack_access = is_stack_access(fault_addr, esp);
  if (gen_page == NULL && !stack_access) {
  
	  struct list_elem *elem_test;
	  struct frame *f_test;
	  
    switch (f->cs) {
    case SEL_KCSEG:
      if (!cur->in_syscall)
        printf("kernel page fault at 0x%08x\n", fault_addr);
      ASSERT(cur->in_syscall && !!"page fault in kernel");
      f->eip = (void *)f->eax;
      f->eax = -1;
      return;
    default:
      exit (-1);	
    }
  }
  
  /* Get a page of memory. */
  struct frame *frame = ft_get_page (PAL_USER);   
  if (frame == NULL){
    printf ("Unable to get a page of memory to handle a page fault\n");
    exit (-1);
  }
  uint32_t *kpage = frame->user_page;

  bool writable = true;
  bool dirty = false;
  if (gen_page != NULL) {
    switch (gen_page->type) {
    case FILE: //same as EXEC
    case EXEC:
      noop(); //why is this line needed?  Crazy C syntax
      struct exec_page *exec_page = (struct exec_page*) gen_page;

      lock_acquire (&filesys_lock);
      /* Load this page. */
      file_seek (exec_page->elf_file, exec_page->offset);
      if (file_read (exec_page->elf_file, kpage, exec_page->zero_after) 
          != (int) exec_page->zero_after) {
        ft_free_page (kpage);
        printf("Unable to read in exec file in page fault handler\n");
        lock_release (&filesys_lock);
        exit (-1);
      }
      lock_release (&filesys_lock);
      memset ((uint8_t *)kpage + exec_page->zero_after, 0, PGSIZE - exec_page->zero_after);
      if (exec_page->type == EXEC)
        writable = exec_page->writable;
      break;
    case SWAP:
      noop();
      struct swap_page *swap_page = (struct swap_page*) gen_page;

      dirty = swap_page->dirty;
      swap_slot_read (kpage, swap_page->slot);

      //delete swap_page in supplemental table.
      sema_down (&cur->page_sema);
      hash_delete (&cur->sup_pagetable, &swap_page->elem);
      sema_up (&cur->page_sema);

      if (swap_page->evicted_page != NULL)
        add_lazy_page (thread_current (), swap_page->evicted_page);
      free(swap_page);
      break;
    case ZERO:
      memset (kpage, 0, PGSIZE);
      break;
    }
  }

  lock_acquire (&frame_lock);

  /* Add the page to the process's address space. */
  if (!install_page ((void *)fault_page, frame, writable)) {
	lock_release (&frame_lock);
  
    ft_free_page (kpage);
    exit (-1);
  }

  /* Set dirty bit if this frame is dirty before swaping to the swap disk. */
  if (dirty)
	  pagedir_set_dirty (cur->pagedir, (void *)fault_page, true);
  
  frame->virtual_address = (uint32_t *) fault_page;
  frame->loaded = true;

  lock_release (&frame_lock);

}

