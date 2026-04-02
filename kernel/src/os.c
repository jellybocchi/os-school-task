#include <common.h>
#include <os.h>

enum { MAX_HANDLERS = 64 };

typedef struct {
  int seq;
  int event;
  handler_t handler;
} irq_handler_t;

static irq_handler_t handlers[MAX_HANDLERS];
static int nhandlers;

static void os_on_irq(int seq, int event, handler_t handler) {
  panic_on(!handler, "register null irq handler");
  panic_on(nhandlers >= MAX_HANDLERS, "too many irq handlers");

  int i = nhandlers++;
  handlers[i] = (irq_handler_t) {
    .seq = seq,
    .event = event,
    .handler = handler,
  };
  while (i > 0 && handlers[i - 1].seq > handlers[i].seq) {
    irq_handler_t tmp = handlers[i - 1];
    handlers[i - 1] = handlers[i];
    handlers[i] = tmp;
    i--;
  }
}

static void os_init() {
  pmm->init();
  kmt->init();
}

static void os_run() {
  iset(true);
  while (1) {
    yield();
  }
}

static Context *os_trap(Event ev, Context *context) {
  if (ev.event == EVENT_ERROR) {
    printf("Unhandled trap: %s\n", ev.msg);
    halt(1);
  }

  Context *next = NULL;
  for (int i = 0; i < nhandlers; i++) {
    irq_handler_t *h = &handlers[i];
    if (h->event == EVENT_NULL || h->event == ev.event) {
      Context *ret = h->handler(ev, context);
      panic_on(ret && next, "multiple irq handlers returned context");
      if (ret) {
        next = ret;
      }
    }
  }
  panic_on(!next, "trap returns null context");
  return next;
}

MODULE_DEF(os) = {
  .init = os_init,
  .run = os_run,
  .trap = os_trap,
  .on_irq = os_on_irq,
};
