#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Lab2 - userProcess */
#include "devices/shutdown.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
/* Lab2 - userProcess */
// syscall_handler (struct intr_frame *f UNUSED) 
syscall_handler (struct intr_frame *f)
{
  /* Lab2 - userProcess */
  // printf ("system call!\n");
  // thread_exit ();

  if (!is_user_vaddr (f -> esp))
    syscall_exit (-1);

  int argv[3];

  switch (*(int *)(f -> esp))
  {
    case SYS_HALT:
      syscall_halt ();
      break;
    case SYS_EXIT:
      load_arguments (f -> esp, argv, 1);
      syscall_exit (argv[0]);
      break;
    case SYS_EXEC:
      break;
    case SYS_WAIT:
      break;
    
    case SYS_WRITE:
      load_arguments (f -> esp, argv, 3);
      if (!is_user_vaddr ((void *)argv[1]))
        syscall_exit (-1);
      f -> eax = syscall_write ((int)argv[0], (void *)argv[1], argv[2]);
      break;
    default:
      /* temporary handling */
      printf ("default syscall handling!!\n");
      // thread_exit ();
  }
}

/* Lab2 - userProcess */

void
load_arguments (int *esp, int *argv, int n)
{
  int i;
  for (i = 0; i < n; i++)
  {
    if (!is_user_vaddr (esp + 1 + i))
      syscall_exit(-1);
    argv[i] = *(esp + 1 + i);
  }
}

void
syscall_halt(void)
{
  shutdown_power_off ();
}

void
syscall_exit(int status)
{
  struct thread *thread = thread_current ();

  printf ("%s: exit(%d)\n", thread -> name, status);
  thread_exit ();
}

pid_t
sys_exec (const char *command)
{

}

int
sys_wait (pid_t pid)
{

}

int
syscall_write (int fd, void *buffer, size_t size)
{
  putbuf (buffer, size);
  return size;
}