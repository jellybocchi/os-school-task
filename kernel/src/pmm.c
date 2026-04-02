#include <common.h>

enum {
  PAGE_SHIFT = 12,
  PAGE_SIZE = 1 << PAGE_SHIFT,
  MAX_ALLOC = 16 * 1024 * 1024,
  SMALL_MAX = 2048,
};

enum page_type {
  PT_UNUSED = 0,
  PT_FREE,
  PT_SMALL,
  PT_LARGE,
};

typedef struct {
  int locked;
} rawlock_t;

typedef struct page_desc {
  int type;         // enum page_type
  int order;        // buddy order for PT_FREE/PT_LARGE
  int next;         // linked-list next for free/partial lists
  int class_idx;    // size class index for PT_SMALL
  int used;         // used objects in small slab
  int capacity;     // total objects in small slab
  int in_partial;   // whether in size-class partial list
  void *free_obj;   // small-slab free object list head
} page_desc_t;

typedef struct objnode {
  struct objnode *next;
} objnode_t;

enum { NCLASS = 9 };
static const int class_size[NCLASS] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };

static rawlock_t alloc_lock;
static rawlock_t page_lock;
static rawlock_t class_lock[NCLASS];
static int class_partial[NCLASS];

static uintptr_t heap_base;
static int heap_pages;
static page_desc_t *pages;

static int max_order;
static int free_head[32];

static inline void lock(rawlock_t *lk) {
  while(atomic_xchg(&lk->locked, 1) != 0) {
  }
}

static inline void unlock(rawlock_t *lk) {
  atomic_xchg(&lk->locked, 0);
}

static inline int irq_save() {
  int enabled = ienabled();
  iset(false);
  return enabled;
}

static inline void irq_restore(int enabled) {
  iset(enabled);
}

static inline uintptr_t align_up(uintptr_t x, uintptr_t a) {
  return (x + a - 1) & ~(a - 1);
}

static inline int ilog2_u32(unsigned x) {
  int r = 0;
  while((1u << r) < x) {
    r++;
  }
  return r;
}

static inline size_t ceil_pow2(size_t x) {
  if(x <= 1) return 1;
  size_t p = 1;
  while(p < x) {
    p <<= 1;
  }
  return p;
}

static inline int size_to_class(size_t sz) {
  for(int i = 0; i < NCLASS; i++) {
    if((size_t)class_size[i] >= sz) return i;
  }
  return -1;
}

static inline uintptr_t page_addr(int idx) {
  return heap_base + ((uintptr_t)idx << PAGE_SHIFT);
}

static inline int ptr_to_page_idx(uintptr_t p) {
  if(p < heap_base) return -1;
  uintptr_t off = p - heap_base;
  int idx = (int)(off >> PAGE_SHIFT);
  if(idx < 0 || idx >= heap_pages) return -1;
  return idx;
}

static inline void free_push(int order, int idx) {
  pages[idx].type = PT_FREE;
  pages[idx].order = order;
  pages[idx].next = free_head[order];
  free_head[order] = idx;
}

static inline int free_pop(int order) {
  int h = free_head[order];
  if(h < 0) return -1;
  free_head[order] = pages[h].next;
  pages[h].next = -1;
  return h;
}

static int free_remove(int order, int idx) {
  int *pp = &free_head[order];
  while(*pp >= 0) {
    int cur = *pp;
    if(cur == idx) {
      *pp = pages[cur].next;
      pages[cur].next = -1;
      return 1;
    }
    pp = &pages[cur].next;
  }
  return 0;
}

static void buddy_add_initial_blocks() {
  int idx = 0;
  int remain = heap_pages;
  while(remain > 0) {
    int o = max_order;
    while(o > 0) {
      int blk = 1 << o;
      if((idx & (blk - 1)) == 0 && blk <= remain) break;
      o--;
    }
    free_push(o, idx);
    idx += 1 << o;
    remain -= 1 << o;
  }
}

