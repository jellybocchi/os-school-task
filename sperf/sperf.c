#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
enum { 
  MAX_SYSCALLS = 1024, 
  NAME_LEN = 64, 
  TOPK = 5 
};

typedef struct {
  char name[NAME_LEN];
  double total_sec;
} syscall_stat_t;

static bool is_executable_file(const char *path) {
  struct stat st;
  if(path == NULL || access(path, X_OK) != 0) {
    return false;
  }
  if(stat(path, &st) != 0) {
    return false;
  }
  return S_ISREG(st.st_mode);
}

static char *search_in_path(const char *name) {
  if(name == NULL) {
    return NULL;
  }
  if(name[0] == '/') {
    return is_executable_file(name) ? strdup(name) : NULL;
  }

  const char *path_env = getenv("PATH");
  if(path_env == NULL || path_env[0] == '\0') {
    path_env = "/bin:/usr/bin";
  }

  size_t env_len = strlen(path_env);
  char *copy = malloc(env_len + 1);
  if(copy == NULL) {
    return NULL;
  }
  memcpy(copy, path_env, env_len + 1);

  char *save = NULL;
  for(char *dir = strtok_r(copy, ":", &save); dir != NULL;
       dir = strtok_r(NULL, ":", &save)) {
    if(dir[0] == '\0') {
      dir = ".";
    }
    size_t needed = strlen(dir) + 1 + strlen(name) + 1;
    char *candidate = malloc(needed);
    if(candidate == NULL) {
      free(copy);
      return NULL;
    }
    snprintf(candidate, needed, "%s/%s", dir, name);
    if(is_executable_file(candidate)) {
      free(copy);
      return candidate;
    }
    free(candidate);
  }

  free(copy);
  return NULL;
}

static int find_or_add_stat(syscall_stat_t stats[], int *n, const char *name) {
  for(int i = 0; i < *n; i++) {
    if(strcmp(stats[i].name, name) == 0) {
      return i;
    }
  }
  if(*n >= MAX_SYSCALLS) {
    return -1;
  }
  snprintf(stats[*n].name, NAME_LEN, "%s", name);
  stats[*n].total_sec = 0.0;
  (*n)++;
  return (*n) - 1;
}

static bool parse_syscall_line(const char *line, char syscall_name[NAME_LEN],
                               double *sec) {
  const char *p = line;
  while(*p == ' ' || *p == '\t') {
    p++;
  }
  if(!islower((unsigned char)*p)) {
    return false;
  }

  const char *lp = strchr(p, '(');
  if(lp == NULL) {
    return false;
  }

  size_t name_len = (size_t)(lp - p);
  if(name_len == 0 || name_len >= NAME_LEN) {
    return false;
  }
  for(size_t i = 0; i < name_len; i++) {
    unsigned char c = (unsigned char)p[i];
    if(!(islower(c) || isdigit(c) || c == '_')) {
      return false;
    }
  }
  memcpy(syscall_name, p, name_len);
  syscall_name[name_len] = '\0';

  size_t len = strlen(line);
  while(len > 0 && isspace((unsigned char)line[len - 1])) {
    len--;
  }
  if(len == 0 || line[len - 1] != '>') {
    return false;
  }
  const char *gt = line + len - 1;
  const char *lt = gt;
  while(lt > line && *lt != '<') {
    lt--;
  }
  if(*lt != '<' || lt + 1 >= gt) {
    return false;
  }

  size_t num_len = (size_t)(gt - lt - 1);
  if(num_len >= 64) {
    return false;
  }

  char numbuf[64];
  memcpy(numbuf, lt + 1, num_len);
  numbuf[num_len] = '\0';

  errno = 0;
  char *endptr = NULL;
  double value = strtod(numbuf, &endptr);
  if(errno != 0 || endptr == NULL || *endptr != '\0' || value < 0.0) {
    return false;
  }

  *sec = value;
  return true;
}

