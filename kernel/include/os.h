// When we test your kernel implementation, the framework
// directory will be replaced. Any modifications you make
// to its files (e.g., kernel.h) will be lost.

// Note that some code requires data structure definitions
// (such as `sem_t`) to compile, and these definitions are
// not present in kernel.h.

// Include these definitions in os.h.

#ifndef __OS_H__
#define __OS_H__

#include <common.h>

enum task_state {
  TASK_DEAD = 0,
  TASK_RUNNABLE,
  TASK_RUNNING,
  TASK_BLOCKED,
};

struct task {
  const char *name;
  Context *context;
  void *stack;
  enum task_state state;
  int cpu;
  struct task *next;
  struct task *wait_next;
};

struct spinlock {
  const char *name;
  int locked;
  int cpu;
};

struct semaphore {
  const char *name;
  int count;
  spinlock_t lock;
  task_t *wait_head;
  task_t *wait_tail;
};

#endif