static int buddy_alloc_pages_nolock(int order) {
  int o = order;
  while(o <= max_order && free_head[o] < 0) {
    o++;
  }
  if(o > max_order) return -1;

  int idx = free_pop(o);
  while(o > order) {
    o--;
    int right = idx + (1 << o);
    free_push(o, right);
  }

  pages[idx].type = PT_LARGE;
  pages[idx].order = order;
  pages[idx].next = -1;
  return idx;
}

static void buddy_free_pages_nolock(int idx, int order) {
  int cur = idx;
  int o = order;
  while(o < max_order) {
    int buddy = cur ^ (1 << o);
    if(buddy < 0 || buddy >= heap_pages) break;
    if(pages[buddy].type != PT_FREE || pages[buddy].order != o) break;
    if(!free_remove(o, buddy)) break;
    if(buddy < cur) cur = buddy;
    o++;
  }
  free_push(o, cur);
}

static void partial_add_nolock(int cls, int idx) {
  if(pages[idx].in_partial) return;
  pages[idx].next = class_partial[cls];
  class_partial[cls] = idx;
  pages[idx].in_partial = 1;
}

static void partial_remove_nolock(int cls, int idx) {
  if(!pages[idx].in_partial) return;
  int *pp = &class_partial[cls];
  while(*pp >= 0) {
    int cur = *pp;
    if(cur == idx) {
      *pp = pages[cur].next;
      pages[cur].next = -1;
      pages[cur].in_partial = 0;
      return;
    }
    pp = &pages[cur].next;
  }
  pages[idx].in_partial = 0;
}

static int small_new_slab_page(int cls) {
  int pg;
  lock(&page_lock);
  pg = buddy_alloc_pages_nolock(0);
  unlock(&page_lock);
  if(pg < 0) return -1;

  int sz = class_size[cls];
  int cap = PAGE_SIZE / sz;
  uintptr_t base = page_addr(pg);
  objnode_t *head = NULL;
  for(int i = cap - 1; i >= 0; i--) {
    objnode_t *n = (objnode_t *)(base + (uintptr_t)i * sz);
    n->next = head;
    head = n;
  }

  pages[pg].type = PT_SMALL;
  pages[pg].order = 0;
  pages[pg].class_idx = cls;
  pages[pg].used = 0;
  pages[pg].capacity = cap;
  pages[pg].free_obj = head;
  pages[pg].next = -1;
  pages[pg].in_partial = 0;
  return pg;
}

static void *kalloc(size_t size) {
  int irq = irq_save();
  lock(&alloc_lock);
  if(!pages || heap_pages <= 0) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return NULL;
  }
  if(size == 0 || size > MAX_ALLOC) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return NULL;
  }

  size_t need = ceil_pow2(size);

  if(need <= SMALL_MAX) {
    int cls = size_to_class(need);
    if(cls < 0) {
      unlock(&alloc_lock);
      irq_restore(irq);
      return NULL;
    }

    lock(&class_lock[cls]);
    int pg = class_partial[cls];
    if(pg < 0) {
      int npg = small_new_slab_page(cls);
      if(npg < 0) {
        unlock(&class_lock[cls]);
        unlock(&alloc_lock);
        irq_restore(irq);
        return NULL;
      }
      partial_add_nolock(cls, npg);
      pg = npg;
    }

    objnode_t *obj = (objnode_t *)pages[pg].free_obj;
    if(!obj) {
      unlock(&class_lock[cls]);
      unlock(&alloc_lock);
      irq_restore(irq);
      return NULL;
    }
    pages[pg].free_obj = obj->next;
    pages[pg].used++;
    if(pages[pg].used >= pages[pg].capacity) {
      partial_remove_nolock(cls, pg);
    }
    unlock(&class_lock[cls]);
    unlock(&alloc_lock);
    irq_restore(irq);
    return (void *)obj;
  }

  if(need < PAGE_SIZE) need = PAGE_SIZE;
  int npage = (int)(need >> PAGE_SHIFT);
  int order = ilog2_u32((unsigned)npage);
  if(((size_t)1 << order) != (size_t)npage) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return NULL;
  }

  lock(&page_lock);
  int pg = buddy_alloc_pages_nolock(order);
  unlock(&page_lock);
  if(pg < 0) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return NULL;
  }
  void *ret = (void *)page_addr(pg);
  unlock(&alloc_lock);
  irq_restore(irq);
  return ret;
}

