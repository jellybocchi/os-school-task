#include <common.h>
#include <os.h>

enum {
  MAX_CPU = 8,
  STACK_SIZE = 32 * 1024,
  KMT_SEQ_SAVE = -0x3fffffff,
  KMT_SEQ_SCHED = 0x3fffffff,
};

typedef struct {
  int noff;
  int intena;
  task_t *current;
  Context *boot;
} cpu_state_t;

static cpu_state_t cpu_state[MAX_CPU];
static spinlock_t task_lock;
static task_t *task_list;
static task_t *task_cursor;

static inline cpu_state_t *thiscpu() {
  int cpu = cpu_current();
  panic_on(cpu < 0 || cpu >= MAX_CPU, "invalid cpu id");
  return &cpu_state[cpu];
}

static void push_off() {
  bool enabled = ienabled();
  iset(false);
  cpu_state_t *c = thiscpu();
  if (c->noff++ == 0) {
    c->intena = enabled;
  }
}

static void pop_off() {
  cpu_state_t *c = thiscpu();
  panic_on(ienabled(), "pop_off with interrupt enabled");
  panic_on(c->noff <= 0, "pop_off underflow");
  if (--c->noff == 0 && c->intena) {
    iset(true);
  }
}

static void spin_init(spinlock_t *lk, const char *name) {
  *lk = (spinlock_t) {
    .name = name,
    .locked = 0,
    .cpu = -1,
  };
}

static void spin_lock(spinlock_t *lk) {
  push_off();
  int cpu = cpu_current();
  panic_on(lk->cpu == cpu, "spin_lock reentered");
  while (atomic_xchg(&lk->locked, 1) != 0) {
  }
  __sync_synchronize();
  lk->cpu = cpu;
}

static void spin_unlock(spinlock_t *lk) {
  int cpu = cpu_current();
  panic_on(lk->locked == 0, "spin_unlock on unlocked lock");
  panic_on(lk->cpu != cpu, "spin_unlock by non-owner");
  lk->cpu = -1;
  __sync_synchronize();
  atomic_xchg(&lk->locked, 0);
  pop_off();
}

static void sem_init(sem_t *sem, const char *name, int value) {
  sem->name = name;
  sem->count = value;
  sem->wait_head = NULL;
  sem->wait_tail = NULL;
  spin_init(&sem->lock, name);
}

static void sem_wait(sem_t *sem) {
  cpu_state_t *c = thiscpu();
  task_t *current = c->current;
  panic_on(!current, "sem_wait outside thread context");

  int need_yield = 0;
  spin_lock(&sem->lock);
  sem->count--;
  if (sem->count < 0) {
    spin_lock(&task_lock);
    current->state = TASK_BLOCKED;
    current->wait_next = NULL;
    if (sem->wait_tail) {
      sem->wait_tail->wait_next = current;
    } else {
      sem->wait_head = current;
    }
    sem->wait_tail = current;
    spin_unlock(&task_lock);
    need_yield = 1;
  }
  spin_unlock(&sem->lock);

  if (need_yield) {
    yield();
  }
}

static void sem_signal(sem_t *sem) {
  task_t *wakeup = NULL;

  spin_lock(&sem->lock);
  sem->count++;
  if (sem->count <= 0 && sem->wait_head) {
    wakeup = sem->wait_head;
    sem->wait_head = wakeup->wait_next;
    if (!sem->wait_head) {
      sem->wait_tail = NULL;
    }
    wakeup->wait_next = NULL;
  }
  spin_unlock(&sem->lock);

  if (wakeup) {
    spin_lock(&task_lock);
    if (wakeup->state == TASK_BLOCKED) {
      wakeup->state = TASK_RUNNABLE;
    }
    spin_unlock(&task_lock);
  }
}

static Context *kmt_context_save(Event ev, Context *ctx) {
  (void)ev;
  cpu_state_t *c = thiscpu();
  if (c->current) {
    c->current->context = ctx;
    if (c->current->state == TASK_RUNNING) {
      c->current->state = TASK_RUNNABLE;
    }
  } else {
    c->boot = ctx;
  }
  return NULL;
}

static task_t *pick_next() {
  if (!task_list) {
    return NULL;
  }

  task_t *start = task_cursor ? task_cursor->next : task_list;
  if (!start) {
    start = task_list;
  }

  task_t *t = start;
  do {
    if (t->state == TASK_RUNNABLE) {
      task_cursor = t;
      return t;
    }
    t = t->next ? t->next : task_list;
  } while (t != start);

  return NULL;
}

static Context *kmt_schedule(Event ev, Context *ctx) {
  (void)ev;
  cpu_state_t *c = thiscpu();

  spin_lock(&task_lock);
  task_t *next = pick_next();
  if (next) {
    next->state = TASK_RUNNING;
    next->cpu = cpu_current();
    c->current = next;
    Context *ret = next->context;
    spin_unlock(&task_lock);
    return ret;
  }
  c->current = NULL;
  Context *ret = c->boot ? c->boot : ctx;
  spin_unlock(&task_lock);
  return ret;
}

static int kmt_create(task_t *task, const char *name, void (*entry)(void *arg), void *arg) {
  void *stack = pmm->alloc(STACK_SIZE);
  if (!stack) {
    return -1;
  }

  Context *context = kcontext((Area) {
    .start = stack,
    .end = (char *)stack + STACK_SIZE,
  }, entry, arg);

  *task = (task_t) {
    .name = name,
    .context = context,
    .stack = stack,
    .state = TASK_RUNNABLE,
    .cpu = -1,
    .next = NULL,
    .wait_next = NULL,
  };

  spin_lock(&task_lock);
  task->next = task_list;
  task_list = task;
  if (!task_cursor) {
    task_cursor = task;
  }
  spin_unlock(&task_lock);
  return 0;
}

static void kmt_teardown(task_t *task) {
  spin_lock(&task_lock);
  panic_on(task->state == TASK_RUNNING, "teardown running task");

  task_t **pp = &task_list;
  while (*pp && *pp != task) {
    pp = &(*pp)->next;
  }
  panic_on(!*pp, "teardown unknown task");
  *pp = task->next;

  if (task_cursor == task) {
    task_cursor = task_list;
  }
  task->state = TASK_DEAD;
  task->next = NULL;
  task->wait_next = NULL;
  spin_unlock(&task_lock);

  if (task->stack) {
    pmm->free(task->stack);
    task->stack = NULL;
  }
}

static void kmt_init() {
  for (int i = 0; i < MAX_CPU; i++) {
    cpu_state[i] = (cpu_state_t) { 0 };
  }
  spin_init(&task_lock, "task-lock");
  os->on_irq(KMT_SEQ_SAVE, EVENT_NULL, kmt_context_save);
  os->on_irq(KMT_SEQ_SCHED, EVENT_NULL, kmt_schedule);
}

MODULE_DEF(kmt) = {
  .init = kmt_init,
  .create = kmt_create,
  .teardown = kmt_teardown,
  .spin_init = spin_init,
  .spin_lock = spin_lock,
  .spin_unlock = spin_unlock,
  .sem_init = sem_init,
  .sem_wait = sem_wait,
  .sem_signal = sem_signal,
};
