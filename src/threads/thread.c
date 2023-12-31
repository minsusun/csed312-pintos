#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Lab1 - MLFQS */
#include "threads/fixed-op.h"

/* lab3 - supplemental page table */
#include "vm/spt.h"

/* lab3 - MMF */
#include <list.h>
#include <stdlib.h>
#include "filesys/file.h"

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of process in THREAD_SLEEP state, that is, processes
   that are sleeping till the wake_tick of each and wake up
   after wake_tick. */
static struct list sleep_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/* Lab1 - MLFQS */
int load_avg;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&sleep_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Lab1 - MLFQS */
  load_avg = LOAD_AVG_DEFAULT;

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Lab2 - systemCall */
  struct thread *parent = thread_current ();
  struct thread *child = t;

  /* PCB */
  struct pcb *pcb = child -> pcb = palloc_get_page (0);

  if (pcb == NULL)
    return TID_ERROR;
  
  pcb -> exitcode = -1;
  pcb -> isexited = false;
  pcb -> isloaded = false;

  sema_init (&(pcb -> load), 0);
  sema_init (&(pcb -> wait), 0);

  pcb -> _file = NULL;

  pcb -> fdtable = palloc_get_page (PAL_ZERO);
  pcb -> fdcount = 2;

  if (pcb -> fdtable == NULL)
  {
    palloc_free_page (pcb);
    return TID_ERROR;
  }

  child -> parent = parent;

  list_push_back (&(parent -> child_list), &(child -> childelem));

  /* lab3 - supplemental page table */
  init_spt (&(t -> spt));

  /* lab3 - MMF */
  list_init (&(t -> mmf_list));
  t -> mmfid = 0;

  /* Add to run queue. */
  thread_unblock (t);

  /* Lab1 - priority scheduling */
  /* The priority of current thread is changed due to the thread_init().
     Validate current thread's priority.  */
  thread_validate_priority ();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  // list_push_back (&ready_list, &t->elem);
  
  /* Lab1 - priority scheduling */
  list_insert_ordered (&ready_list, &t -> elem, thread_compare_priority, 0);
  
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
  {
    // list_push_back (&ready_list, &cur->elem);

    /* Lab1 - priority scheduling */
    list_insert_ordered (&ready_list, &cur -> elem, thread_compare_priority, 0);
  }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  /* Lab1 - MFLQS */
  if (thread_mlfqs) return;

  // thread_current ()->priority = new_priority;

  /* Lab1 - priority donation */
  thread_current () -> priority_original = new_priority;

  /* Lab1 - priority scheduling */
  /* Update donation relation due to change of original priority.  */
  update_donation ();
  /* The priority of current thread is changed due to the new priority.
     Validate current thread's priority.  */
  thread_validate_priority ();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Lab1 - MLFQS */
  enum intr_level old_level = intr_disable ();
  struct thread *thread = thread_current ();
  thread -> nice = nice;
  mlfqs_update_priority (thread);
  
  // Hotfix #1
  list_sort (&ready_list, thread_compare_priority, 0);
  if (thread != idle_thread) thread_validate_priority ();
  
  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Lab1 - MLFQS */
  enum intr_level old_level = intr_disable ();
  int nice = thread_current () -> nice;
  intr_set_level (old_level);
  return nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Lab1 - MLFQS */
  enum intr_level old_level = intr_disable ();
  int _load_avg = fp_int_round (fp_mul (load_avg, int_fp (100)));
  intr_set_level (old_level);
  return _load_avg;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Lab1 - MLFQS */
  enum intr_level old_level = intr_disable ();
  int recent_cpu = fp_int_round (fp_mul (thread_current () -> recent_cpu, int_fp (100)));
  intr_set_level (old_level);
  return recent_cpu;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;

  /* Lab1 - alarm clock */
  t-> wakeup_ticks = -1;
  /* Lab1 - priority donation */
  t -> priority_original = priority;
  t -> _lock = NULL;
  list_init (&t -> donation_list);
  /* Lab1 - MLFQS */
  t -> nice = NICE_DEFAULT;
  t -> recent_cpu = RECENT_CPU_DEFAULT;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);

  /* Lab2 - systemCall */
  list_init (&(t -> child_list));
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      // palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);

