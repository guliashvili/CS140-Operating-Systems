#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <list.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "../lib/kernel/list.h"
#include "../threads/thread.h"
#include "../threads/interrupt.h"
#include "../threads/synch.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

#define SLEEPING_THREAD_MAGIC 132432325
struct sleeping_thread{
    struct list_elem elem;
    struct thread *thread;
    int64_t wake_up_time;
    int MAGIC;
};
static void init_sleeping_thread(struct sleeping_thread* sleeping_thread, int64_t wake_up_time, struct thread* thread){
  sleeping_thread->MAGIC = SLEEPING_THREAD_MAGIC;
  sleeping_thread->wake_up_time = wake_up_time;
  sleeping_thread->thread = thread;
}
static struct list sleeping_threads_list;
static struct semaphore sleeping_threads_sema;
static struct thread *background_thread;

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

static void background_processor(void* aux UNUSED){
  background_thread = thread_current();
  sema_up(&sleeping_threads_sema);

  while(1){
    while(1) {


      struct list_elem *list_elem;

      sema_down(&sleeping_threads_sema);

      if (list_empty(&sleeping_threads_list)) {
        sema_up(&sleeping_threads_sema);
        break;
      } else {
        list_elem = list_pop_front(&sleeping_threads_list);
        sema_up(&sleeping_threads_sema);
      }

      struct sleeping_thread *sleeping_thread = list_entry(list_elem, struct sleeping_thread, elem);
      ASSERT(sleeping_thread->MAGIC == SLEEPING_THREAD_MAGIC);
      if (sleeping_thread->wake_up_time <= ticks) {
        thread_unblock(sleeping_thread->thread);
      } else {
        sema_down(&sleeping_threads_sema);
        list_push_front(&sleeping_threads_list, &sleeping_thread->elem);
        sema_up(&sleeping_threads_sema);
        break;
      }
    }

    enum intr_level old_level = intr_disable();
    thread_block();
    intr_set_level(old_level);
  }
}

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void)
{
  list_init (&sleeping_threads_list);
  sema_init(&sleeping_threads_sema, 0);
  thread_create("iavnana", PRI_MAX, background_processor, NULL);

  sema_down(&sleeping_threads_sema);
  sema_up(&sleeping_threads_sema);

  barrier ();
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void)
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1))
  {
    loops_per_tick <<= 1;
    ASSERT (loops_per_tick != 0);
  }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void)
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then)
{
  return timer_ticks () - then;
}

static bool sleeping_thread_less_func (const struct list_elem *a,
                                       const struct list_elem *b,
                                       void *aux  __attribute__((unused))){
  ASSERT( list_entry(a, struct sleeping_thread, elem)->MAGIC == SLEEPING_THREAD_MAGIC);
  ASSERT( list_entry(b, struct sleeping_thread, elem)->MAGIC == SLEEPING_THREAD_MAGIC);
  return list_entry(a, struct sleeping_thread, elem)->wake_up_time <
         list_entry(b, struct sleeping_thread, elem)->wake_up_time;
}

/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks)
{
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);

  struct sleeping_thread my_thread;
  init_sleeping_thread(&my_thread, start + ticks, thread_current());

  sema_down(&sleeping_threads_sema);

  list_insert_ordered (&sleeping_threads_list, &my_thread.elem, sleeping_thread_less_func, NULL);

  sema_up(&sleeping_threads_sema);

  enum intr_level old_level = intr_disable();
  thread_block();
  intr_set_level(old_level);

}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms)
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us)
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns)
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms)
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us)
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns)
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void)
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

#define SLEEP_APPROXIMATION -1
/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  thread_tick ();

  if(ticks & SLEEP_APPROXIMATION){
    try_thread_unblock(background_thread);
  }
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops)
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
          barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
        busy_wait (int64_t loops)
{
  while (loops-- > 0)
          barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom)
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.

        (NUM / DENOM) s
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
  {
    /* We're waiting for at least one full timer tick.  Use
       timer_sleep() because it will yield the CPU to other
       processes. */
    timer_sleep (ticks);
  }
  else
  {
    /* Otherwise, use a busy-wait loop for more accurate
       sub-tick timing. */
    real_time_delay (num, denom);
  }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
}
