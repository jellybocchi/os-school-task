#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define INITIAL_PROC_CAP 256
#define INITIAL_CHILD_CAP 4
#define VERSION_STRING "pstree (os-school-task) 1.0\n"

typedef struct {
  int pid;
  int ppid;
  char *name;
  int *children;
  size_t child_cnt;
  size_t child_cap;
} Process;

typedef struct {
  int pid;
  size_t idx;
} PidIndex;

static const Process *g_cmp_procs = NULL;

static bool is_all_digits(const char *s) {
  if (*s == '\0') {
    return false;
  }
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (!isdigit(*p)) {
      return false;
    }
  }
  return true;
}

static char *dupstr(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (!p) {
    return NULL;
  }
  memcpy(p, s, n);
  return p;
}

static int parse_status_file(int pid, int *ppid, char **name_out) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", pid);

  FILE *fp = fopen(path, "r");
  if (!fp) {
    return -1;
  }

  int got_ppid = 0;
  int got_name = 0;
  int parent = -1;
  char name_buf[256];
  name_buf[0] = '\0';

  char line[512];
  while (fgets(line, sizeof(line), fp)) {
    if (!got_name && strncmp(line, "Name:", 5) == 0) {
      char *v = line + 5;
      while (*v == ' ' || *v == '\t') v++;
      size_t len = strcspn(v, "\r\n");
      if (len >= sizeof(name_buf)) len = sizeof(name_buf) - 1;
      memcpy(name_buf, v, len);
      name_buf[len] = '\0';
      got_name = 1;
    } else if (!got_ppid && strncmp(line, "PPid:", 5) == 0) {
      char *v = line + 5;
      while (*v == ' ' || *v == '\t') v++;
      char *end = NULL;
      long x = strtol(v, &end, 10);
      if (end != v && x >= INT_MIN && x <= INT_MAX) {
        parent = (int)x;
        got_ppid = 1;
      }
    }

    if (got_name && got_ppid) {
      break;
    }
  }

  fclose(fp);

  if (!got_name || !got_ppid) {
    return -1;
  }

  char *name = dupstr(name_buf);
  if (!name) {
    return -1;
  }

  *ppid = parent;
  *name_out = name;
  return 0;
}

static int push_process(Process **procs, size_t *cnt, size_t *cap, int pid, int ppid, char *name) {
  if (*cnt == *cap) {
    size_t new_cap = (*cap == 0 ? INITIAL_PROC_CAP : (*cap * 2));
    Process *new_arr = (Process *)realloc(*procs, new_cap * sizeof(Process));
    if (!new_arr) {
      return -1;
    }
    *procs = new_arr;
    *cap = new_cap;
  }

  Process *p = &(*procs)[(*cnt)++];
  p->pid = pid;
  p->ppid = ppid;
  p->name = name;
  p->children = NULL;
  p->child_cnt = 0;
  p->child_cap = 0;
  return 0;
}

static int add_child(Process *parent, int child_idx) {
  if (parent->child_cnt == parent->child_cap) {
    size_t new_cap = (parent->child_cap == 0 ? INITIAL_CHILD_CAP : parent->child_cap * 2);
    int *new_arr = (int *)realloc(parent->children, new_cap * sizeof(int));
    if (!new_arr) {
      return -1;
    }
    parent->children = new_arr;
    parent->child_cap = new_cap;
  }
  parent->children[parent->child_cnt++] = child_idx;
  return 0;
}

static int cmp_pid_index(const void *a, const void *b) {
  const PidIndex *pa = (const PidIndex *)a;
  const PidIndex *pb = (const PidIndex *)b;
  if (pa->pid < pb->pid) return -1;
  if (pa->pid > pb->pid) return 1;
  return 0;
}

static int cmp_child_by_pid(const void *a, const void *b) {
  const Process *procs = g_cmp_procs;
  int ia = *(const int *)a;
  int ib = *(const int *)b;
  if (procs[ia].pid < procs[ib].pid) return -1;
  if (procs[ia].pid > procs[ib].pid) return 1;
  return 0;
}

static ssize_t find_by_pid(const PidIndex *map, size_t n, int pid) {
  size_t l = 0, r = n;
  while (l < r) {
    size_t m = l + (r - l) / 2;
    if (map[m].pid == pid) return (ssize_t)map[m].idx;
    if (map[m].pid < pid) l = m + 1;
    else r = m;
  }
  return -1;
}

