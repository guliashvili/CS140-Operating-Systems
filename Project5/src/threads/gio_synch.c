#include "gio_synch.h"
#include "interrupt.h"

void rw_lock_init(struct rw_lock *l){
  l->w_holder = NULL;
  l->level = 0;
  list_init(&l->waiters);
}

void r_lock_acquire(struct rw_lock *l){
  ASSERT(l);
  enum intr_level old_level = intr_disable ();

  bool is_first_iteration = true;
  thread_current()->waits_write = false;
  while(l->w_holder){
    ASSERT(is_first_iteration);
    list_push_back (&l->waiters, &thread_current ()->elem);
    thread_block ();

    is_first_iteration = false;
  }

  l->level++;

  intr_set_level (old_level);
}

void r_lock_release(struct rw_lock *l){
  ASSERT(l);
  enum intr_level old_level = intr_disable ();

  ASSERT(l->level);
  if(--l->level == 0 && !list_empty(&l->waiters)){
    ASSERT(!l->w_holder);
    struct thread *t = list_entry (list_pop_front(&l->waiters),
                    struct thread, elem);
    ASSERT(t->waits_write);
    thread_unblock(t);
  }

  intr_set_level (old_level);
}

void w_lock_acquire(struct rw_lock *l){
  ASSERT(l);
  enum intr_level old_level = intr_disable ();


  bool is_first_iteration = true;
  thread_current()->waits_write = true;
  while(l->w_holder || l->level){
    ASSERT(is_first_iteration);
    list_push_back(&l->waiters, &thread_current()->elem);
    thread_block();
    is_first_iteration = false;
  }
  l->w_holder = thread_current();

  intr_set_level (old_level);
}

void w_lock_release(struct rw_lock *l){
  ASSERT(l);
  ASSERT(l->w_holder == thread_current());
  enum intr_level old_level = intr_disable ();

  l->w_holder = NULL;

  struct thread *t;
  bool did_one = false;
  while(!list_empty(&l->waiters)) {
    t = list_entry (list_front(&l->waiters),
                    struct thread, elem);

    if(t->waits_write){
      if(!did_one) {
        list_pop_front(&l->waiters);
        thread_unblock(t);
      }
      break;
    }else{
      list_pop_front(&l->waiters);
      thread_unblock(t);
      did_one = true;
    }
  }

  intr_set_level (old_level);
}
