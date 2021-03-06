#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include "threads/malloc.h"


static thread_func execute_thread NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *cmdline) 
{
  char *cmdline_copy;
  tid_t tid;
  size_t i;
  char file_name[17];


  for(i = 0; i < sizeof(file_name)-1; i++) {
    if ((cmdline[i] == ' ') || cmdline[i] == '\0')
      break;
    file_name[i] = cmdline[i];
  }
  while(i < sizeof(file_name))
    file_name[i++] = '\0';
  
  lock_acquire (&filesys_lock);
  struct file * file = filesys_open (file_name);
  lock_release (&filesys_lock);
  
  if (file == NULL)
    return TID_ERROR;

  lock_acquire (&filesys_lock);
  file_deny_write (file);
  lock_release (&filesys_lock);
  
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  cmdline_copy = palloc_get_page (0);
  if (cmdline_copy == NULL)
    return TID_ERROR;
  strlcpy (cmdline_copy, cmdline, PGSIZE);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create_child (file_name, file, PRI_DEFAULT, execute_thread, cmdline_copy);
  if (tid == TID_ERROR)  
    palloc_free_page (cmdline_copy);    
  
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
execute_thread (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  struct thread *cur = thread_current();
  bool success;
  
  char *token, *save_ptr;
  int count = 0;
  char *argv_temp[100];
  
  for (token = strtok_r (file_name, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
  {
    argv_temp[count] = token;
    count ++;   
  }
  
  char *argv_count[count];
  int i; 
  size_t sum = 0;
  size_t arg_length[count];

  for (i = 0; i < count; i++)
  {
    argv_count[i] = argv_temp[i];   
    arg_length[i] = strlen(argv_count[i]) + 1;
    sum += arg_length[i];
  }   
  
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (argv_count[0], &if_.eip, &if_.esp);

  if (!success) 
  {
    /* If load failed, quit. */
    cur->parent->child_success = false;
    sema_up (&cur->parent->child_sema);
    exit (-1);
  }   
  
  sema_up (&cur->parent->child_sema);

  char *argv_stack_address, *temp;
  argv_stack_address = (char *)(PHYS_BASE - sum);
  temp = argv_stack_address;
  for (i = 0; i < count; i++)
  {
    strlcpy (temp, argv_count[i], arg_length[i]);
    temp = (char *)temp + arg_length[i];    
  }   
  
  int num_word_align = 4 - sum % 4;
  
  uint8_t *word_align;
  word_align = (uint8_t *)argv_stack_address - num_word_align;
  
  for (i =0; i < num_word_align; i++)
  {
    *(word_align + i) = 0;    
  }    
  
  char **argvs, **temp2;
  argvs = (char**)((size_t)word_align - (4 * (count + 1)));
  temp2 = argvs;
  size_t argvs_offset = (size_t)argv_stack_address;
  for (i = 0; i < count; i++)
  {
    *temp2 = (char *)argvs_offset;
    argvs_offset += arg_length[i];
    temp2 = (char **)((size_t)temp2 + 4);
  }
  *temp2 = 0;
  
  char ***argv;
  argv = (char ***)((size_t)argvs -4);
  *argv = argvs;
  
  int *argc;
  argc = (int *)((size_t)argv - 4);
  *argc = count;
  
  if_.esp = (void *)argc - 4;
     
  palloc_free_page (file_name);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *t = thread_current ();
  struct list_elem *elem = NULL;
  struct dead_thread *d = NULL;
  int exit_code;
  while(true){
  lock_acquire (&t->children_lock);
    lforeach(elem, &t->children){
      d = list_entry(elem, struct dead_thread, child_elem);
      if (child_tid == d->tid)
        break;
    }
    //if we can't find the child
    if (elem == list_end(&t->children)){
      lock_release (&t->children_lock);
      return -1;
    }
      

    if (d->status >= THREAD_DYING){
      exit_code = d->exit_code;
      list_remove (&d->child_elem);
      lock_release (&t->children_lock);
      if (d->status == THREAD_DEAD) 
        free(d); // our responsibility to free a dead child
      return exit_code;
    }
    //otherwise, loop!
    lock_release (&t->children_lock);
    thread_yield ();
  }
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      ft_destroy (cur);
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_set_esp0 ((uint8_t *) t + PGSIZE);
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  
  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  lock_release (&filesys_lock);
  
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  /* Failed if the range of pages mapped overlaps any existing set of mapping pages. 
     (Stack validation not implimented yet!) */
  if (!validate_free_page (upage, read_bytes)) return false;

  while (read_bytes > 0 || zero_bytes > 0) 
  {
    /* Calculate how to fill this page.
    We will read PAGE_READ_BYTES bytes from FILE
    and zero the rest of the page. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    struct special_page_elem *gen_page;
    if (page_read_bytes == 0)
      gen_page = (struct special_page_elem*)new_zero_page ((uint32_t)upage);
    else
      gen_page = (struct special_page_elem*) 
                  new_exec_page ((uint32_t)upage, file, ofs, page_read_bytes, writable);
    add_lazy_page (thread_current (), gen_page);
    
    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    ofs += PGSIZE;
    upage += PGSIZE;
  }
  file_seek (file, ofs);
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct frame *frame = ft_get_page (PAL_USER | PAL_ZERO); 
  if (frame == NULL)
    return false;
  uint8_t *stack_begin = ((uint8_t *) PHYS_BASE) - PGSIZE;
  lock_acquire (&frame_lock);
  if (!install_page (stack_begin, frame, true)){
	  lock_release (&frame_lock);
	  ft_free_page (frame->user_page);
    return false;
  }
  *esp = PHYS_BASE;
  frame->virtual_address = (uint32_t *)stack_begin;
  frame->loaded = true;
  lock_release (&frame_lock);

  return true;
}
