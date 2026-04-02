#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int expr_id = 0;
static char *func_defs = NULL;
static size_t func_defs_len = 0;
static size_t func_defs_cap = 0;

static int starts_with_int(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return strncmp(s, "int", 3) == 0;
}

static void strip_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t k = write(fd, buf, n);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += (size_t)k;
        n -= (size_t)k;
    }
    return 0;
}

static int compile_source_to_exe(const char *source, char *exe_path, size_t exe_path_sz) {
    char ctmpl[] = "/tmp/crepl-src-XXXXXX.c";
    int cfd = mkstemps(ctmpl, 2);
    if (cfd < 0) return -1;

    if (write_all(cfd, source, strlen(source)) != 0) {
        close(cfd);
        unlink(ctmpl);
        return -1;
    }
    close(cfd);

    char btmpl[] = "/tmp/crepl-bin-XXXXXX";
    int bfd = mkstemp(btmpl);
    if (bfd < 0) {
        unlink(ctmpl);
        return -1;
    }
    close(bfd);

    pid_t pid = fork();
    if (pid < 0) {
        unlink(ctmpl);
        unlink(btmpl);
        return -1;
    }

    if (pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        char *argv[] = {
            "gcc",
            "-x", "c",
            "-Wno-implicit-function-declaration",
#if __x86_64__
            "-m64",
#else
            "-m32",
#endif
            ctmpl,
            "-o", btmpl,
            NULL
        };
        execvp("gcc", argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        unlink(ctmpl);
        unlink(btmpl);
        return -1;
    }
    unlink(ctmpl);

    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        unlink(btmpl);
        return -1;
    }

    if (strlen(btmpl) + 1 > exe_path_sz) {
        unlink(btmpl);
        return -1;
    }
    strcpy(exe_path, btmpl);
    return 0;
}

static int append_function_line(const char *line) {
    size_t n = strlen(line);
    size_t need = func_defs_len + n + 2;  // '\n' + '\0'
    if (need > func_defs_cap) {
        size_t new_cap = func_defs_cap ? func_defs_cap : 256;
        while (new_cap < need) new_cap *= 2;
        char *p = (char *)realloc(func_defs, new_cap);
        if (!p) return -1;
        func_defs = p;
        func_defs_cap = new_cap;
    }
    memcpy(func_defs + func_defs_len, line, n);
    func_defs_len += n;
    func_defs[func_defs_len++] = '\n';
    func_defs[func_defs_len] = '\0';
    return 0;
}

static int eval_expr_line(const char *line, int *value) {
    char wrapper_name[64];
    snprintf(wrapper_name, sizeof(wrapper_name), "__expr_wrapper_%d", expr_id++);

    size_t wrap_need = strlen(wrapper_name) * 2 + strlen(line) + 160;
    size_t src_need = func_defs_len + wrap_need + 1;
    char *src = (char *)malloc(src_need);
    if (!src) return -1;

    size_t off = 0;
    if (func_defs_len > 0) {
        memcpy(src + off, func_defs, func_defs_len);
        off += func_defs_len;
    }
    int n = snprintf(
        src + off, src_need - off,
        "#include <stdio.h>\nint %s(void) { return (%s); }\nint main(void) { printf(\"%%d\\n\", %s()); return 0; }\n",
        wrapper_name, line, wrapper_name
    );
    if (n < 0 || (size_t)n >= src_need - off) {
        free(src);
        return -1;
    }

    char exe_path[256];
    if (compile_source_to_exe(src, exe_path, sizeof(exe_path)) != 0) {
        free(src);
        return -1;
    }
    free(src);

    int pfd[2];
    if (pipe(pfd) != 0) {
        unlink(exe_path);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        unlink(exe_path);
        return -1;
    }

    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        char *argv[] = { exe_path, NULL };
        execv(exe_path, argv);
        _exit(127);
    }

    close(pfd[1]);
    char outbuf[128];
    ssize_t rn = read(pfd[0], outbuf, sizeof(outbuf) - 1);
    close(pfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        unlink(exe_path);
        return -1;
    }
    unlink(exe_path);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) return -1;
    if (rn <= 0) return -1;
    outbuf[rn] = '\0';
    char *end = NULL;
    long v = strtol(outbuf, &end, 10);
    if (end == outbuf) return -1;
    *value = (int)v;
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    static char line[4096];

    while (1) {
        printf("crepl> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        strip_newline(line);
        if (line[0] == '\0') continue;

        if (starts_with_int(line)) {
            if (append_function_line(line) == 0) {
                printf("OK.\n");
            } else {
                printf("ERR.\n");
            }
            fflush(stdout);
        } else {
            int v = 0;
            if (eval_expr_line(line, &v) == 0) {
                printf("= %d.\n", v);
            } else {
                printf("ERR.\n");
            }
            fflush(stdout);
        }
    }

    free(func_defs);
    return 0;
}
