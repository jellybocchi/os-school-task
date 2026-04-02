#include "co.h"
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE (64 * 1024)
#define MAX_CO 128

enum co_status {
  CO_NEW = 1,
  CO_RUNNING,
  CO_WAITING,
  CO_DEAD,
};

struct co {
  char *name;
  void (*func)(void *);
  void *arg;
  enum co_status status;
  struct co *waiter;
  jmp_buf context;
  uint8_t stack[STACK_SIZE];
};

static struct co main_co;
static struct co *current = NULL;
static struct co *all[MAX_CO];
static int nco = 0;

static inline void stack_switch_call(void *sp, void *entry, uintptr_t arg) {
  asm volatile(
#if __x86_64__
      "movq %0, %%rsp; movq %2, %%rdi; jmp *%1"
      :
      : "b"((uintptr_t)sp), "d"(entry), "a"(arg)
      : "memory");
#else
      "movl %0, %%esp; movl %2, 4(%0); jmp *%1"
      :
      : "b"((uintptr_t)sp - 8), "d"(entry), "a"(arg)
      : "memory");
#endif
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (!p) return NULL;
  memcpy(p, s, n);
  return p;
}

static void add_co(struct co *co) {
  assert(nco < MAX_CO);
  all[nco++] = co;
}

static void del_co(struct co *co) {
  for (int i = 0; i < nco; i++) {
    if (all[i] == co) {
      all[i] = all[nco - 1];
      nco--;
      return;
    }
  }
}

static int runnable(struct co *co) {
  return co->status == CO_NEW || co->status == CO_RUNNING;
}

static struct co *pick_next(void) {
  struct co *cand[MAX_CO];
  int m = 0;
  for (int i = 0; i < nco; i++) {
    if (runnable(all[i])) {
      cand[m++] = all[i];
    }
  }
  if (m == 0) {
    return current;
  }
  return cand[rand() % m];
}

static void co_entry(uintptr_t arg) {
  struct co *co = (struct co *)arg;
  current = co;
  co->status = CO_RUNNING;
  co->func(co->arg);
  co->status = CO_DEAD;
  if (co->waiter && co->waiter->status == CO_WAITING) {
    co->waiter->status = CO_RUNNING;
  }
  co_yield();
  abort();
}

static void co_init(void) __attribute__((constructor));
static void co_init(void) {
  memset(&main_co, 0, sizeof(main_co));
  main_co.name = (char *)"main";
  main_co.status = CO_RUNNING;
  current = &main_co;
  add_co(&main_co);

  unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
  srand(seed);
}

struct co *co_start(const char *name, void (*func)(void *), void *arg) {
  struct co *co = (struct co *)malloc(sizeof(*co));
  if (!co) return NULL;
  memset(co, 0, sizeof(*co));

  co->name = xstrdup(name ? name : "co");
  if (!co->name) {
    free(co);
    return NULL;
  }

  co->func = func;
  co->arg = arg;
  co->status = CO_NEW;
  co->waiter = NULL;
  add_co(co);
  return co;
}

void co_wait(struct co *co) {
  if (!co || co == &main_co) return;
  if (co->status == CO_DEAD) {
    del_co(co);
    free(co->name);
    free(co);
    return;
  }

  co->waiter = current;
  current->status = CO_WAITING;
  while (co->status != CO_DEAD) {
    co_yield();
  }
  current->status = CO_RUNNING;

  del_co(co);
  free(co->name);
  free(co);
}

void co_yield() {
  int val = setjmp(current->context);
  if (val == 0) {
    struct co *prev = current;
    struct co *next = pick_next();
    current = next;

    if (next->status == CO_NEW) {
      uintptr_t sp = (uintptr_t)(next->stack + STACK_SIZE);
      sp &= ~(uintptr_t)0xf;
#if __x86_64__
      sp -= 8;  
#endif
      stack_switch_call((void *)sp, (void *)co_entry, (uintptr_t)next);
      abort();
    } else {
      longjmp(next->context, 1);
    }
    (void)prev;
  }
}