static void print_tree(const Process *procs, int idx, int depth, bool show_pids) {
  for (int i = 0; i < depth; i++) {
    fputs("  ", stdout);
  }
  if (show_pids) {
    printf("%s(%d)\n", procs[idx].name, procs[idx].pid);
  } else {
    printf("%s\n", procs[idx].name);
  }

  for (size_t i = 0; i < procs[idx].child_cnt; i++) {
    print_tree(procs, procs[idx].children[i], depth + 1, show_pids);
  }
}

static void free_all(Process *procs, size_t n, int *roots) {
  if (procs) {
    for (size_t i = 0; i < n; i++) {
      free(procs[i].name);
      free(procs[i].children);
    }
  }
  free(procs);
  free(roots);
}

int main(int argc, char *argv[]) {
  bool show_pids = false;
  bool numeric_sort = false;
  bool show_version = false;

  static struct option long_opts[] = {
    {"show-pids",    no_argument, 0, 'p'},
    {"numeric-sort", no_argument, 0, 'n'},
    {"version",      no_argument, 0, 'V'},
    {0, 0, 0, 0}
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "pnV", long_opts, NULL)) != -1) {
    switch (opt) {
      case 'p': show_pids = true; break;
      case 'n': numeric_sort = true; break;
      case 'V': show_version = true; break;
      default:
        fprintf(stderr, "usage: %s [-p|--show-pids] [-n|--numeric-sort] [-V|--version]\n", argv[0]);
        return EXIT_FAILURE;
    }
  }

  if (optind < argc) {
    fprintf(stderr, "usage: %s [-p|--show-pids] [-n|--numeric-sort] [-V|--version]\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (show_version) {
    fputs(VERSION_STRING, stderr);
    return EXIT_SUCCESS;
  }

  DIR *dir = opendir("/proc");
  if (!dir) {
    return EXIT_FAILURE;
  }

  Process *procs = NULL;
  size_t proc_cnt = 0;
  size_t proc_cap = 0;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (!is_all_digits(ent->d_name)) {
      continue;
    }

    char *end = NULL;
    long pid_long = strtol(ent->d_name, &end, 10);
    if (end == ent->d_name || *end != '\0' || pid_long <= 0 || pid_long > INT_MAX) {
      continue;
    }
    int pid = (int)pid_long;

    int ppid = -1;
    char *name = NULL;
    if (parse_status_file(pid, &ppid, &name) != 0) {
      continue;  
    }

    if (push_process(&procs, &proc_cnt, &proc_cap, pid, ppid, name) != 0) {
      free(name);
      closedir(dir);
      free_all(procs, proc_cnt, NULL);
      return EXIT_FAILURE;
    }
  }

  closedir(dir);

  PidIndex *map = NULL;
  if (proc_cnt > 0) {
    map = (PidIndex *)malloc(proc_cnt * sizeof(PidIndex));
    if (!map) {
      free_all(procs, proc_cnt, NULL);
      return EXIT_FAILURE;
    }

    for (size_t i = 0; i < proc_cnt; i++) {
      map[i].pid = procs[i].pid;
      map[i].idx = i;
    }
    qsort(map, proc_cnt, sizeof(PidIndex), cmp_pid_index);
  }

  int *roots = NULL;
  size_t roots_cnt = 0;
  size_t roots_cap = 0;

  for (size_t i = 0; i < proc_cnt; i++) {
    ssize_t parent_idx = find_by_pid(map, proc_cnt, procs[i].ppid);
    if (parent_idx < 0) {
      if (roots_cnt == roots_cap) {
        size_t new_cap = (roots_cap == 0 ? 8 : roots_cap * 2);
        int *new_arr = (int *)realloc(roots, new_cap * sizeof(int));
        if (!new_arr) {
          free(map);
          free_all(procs, proc_cnt, roots);
          return EXIT_FAILURE;
        }
        roots = new_arr;
        roots_cap = new_cap;
      }
      roots[roots_cnt++] = (int)i;
    } else {
      if (add_child(&procs[parent_idx], (int)i) != 0) {
        free(map);
        free_all(procs, proc_cnt, roots);
        return EXIT_FAILURE;
      }
    }
  }

  free(map);

  if (numeric_sort) {
    g_cmp_procs = procs;
    for (size_t i = 0; i < proc_cnt; i++) {
      if (procs[i].child_cnt > 1) {
        qsort(procs[i].children, procs[i].child_cnt, sizeof(int), cmp_child_by_pid);
      }
    }
    g_cmp_procs = NULL;
  }

  for (size_t i = 0; i < roots_cnt; i++) {
    print_tree(procs, roots[i], 0, show_pids);
  }

  free_all(procs, proc_cnt, roots);
  return EXIT_SUCCESS;
}