/* Lab1 - alarm clock */
bool
thread_compare_wakeup_ticks (const struct list_elem *p1, const struct list_elem *p2, void *aux UNUSED)
{
  return list_entry(p1, struct thread, sleep_elem) -> wakeup_ticks < list_entry(p2, struct thread, sleep_elem) -> wakeup_ticks;
}

/* Lab1 - alarm clock */
void
thread_sleep (int64_t wakeup_ticks)
{
  enum intr_level old_level = intr_disable ();
  struct thread *current = thread_current ();

  ASSERT (current != idle_thread);
  ASSERT (current -> status == THREAD_RUNNING);

  current -> wakeup_ticks = wakeup_ticks;
  
  list_insert_ordered (&sleep_list, &current -> sleep_elem, thread_compare_wakeup_ticks, 0);
  thread_block ();

  intr_set_level (old_level);
}

/* Lab1 - alarm clock */
void
thread_wakeup (int64_t current_ticks)
{
  struct list_elem *element = list_begin (&sleep_list);

  while (element != list_end (&sleep_list))
  {
    struct thread *thread_ = list_entry (element, struct thread, sleep_elem);

    if (thread_ -> wakeup_ticks > current_ticks)
      break;
    
    element = list_remove (element);
    thread_unblock (thread_);
  }
}

/* Lab1 - priority scheduling */
bool
thread_compare_priority (const struct list_elem *p1, const struct list_elem *p2, void *aux UNUSED)
{
  return list_entry (p1, struct thread, elem) -> priority > list_entry (p2, struct thread, elem) -> priority;
}

/* Lab1 - priority scheduling */
void
thread_validate_priority (void)
{
  /* If current thread has lower priority than the highest priority of ready_list,
     it should be re-scheduled. So, just yield the current thread. */
  if (!list_empty (&ready_list) && thread_current () -> priority < list_entry (list_front (&ready_list), struct thread, elem) -> priority)
    thread_yield ();
}

/* Lab1 - priority donation */
bool
thread_compare_donation_priority (const struct list_elem *p1, const struct list_elem *p2, void *aux UNUSED)
{
  return list_entry (p1, struct thread, donation_elem) -> priority > list_entry (p2, struct thread, donation_elem) -> priority;
}

/* Lab1 - priority donation */
void
donate_priority (void)
{
  int depth;
  struct thread *current = thread_current ();
  for (depth = 0; depth < DONATION_MAX_DEPTH; depth++)
  {
    if (!current -> _lock) break;
    struct thread *holder = current -> _lock -> holder;
    /* No need to validate about priority. Currently running thread must have
      higher priority. No doubts. */
    holder -> priority = current -> priority;
    current = holder;
  }
}

/* Lab1 - priority donation */
void
update_donation ()
{
  struct thread *current = thread_current ();
  current -> priority = current -> priority_original;
  if (!list_empty (&current -> donation_list))
  {
    struct thread *p1;
    
    list_sort (&current -> donation_list, thread_compare_donation_priority, 0);
    
    p1 = list_entry (list_front (&current -> donation_list), struct thread, donation_elem);
    
    if (p1 -> priority > current -> priority)
      current -> priority = p1 -> priority;
  }
}

/* Lab1 - priority donation */
void
remove_donation (struct lock *lock)
{
  struct thread *thread = thread_current ();
  struct list_elem *element = list_begin (&thread -> donation_list);
  while (element != list_end (&thread -> donation_list))
  {
    struct thread *donor = list_entry (element, struct thread, donation_elem);
    if (donor -> _lock == lock) element = list_remove (&donor -> donation_elem);
    else element = list_next (element);
  }
}

/* Lab1 - MLFQS */
void
mlfqs_update_priority (struct thread *thread)
{
  if (thread == idle_thread) return;
  // Hotfix #2
  int priority = fp_int_round (fp_add (fp_div (thread -> recent_cpu, int_fp (-4)), int_fp (PRI_MAX - thread -> nice * 2)));
  if (priority > PRI_MAX) priority = PRI_MAX;
  if (priority < PRI_MIN) priority = PRI_MIN;
  thread -> priority = priority;
}

