#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

#include "userprog/pagedir.h"
#include "threads/palloc.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

void syscall_exit(int status) {
  printf("%s: exit(%d)\n", thread_current()->name, status);
  thread_exit();
}

/*
 * This does not check that the buffer consists of only mapped pages; it merely
 * checks the buffer exists entirely below PHYS_BASE.
 */
static void validate_buffer_in_user_region(const void* buffer, size_t length) {
  uintptr_t delta = PHYS_BASE - buffer;
  if (!is_user_vaddr(buffer) || length > delta)
    syscall_exit(-1);
}

/*
 * This does not check that the string consists of only mapped pages; it merely
 * checks the string exists entirely below PHYS_BASE.
 */
static void validate_string_in_user_region(const char* string) {
  uintptr_t delta = PHYS_BASE - (const void*)string;
  if (!is_user_vaddr(string) || strnlen(string, delta) == delta)
    syscall_exit(-1);
}

static int syscall_open(const char* filename) {
  struct thread* t = thread_current();
  if (t->open_file != NULL)
    return -1;

  t->open_file = filesys_open(filename);
  if (t->open_file == NULL)
    return -1;

  return 2;
}

static int syscall_write(int fd, void* buffer, unsigned size) {
  struct thread* t = thread_current();
  if (fd == STDOUT_FILENO) {
    putbuf(buffer, size);
    return size;
  } else if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int)file_write(t->open_file, buffer, size);
}

static int syscall_read(int fd, void* buffer, unsigned size) {
  struct thread* t = thread_current();
  if (fd != 2 || t->open_file == NULL)
    return -1;

  return (int)file_read(t->open_file, buffer, size);
}

static void syscall_close(int fd) {
  struct thread* t = thread_current();
  if (fd == 2 && t->open_file != NULL) {
    file_close(t->open_file);
    t->open_file = NULL;
  }
}

static void* syscall_sbrk(intptr_t increment) {
  struct thread* curThread = thread_current();

  void* prevEnd = curThread->heapEnd;
  void* newEnd = prevEnd + increment;

  // esp should def be assigned since this method can only be called thru syscall handler
  if (pg_round_up(newEnd) >= curThread->esp) {
    return (void*) -1;
  }

  if (newEnd > pg_round_up(prevEnd)) { // Checking page boundaries when increment > 0
    // POSSIBLE BUG: might need to do pg_round_up(prevEnd)
    // bruh i literally wrote that it was a possible bug and it took me a year to find it... dumb as shit
    void* newUserPage = pg_round_up(newEnd);
    for (void* curUserPage = pg_round_up(prevEnd); curUserPage != newUserPage; curUserPage += PGSIZE) {
      void* curKernelPage = palloc_get_page(PAL_ZERO | PAL_USER); // need to zero the pages cuz they might be non-empty
      
      if (curKernelPage == NULL) {
        // free all pages we've allocated before the current one which is null
        // basically resetting all the work we've done in this method so far
        for (void* curPtr = pg_round_up(prevEnd); curPtr <= curUserPage; curPtr += PGSIZE) {
          void* curPage = pagedir_get_page(curThread->pagedir, curPtr);
          pagedir_clear_page(curThread->pagedir, curPtr);
          palloc_free_page(curPage);
        }
        return (void*) -1;
      }

      if (pagedir_get_page(curThread->pagedir, curUserPage) != NULL || 
          !pagedir_set_page(curThread->pagedir, curUserPage, curKernelPage, true)) {
        // void* curPage = pagedir_get_page(curThread->pagedir, curUserPage);
        // pagedir_clear_page(curThread->pagedir, curUserPage);
        palloc_free_page(curKernelPage);
        return (void*) -1;
      }
    }
  } else if (newEnd < pg_round_down(prevEnd)) { // Checking page boundaries when increment < 0
    // need to free user pages that are no longer in user space since process_exit won't free these anymore
    void* newUserPage = pg_round_down(newEnd - 1);
    for (void* curUserPage = pg_round_down(prevEnd); curUserPage != newUserPage; curUserPage -= PGSIZE) {
      void* curPage = pagedir_get_page(curThread->pagedir, curUserPage);
      pagedir_clear_page(curThread->pagedir, curUserPage);
      palloc_free_page(curPage);
    }
  } else if (newEnd < prevEnd) {
    // memset(newEnd, NULL, prevEnd - newEnd);
    void* curPage = pagedir_get_page(curThread->pagedir, pg_round_down(prevEnd));
    pagedir_clear_page(curThread->pagedir, pg_round_down(prevEnd));
    palloc_free_page(curPage);
  }

  curThread->heapEnd = newEnd;
  return prevEnd;
}

static void syscall_handler(struct intr_frame* f) {
  uint32_t* args = (uint32_t*)f->esp;
  struct thread* t = thread_current();
  t->in_syscall = true;
  t->esp = f->esp;

  validate_buffer_in_user_region(args, sizeof(uint32_t));
  switch (args[0]) {
    case SYS_EXIT:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      syscall_exit((int)args[1]);
      break;

    case SYS_OPEN:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      validate_string_in_user_region((char*)args[1]);
      f->eax = (uint32_t)syscall_open((char*)args[1]);
      break;

    case SYS_WRITE:
      validate_buffer_in_user_region(&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region((void*)args[2], (unsigned)args[3]);
      f->eax = (uint32_t)syscall_write((int)args[1], (void*)args[2], (unsigned)args[3]);
      break;

    case SYS_READ:
      validate_buffer_in_user_region(&args[1], 3 * sizeof(uint32_t));
      validate_buffer_in_user_region((void*)args[2], (unsigned)args[3]);
      f->eax = (uint32_t)syscall_read((int)args[1], (void*)args[2], (unsigned)args[3]);
      break;

    case SYS_CLOSE:
      validate_buffer_in_user_region(&args[1], sizeof(uint32_t));
      syscall_close((int)args[1]);
      break;

    case SYS_SBRK:
      validate_buffer_in_user_region(&args[1], sizeof(intptr_t));
      f->eax = syscall_sbrk((intptr_t) args[1]);
      break;

    default:
      printf("Unimplemented system call: %d\n", (int)args[0]);
      break;
  }

  t->in_syscall = false;
}