static void kfree(void *ptr) {
  int irq = irq_save();
  lock(&alloc_lock);
  if(!pages || heap_pages <= 0) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return;
  }
  if(!ptr) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return;
  }
  uintptr_t p = (uintptr_t)ptr;
  int pg = ptr_to_page_idx(p);
  if(pg < 0) {
    unlock(&alloc_lock);
    irq_restore(irq);
    return;
  }

  page_desc_t *d = &pages[pg];
  if(d->type == PT_LARGE) {
    lock(&page_lock);
    int order = d->order;
    d->type = PT_UNUSED;
    d->order = 0;
    d->next = -1;
    buddy_free_pages_nolock(pg, order);
    unlock(&page_lock);
    unlock(&alloc_lock);
    irq_restore(irq);
    return;
  }

  if(d->type == PT_SMALL) {
    int cls = d->class_idx;
    lock(&class_lock[cls]);

    int was_full = (d->used == d->capacity);
    objnode_t *obj = (objnode_t *)ptr;
    obj->next = (objnode_t *)d->free_obj;
    d->free_obj = obj;
    if(d->used > 0) d->used--;

    if(was_full) {
      partial_add_nolock(cls, pg);
    }

    if(d->used == 0) {
      partial_remove_nolock(cls, pg);
      d->type = PT_UNUSED;
      d->order = 0;
      d->next = -1;
      d->free_obj = NULL;
      unlock(&class_lock[cls]);

      lock(&page_lock);
      buddy_free_pages_nolock(pg, 0);
      unlock(&page_lock);
      unlock(&alloc_lock);
      irq_restore(irq);
      return;
    }

    unlock(&class_lock[cls]);
  }
  unlock(&alloc_lock);
  irq_restore(irq);
}

static void pmm_init() {
  uintptr_t start = align_up((uintptr_t)heap.start, PAGE_SIZE);
  uintptr_t end = (uintptr_t)heap.end & ~(uintptr_t)(PAGE_SIZE - 1);
  if(end <= start + PAGE_SIZE) {
    printf("Got 0 MiB heap: [%p, %p)\n", heap.start, heap.end);
    heap_base = 0;
    heap_pages = 0;
    pages = NULL;
    return;
  }

  int total_pages = (int)((end - start) >> PAGE_SHIFT);
  int desc_pages = (int)align_up((uintptr_t)total_pages * sizeof(page_desc_t), PAGE_SIZE) >> PAGE_SHIFT;
  if(desc_pages >= total_pages) {
    printf("Got 0 MiB heap: [%p, %p)\n", heap.start, heap.end);
    heap_base = 0;
    heap_pages = 0;
    pages = NULL;
    return;
  }

  pages = (page_desc_t *)start;
  heap_base = start + ((uintptr_t)desc_pages << PAGE_SHIFT);
  heap_pages = total_pages - desc_pages;

  alloc_lock.locked = 0;
  page_lock.locked = 0;
  for(int i = 0; i < NCLASS; i++) {
    class_lock[i].locked = 0;
  }

  for(int i = 0; i < heap_pages; i++) {
    pages[i].type = PT_UNUSED;
    pages[i].order = 0;
    pages[i].next = -1;
    pages[i].class_idx = -1;
    pages[i].used = 0;
    pages[i].capacity = 0;
    pages[i].in_partial = 0;
    pages[i].free_obj = NULL;
  }

  for(int i = 0; i < 32; i++) {
    free_head[i] = -1;
  }
  for(int i = 0; i < NCLASS; i++) {
    class_partial[i] = -1;
  }

  max_order = 0;
  while((1 << (max_order + 1)) <= heap_pages && max_order + 1 < 31) {
    max_order++;
  }
  buddy_add_initial_blocks();

  uintptr_t pmsize = (uintptr_t)heap_pages << PAGE_SHIFT;
  printf("Got %d MiB heap: [%p, %p)\n", pmsize >> 20, (void *)heap_base, heap.end);
}

MODULE_DEF(pmm) = {
  .init  = pmm_init,
  .alloc = kalloc,
  .free  = kfree,
};