static long long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void print_report(syscall_stat_t stats[], int n) {
  double total = 0.0;
  for(int i = 0; i < n; i++) {
    total += stats[i].total_sec;
  }
  if(total <= 0.0) {
    return;
  }

  bool picked[MAX_SYSCALLS] = {0};
  for(int k = 0; k < TOPK; k++) {
    int best = -1;
    for(int i = 0; i < n; i++) {
      if(picked[i]) {
        continue;
      }
      if(best == -1 || stats[i].total_sec > stats[best].total_sec) {
        best = i;
      }
    }
    if(best == -1 || stats[best].total_sec <= 0.0) {
      break;
    }
    picked[best] = true;
    int ratio = (int)(stats[best].total_sec * 100.0 / total + 0.5);
    if(ratio < 0) {
      ratio = 0;
    }
    if(ratio > 100) {
      ratio = 100;
    }
    printf("%s (%d%%)\n", stats[best].name, ratio);
  }

  char sep[80] = {0};
  fwrite(sep, 1, sizeof(sep), stdout);
  fflush(stdout);
}

int main(int argc, char *argv[]) {
  if(argc < 2) {
    fprintf(stderr, "Usage: %s COMMAND [ARG]...\n", argv[0]);
    return 1;
  }

  char *cmd_path = search_in_path(argv[1]);
  if(cmd_path == NULL) {
    fprintf(stderr, "%s: command not found or not executable: %s\n", argv[0],
            argv[1]);
    return 1;
  }

  char *strace_path = search_in_path("strace");
  if(strace_path == NULL) {
    fprintf(stderr, "%s: cannot find executable strace in PATH\n", argv[0]);
    free(cmd_path);
    return 1;
  }

  int pipefd[2];
  if(pipe(pipefd) != 0) {
    perror("pipe");
    free(cmd_path);
    free(strace_path);
    return 1;
  }

  pid_t pid = fork();
  if(pid < 0) {
    perror("fork");
    close(pipefd[0]);
    close(pipefd[1]);
    free(cmd_path);
    free(strace_path);
    return 1;
  }

  if(pid == 0) {
    close(pipefd[0]);
    if(pipefd[1] != 3) {
      if(dup2(pipefd[1], 3) < 0) {
        perror("dup2");
        _exit(127);
      }
      close(pipefd[1]);
    }

    int devnull = open("/dev/null", O_WRONLY);
    if(devnull >= 0) {
      if(dup2(devnull, STDOUT_FILENO) < 0 || dup2(devnull, STDERR_FILENO) < 0) {
        perror("dup2");
        _exit(127);
      }
      if(devnull > STDERR_FILENO) {
        close(devnull);
      }
    }

    char **child_argv = calloc((size_t)argc + 4, sizeof(char *));
    if(child_argv == NULL) {
      perror("calloc");
      _exit(127);
    }
    child_argv[0] = "strace";
    child_argv[1] = "-T";
    child_argv[2] = "-o";
    child_argv[3] = "/proc/self/fd/3";
    child_argv[4] = cmd_path;
    for(int i = 2; i < argc; i++) {
      child_argv[i + 3] = argv[i];
    }
    child_argv[argc + 3] = NULL;

    execve(strace_path, child_argv, environ);
    perror("execve");
    _exit(127);
  }

  close(pipefd[1]);
  free(cmd_path);
  free(strace_path);

  FILE *fp = fdopen(pipefd[0], "r");
  if(fp == NULL) {
    perror("fdopen");
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    return 1;
  }

  syscall_stat_t stats[MAX_SYSCALLS];
  int nstats = 0;
  char *line = NULL;
  size_t cap = 0;
  bool dirty = false;
  bool printed_once = false;
  long long last_print = now_ns();
  const long long interval = 100000000LL;  

  while(getline(&line, &cap, fp) != -1) {
    char name[NAME_LEN];
    double sec = 0.0;
    if(parse_syscall_line(line, name, &sec)) {
      int idx = find_or_add_stat(stats, &nstats, name);
      if(idx >= 0) {
        stats[idx].total_sec += sec;
        dirty = true;
      }
    }

    long long t = now_ns();
    if(dirty && t - last_print >= interval) {
      print_report(stats, nstats);
      last_print = t;
      dirty = false;
      printed_once = true;
    }
  }

  free(line);
  fclose(fp);
  waitpid(pid, NULL, 0);

  if(nstats > 0 && (dirty || !printed_once)) {
    print_report(stats, nstats);
  }

  return 0;
}