/* Lab1 - MLFQS */
void 
mlfqs_update_priority_all (void)
{
  struct list_elem *element = list_begin (&all_list);
  while (element != list_end (&all_list))
  {
    mlfqs_update_priority (list_entry (element, struct thread, allelem));
    element = list_next (element);
  }
  // Hotfix #1
  list_sort (&ready_list, thread_compare_priority, 0);
}

/* Lab1 - MLFQS */
void
mlfqs_update_recent_cpu (struct thread *thread)
{
  if (thread == idle_thread) return;
  int k = fp_mul (int_fp (2), load_avg);      // fp
  int a = fp_div (k, fp_add (k, int_fp (1))); // fp
  thread -> recent_cpu = fp_add (fp_mul (a, thread -> recent_cpu), int_fp (thread -> nice));
}

/* Lab1 - MLFQS */
void
mlfqs_update_recent_cpu_all (void)
{
  struct list_elem *element = list_begin (&all_list);
  while (element != list_end (&all_list))
  {
    mlfqs_update_recent_cpu (list_entry (element, struct thread, allelem));
    element = list_next (element);
  }
}

/* Lab1 - MLFQS */
void
mlfqs_update_recent_cpu_tick (void)
{
  if (thread_current () != idle_thread)
    thread_current () -> recent_cpu = fp_add (thread_current () -> recent_cpu, int_fp (1));
}

/* Lab1 - MLFQS */
void
mlfqs_update_load_avg  (void)
{
  int ready_threads = list_size (&ready_list);
  if (thread_current () != idle_thread) ready_threads ++;
  int a = fp_div (int_fp (59), int_fp(60)); // fp
  int b = fp_div (int_fp (1), int_fp(60));  // fp
  load_avg = fp_add (fp_mul (a, load_avg), fp_mul (b, int_fp (ready_threads)));
}

/* Lab2 - systemCall & fileSystem */
/* Return thread pointer to the child of current process
  matching with child_tid. */
struct thread *
thread_get_child (tid_t child_tid)
{
  struct list *child_list = &(thread_current () -> child_list);
  struct list_elem *elem;

  for (elem = list_begin (child_list); elem != list_end (child_list); elem = list_next (elem))
  {
    struct thread *child = list_entry (elem, struct thread, childelem);
    
    if (child -> tid == child_tid)
      return child;
  }

  return NULL;
}

/* Return pcb pointer of child of current process. */
struct pcb *
thread_get_child_pcb (tid_t child_tid)
{
  struct thread *child = thread_get_child (child_tid);
  return (child == NULL) ? NULL : child -> pcb;
}

struct mmf *
init_mmf (int mmfid, void *upage, struct file *file)
{
  struct thread *thread = thread_current ();
  struct hash *spt = &(thread -> spt);
  struct mmf *mmf = (struct mmf *) malloc (sizeof *mmf);
  
  mmf -> id = mmfid;
  mmf -> upage = upage;
  mmf -> file = file;

  off_t size = file_length (file);

  for (off_t ofs = 0; ofs < size; ofs += PGSIZE)
    if (get_spte (spt, upage + ofs) != NULL) return NULL;

  for (off_t ofs = 0; ofs < size; ofs += PGSIZE)
  {
    uint32_t read_bytes;
    if (ofs + PGSIZE < size)
      read_bytes = PGSIZE;
    else
      read_bytes = size - ofs;
    
    spalloc_file (spt, upage + ofs, file, ofs, read_bytes, PGSIZE - read_bytes, true);
  }

  list_push_back (&(thread -> mmf_list), &(mmf -> list_elem));

  return mmf;
}

struct mmf *
get_mmf (int mmfid)
{
  struct list *mmf_list = &(thread_current () -> mmf_list);

  for (struct list_elem *elem = list_begin (mmf_list); elem != list_end (mmf_list); elem = list_next (elem))
  {
    struct mmf *mmf = list_entry (elem, struct mmf, list_elem);
    if (mmf -> id == mmfid)
      return mmf;
  }
  return NULL;
}