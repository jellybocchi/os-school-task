#include <am.h>
#include <klib.h>
#include <klib-macros.h>
#include <stdarg.h>
#include <stdint.h>

#if !defined(__ISA_NATIVE__) || defined(__NATIVE_USE_KLIB__)

typedef struct {
  char *buf;
  size_t size;
  size_t len;
} out_t;

static void out_char(out_t *out, char ch) {
  if (out->size > 0 && out->len + 1 < out->size) {
    out->buf[out->len] = ch;
  }
  out->len++;
}

static void out_str(out_t *out, const char *s) {
  if (!s) {
    s = "(null)";
  }
  while (*s) {
    out_char(out, *s++);
  }
}

static void out_uint(out_t *out, uintptr_t value, unsigned base) {
  static const char digits[] = "0123456789abcdef";
  char buf[2 * sizeof(uintptr_t) + 1];
  int n = 0;
  do {
    buf[n++] = digits[value % base];
    value /= base;
  } while (value);
  while (n--) {
    out_char(out, buf[n]);
  }
}

int vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
  out_t state = { .buf = out, .size = n, .len = 0 };

  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      out_char(&state, *p);
      continue;
    }

    p++;
    if (*p == '\0') {
      break;
    }

    switch (*p) {
      case '%':
        out_char(&state, '%');
        break;
      case 'c':
        out_char(&state, (char)va_arg(ap, int));
        break;
      case 's':
        out_str(&state, va_arg(ap, const char *));
        break;
      case 'd': {
        int val = va_arg(ap, int);
        uintptr_t u = (uintptr_t)val;
        if (val < 0) {
          out_char(&state, '-');
          u = (uintptr_t)(-(intptr_t)val);
        }
        out_uint(&state, u, 10);
        break;
      }
      case 'u':
        out_uint(&state, (uintptr_t)va_arg(ap, unsigned int), 10);
        break;
      case 'x':
        out_uint(&state, (uintptr_t)va_arg(ap, unsigned int), 16);
        break;
      case 'p':
        out_str(&state, "0x");
        out_uint(&state, (uintptr_t)va_arg(ap, void *), 16);
        break;
      default:
        out_char(&state, '%');
        out_char(&state, *p);
        break;
    }
  }

  if (state.size > 0) {
    size_t idx = (state.len < state.size - 1) ? state.len : state.size - 1;
    state.buf[idx] = '\0';
  }
  return (int)state.len;
}

int snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return ret;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
  return vsnprintf(out, (size_t)-1, fmt, ap);
}

int sprintf(char *out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int ret = vsnprintf(out, (size_t)-1, fmt, ap);
  va_end(ap);
  return ret;
}

int printf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  for (int i = 0; buf[i]; i++) {
    putch(buf[i]);
  }
  return ret;
}

#endif
