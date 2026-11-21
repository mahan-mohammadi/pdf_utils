#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <limits.h>
#include <stdint.h>
#include <libgen.h>
#include <getopt.h>
#include <glob.h>
#include <ftw.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define PROG         "pdfcrop"
#define VERSION      "3.0.0"
#define MAX_PAGES    1000000
#define MAX_EXPR     100000
#define MAX_PASSWORD 4096
#define MAX_REPL     64
#define DIAG_SIZE    8192
#define MAX_TMP      128

enum { EXIT_OK = 0, EXIT_ERR = 1, EXIT_USAGE = 2 };

static int g_quiet, g_verbose, g_color, g_say_stderr, g_noprompt;
static struct termios g_term_saved;
static volatile sig_atomic_t g_term_active;

static struct {
    char path[PATH_MAX];
    int dir;
} g_tmp[MAX_TMP];

static char g_pw_path[PATH_MAX];
static char g_pw_buf[MAX_PASSWORD];
static int  g_pw_have;
static char g_upw[MAX_PASSWORD];
static char g_opw[MAX_PASSWORD];
static int  g_npw_have;

static void say(const char *fmt, ...)
{
    if (g_quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_say_stderr ? stderr : stdout, fmt, ap);
    va_end(ap);
}

static void trace(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  > ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void warn(const char *fmt, ...)
{
    if (g_quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s" PROG ": warning:%s ",
            g_color ? "\033[1;33m" : "", g_color ? "\033[0m" : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s" PROG ": error:%s ",
            g_color ? "\033[1;31m" : "", g_color ? "\033[0m" : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *tmp_base(void)
{
    const char *t = getenv("TMPDIR");
    return (t && *t) ? t : "/tmp";
}

static int rm_cb(const char *p, const struct stat *sb, int flag,
                 struct FTW *ftw)
{
    (void)sb; (void)flag; (void)ftw;
    remove(p);
    return 0;
}

static void tmp_release(const char *path)
{
    for (int i = 0; i < MAX_TMP; i++) {
        if (g_tmp[i].path[0] && (g_tmp[i].path == path ||
                                 strcmp(g_tmp[i].path, path) == 0)) {
            if (g_tmp[i].dir)
                nftw(g_tmp[i].path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
            else
                unlink(g_tmp[i].path);
            g_tmp[i].path[0] = '\0';
            return;
        }
    }
}

static void tmp_forget(const char *path)
{
    for (int i = 0; i < MAX_TMP; i++) {
        if (g_tmp[i].path[0] && (g_tmp[i].path == path ||
                                 strcmp(g_tmp[i].path, path) == 0)) {
            g_tmp[i].path[0] = '\0';
            return;
        }
    }
}

static void tmp_release_all(void)
{
    for (int i = 0; i < MAX_TMP; i++) {
        if (!g_tmp[i].path[0])
            continue;
        if (g_tmp[i].dir)
            nftw(g_tmp[i].path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        else
            unlink(g_tmp[i].path);
        g_tmp[i].path[0] = '\0';
    }
}

static char *tmp_slot(int dir)
{
    for (int i = 0; i < MAX_TMP; i++) {
        if (!g_tmp[i].path[0]) {
            g_tmp[i].dir = dir;
            return g_tmp[i].path;
        }
    }
    err("too many temporary files");
    return NULL;
}

static char *tmp_file(const char *dir, const char *prefix, const char *suffix)
{
    char *slot = tmp_slot(0);
    if (!slot)
        return NULL;

    if (snprintf(slot, PATH_MAX, "%s/%s-XXXXXX%s", dir, prefix, suffix)
        >= PATH_MAX) {
        slot[0] = '\0';
        err("temporary path is too long");
        return NULL;
    }

    int fd = mkstemps(slot, (int)strlen(suffix));
    if (fd < 0) {
        err("cannot create temporary file in '%s': %s", dir, strerror(errno));
        slot[0] = '\0';
        return NULL;
    }

    close(fd);
    return slot;
}

static char *tmp_dir(const char *parent, const char *prefix)
{
    char *slot = tmp_slot(1);
    if (!slot)
        return NULL;

    if (snprintf(slot, PATH_MAX, "%s/%s-XXXXXX", parent, prefix) >= PATH_MAX ||
        !mkdtemp(slot)) {
        err("cannot create temporary directory in '%s': %s",
            parent, strerror(errno));
        slot[0] = '\0';
        return NULL;
    }

    return slot;
}

static void cleanup(void)
{
    tmp_release_all();
    if (g_pw_path[0]) {
        unlink(g_pw_path);
        g_pw_path[0] = '\0';
    }
    memset(g_pw_buf, 0, sizeof(g_pw_buf));
    memset(g_upw, 0, sizeof(g_upw));
    memset(g_opw, 0, sizeof(g_opw));
}

static void on_signal(int sig)
{
    if (g_term_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_term_saved);
    cleanup();
    _exit(128 + sig);
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

typedef struct {
    char *v[4096];
    int n;
    char arena[32768];
    size_t used;
} Cmd;

static void cmd_init(Cmd *c)
{
    c->n = 0;
    c->v[0] = NULL;
    c->used = 0;
}

static void cmd_add(Cmd *c, const char *arg)
{
    if (c->n >= 4095) {
        err("too many command-line arguments");
        exit(EXIT_ERR);
    }
    c->v[c->n++] = (char *)arg;
    c->v[c->n] = NULL;
}

static void cmd_addf(Cmd *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(c->arena + c->used, sizeof(c->arena) - c->used, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(c->arena) - c->used) {
        err("internal error: argument buffer full");
        exit(EXIT_ERR);
    }

    cmd_add(c, c->arena + c->used);
    c->used += (size_t)n + 1;
}

static void trace_cmd(const Cmd *c)
{
    if (!g_verbose)
        return;
    fprintf(stderr, "  > run:");
    for (int i = 0; i < c->n; i++)
        fprintf(stderr, " %s", c->v[i]);
    fputc('\n', stderr);
}

static int tool_exists(const char *name)
{
    if (strchr(name, '/'))
        return access(name, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";

    char *copy = strdup(path);
    if (!copy)
        return 0;

    int found = 0;
    char *save = NULL;

    for (char *d = strtok_r(copy, ":", &save); d && !found;
         d = strtok_r(NULL, ":", &save)) {
        char full[PATH_MAX];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", *d ? d : ".", name);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
            access(full, X_OK) == 0)
            found = 1;
    }

    free(copy);
    return found;
}

static int need_tool(const char *name, const char *hint)
{
    if (tool_exists(name))
        return 1;
    err("'%s' is not installed or not in PATH (%s)", name, hint);
    return 0;
}

static int execute(Cmd *c, char *diag, size_t dsz)
{
    int pipes[2];

    trace_cmd(c);

    if (diag && dsz)
        diag[0] = '\0';

    if (pipe(pipes) != 0) {
        perror(PROG ": pipe");
        return -1;
    }

    fflush(NULL);
    pid_t pid = fork();

    if (pid < 0) {
        perror(PROG ": fork");
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipes[0]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);

        execvp(c->v[0], c->v);

        dprintf(STDERR_FILENO, "cannot execute %s: %s\n",
                c->v[0], strerror(errno));
        _exit(127);
    }

    close(pipes[1]);

    size_t used = 0;
    char buffer[1024];

    for (;;) {
        ssize_t n = read(pipes[0], buffer, sizeof(buffer));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;

        if (diag && dsz > 0 && used < dsz - 1) {
            size_t remaining = dsz - used - 1;
            size_t copy = (size_t)n < remaining ? (size_t)n : remaining;
            memcpy(diag + used, buffer, copy);
            used += copy;
            diag[used] = '\0';
        }
    }

    close(pipes[0]);

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int execute_inherit(Cmd *c)
{
    trace_cmd(c);
    fflush(NULL);

    pid_t pid = fork();

    if (pid < 0) {
        perror(PROG ": fork");
        return -1;
    }

    if (pid == 0) {
        execvp(c->v[0], c->v);
        dprintf(STDERR_FILENO, "cannot execute %s: %s\n",
                c->v[0], strerror(errno));
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';

    return s;
}

static int has_ext(const char *s, const char *ext)
{
    size_t n = strlen(s), e = strlen(ext);
    return n > e && s[n - e - 1] == '.' && strcasecmp(s + n - e, ext) == 0;
}

static void file_stem(const char *path, char *out, size_t sz)
{
    char t[PATH_MAX];
    snprintf(t, sizeof(t), "%s", path);

    char *b = basename(t);
    size_t n = strlen(b);

    if (n > 4 && strcasecmp(b + n - 4, ".pdf") == 0)
        n -= 4;

    snprintf(out, sz, "%.*s", (int)n, b);
}

static void path_dir(const char *path, char *out, size_t sz)
{
    char t[PATH_MAX];
    snprintf(t, sizeof(t), "%s", path);
    snprintf(out, sz, "%s", dirname(t));
}

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST)
                return 0;
            *p = '/';
        }
    }

    return mkdir(buf, 0777) == 0 || errno == EEXIST;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void fmt_size(double bytes, char *out, size_t sz)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    int i = 0;

    while (bytes >= 1024 && i < 3) {
        bytes /= 1024;
        i++;
    }

    if (i)
        snprintf(out, sz, "%.1f %s", bytes, units[i]);
    else
        snprintf(out, sz, "%.0f %s", bytes, units[i]);
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        err("cannot read '%s': %s", path, strerror(errno));
        return NULL;
    }

    size_t cap = 65536, n = 0;
    unsigned char *buf = malloc(cap);

    while (buf) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap)
            break;
        cap *= 2;
        unsigned char *nb = realloc(buf, cap);
        if (!nb) {
            free(buf);
            buf = NULL;
        } else {
            buf = nb;
        }
    }

    int bad = ferror(f);
    fclose(f);

    if (!buf || bad) {
        free(buf);
        err("cannot read '%s'", path);
        return NULL;
    }

    *len = n;
    return buf;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        err("cannot write '%s': %s", path, strerror(errno));
        return 0;
    }

    int ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;

    if (!ok)
        err("write error on '%s'", path);

    return ok;
}

static int read_first_line(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        err("cannot read '%s': %s", path, strerror(errno));
        return 0;
    }

    char *r = fgets(buf, (int)sz, f);
    fclose(f);

    if (!r) {
        err("'%s' is empty", path);
        return 0;
    }

    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

static int copy_to_stdout(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        err("cannot read '%s': %s", path, strerror(errno));
        return 0;
    }

    char buf[8192];

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            err("read error: %s", strerror(errno));
            close(fd);
            return 0;
        }
        if (n == 0)
            break;
        if (!write_all(STDOUT_FILENO, buf, (size_t)n)) {
            if (errno != EPIPE)
                err("write error: %s", strerror(errno));
            close(fd);
            return 0;
        }
    }

    close(fd);
    return 1;
}

typedef struct {
    const char *output, *outdir, *suffix, *from_list;
    const char *pw_file, *pw_env;
    int pw_stdin;
    int force, dry_run, in_place, jobs, keep_going, verify;
    const char *pages;
    int text, layout, nopgbrk, linearize, info;
    const char *level;
    int dpi, grayscale;
    const char *to;
    int quality;
    const char *upw_file, *upw_env, *opw_file, *opw_env, *deny;
    int bits;
    const char *repl[MAX_REPL];
    int nrepl;
    const char *lang, *engine, *sidecar;
    int deskew, rotate, force_ocr;
    const char *st_text, *st_image, *st_pdf, *st_color, *st_pos, *st_on, *st_layer;
    double st_size, st_angle, st_opacity, st_scale;
    const char *cert, *cert_pass_file, *cert_pass_env;
    const char *reason, *location, *field;
    int split_every;
    const char *split_ranges;
    const char *num_format;
    int num_start;
    const char *margins;
} Opts;

typedef struct {
    int *data;
    size_t count;
    size_t capacity;
} PageList;

static void pages_free(PageList *p)
{
    free(p->data);
    memset(p, 0, sizeof(*p));
}

static int pages_add(PageList *p, int page)
{
    if (p->count >= MAX_PAGES) {
        err("page selection is too large (limit %d pages)", MAX_PAGES);
        return 0;
    }

    if (p->count == p->capacity) {
        size_t cap = p->capacity ? p->capacity * 2 : 128;
        if (cap > MAX_PAGES)
            cap = MAX_PAGES;

        int *tmp = realloc(p->data, cap * sizeof(int));
        if (!tmp) {
            perror(PROG ": memory allocation");
            return 0;
        }
        p->data = tmp;
        p->capacity = cap;
    }

    p->data[p->count++] = page;
    return 1;
}

static int parse_page(const char *s, int total, int *value)
{
    if (!*s)
        return 0;

    if (strcasecmp(s, "last") == 0) {
        *value = total;
        return 1;
    }

    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!isdigit(*p))
            return 0;
    }

    errno = 0;
    char *end;
    long n = strtol(s, &end, 10);

    if (errno || *end || n < 1 || n > INT_MAX)
        return 0;

    *value = (int)n;
    return 1;
}

static int add_range(PageList *list, int first, int last, int total)
{
    if (first < 1 || last < 1 || first > total || last > total) {
        err("pages must be between 1 and %d", total);
        return 0;
    }

    int step = first <= last ? 1 : -1;

    for (int p = first;; p += step) {
        if (!pages_add(list, p))
            return 0;
        if (p == last)
            break;
    }

    return 1;
}

static int parse_selection(const char *input, int total, PageList *list)
{
    char *copy = strdup(input);
    if (!copy) {
        perror(PROG ": strdup");
        return 0;
    }

    char *save = NULL;
    char *token = strtok_r(copy, ",;\n", &save);
    int entries = 0;

    while (token) {
        char *item = trim(token);

        if (*item == '\0') {
            err("empty page-selection entry");
            goto error;
        }

        entries++;

        if (strcasecmp(item, "all") == 0) {
            if (!add_range(list, 1, total, total))
                goto error;

        } else if (strcasecmp(item, "odd") == 0 ||
                   strcasecmp(item, "even") == 0) {
            int parity = strcasecmp(item, "odd") == 0 ? 1 : 0;

            for (int p = 1; p <= total; p++) {
                if (p % 2 == parity && !pages_add(list, p))
                    goto error;
            }

        } else {
            char *dash = strchr(item, '-');

            if (dash) {
                if (strchr(dash + 1, '-')) {
                    err("invalid range '%s'", item);
                    goto error;
                }

                *dash = '\0';

                int first, last;
                char *a = trim(item);
                char *b = trim(dash + 1);

                if (!parse_page(a, total, &first) ||
                    !parse_page(b, total, &last)) {
                    err("invalid range '%s-%s'", a, b);
                    goto error;
                }

                if (!add_range(list, first, last, total))
                    goto error;

            } else {
                int page;

                if (!parse_page(item, total, &page)) {
                    err("invalid page expression '%s'", item);
                    goto error;
                }

                if (!add_range(list, page, page, total))
                    goto error;
            }
        }

        token = strtok_r(NULL, ",;\n", &save);
    }

    free(copy);

    if (!entries || !list->count) {
        err("no pages selected");
        return 0;
    }

    return 1;

error:
    free(copy);
    return 0;
}

static char *build_expression(const PageList *list)
{
    size_t capacity = list->count * 24 + 1;
    char *buf = malloc(capacity);
    if (!buf)
        return NULL;

    size_t used = 0, i = 0;

    while (i < list->count) {
        size_t j = i;
        while (j + 1 < list->count && list->data[j + 1] == list->data[j] + 1)
            j++;

        int n;
        if (j > i)
            n = snprintf(buf + used, capacity - used, "%s%d-%d",
                         used ? "," : "", list->data[i], list->data[j]);
        else
            n = snprintf(buf + used, capacity - used, "%s%d",
                         used ? "," : "", list->data[i]);

        if (n < 0 || (size_t)n >= capacity - used) {
            free(buf);
            return NULL;
        }

        used += (size_t)n;
        i = j + 1;
    }

    return buf;
}

static char *build_set_expression(const PageList *list, int total)
{
    char *mark = calloc((size_t)total + 2, 1);
    if (!mark)
        return NULL;

    for (size_t i = 0; i < list->count; i++)
        mark[list->data[i]] = 1;

    PageList asc = {0};

    for (int p = 1; p <= total; p++) {
        if (mark[p] && !pages_add(&asc, p)) {
            free(mark);
            pages_free(&asc);
            return NULL;
        }
    }

    free(mark);
    char *expr = build_expression(&asc);
    pages_free(&asc);
    return expr;
}

static int prompt_secret(const char *label, char *buf, size_t size)
{
    struct termios hidden;

    if (g_noprompt || !isatty(STDIN_FILENO) || !isatty(STDERR_FILENO))
        return 0;

    if (tcgetattr(STDIN_FILENO, &g_term_saved) != 0)
        return 0;

    hidden = g_term_saved;
    hidden.c_lflag &= ~ECHO;

    fprintf(stderr, "%s", label);
    fflush(stderr);

    g_term_active = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
        g_term_active = 0;
        return 0;
    }

    char *r = fgets(buf, (int)size, stdin);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_term_saved);
    g_term_active = 0;
    fputc('\n', stderr);

    if (!r)
        return 0;

    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

static int acquire_password(const Opts *o)
{
    if (o->pw_file) {
        if (!read_first_line(o->pw_file, g_pw_buf, sizeof(g_pw_buf)))
            return 0;
    } else if (o->pw_env) {
        const char *v = getenv(o->pw_env);
        if (!v) {
            err("environment variable '%s' is not set", o->pw_env);
            return 0;
        }
        if (strlen(v) >= sizeof(g_pw_buf)) {
            err("password too long");
            return 0;
        }
        strcpy(g_pw_buf, v);
    } else if (o->pw_stdin) {
        if (!fgets(g_pw_buf, sizeof(g_pw_buf), stdin)) {
            err("could not read password from stdin");
            return 0;
        }
        g_pw_buf[strcspn(g_pw_buf, "\r\n")] = '\0';
    } else if (!prompt_secret("Enter PDF password: ", g_pw_buf,
                              sizeof(g_pw_buf))) {
        err("PDF is password protected; use --password-file, "
            "--password-env or --password-stdin");
        return 0;
    }

    g_pw_have = 1;
    return 1;
}

static int pw_write_file(void)
{
    if (strchr(g_pw_buf, '\n') || strchr(g_pw_buf, '\r')) {
        err("passwords containing newlines are not supported");
        return 0;
    }

    if (!g_pw_path[0]) {
        if (snprintf(g_pw_path, sizeof(g_pw_path), "%s/pdfcrop-pw-XXXXXX",
                     tmp_base()) >= (int)sizeof(g_pw_path)) {
            g_pw_path[0] = '\0';
            err("temporary directory path too long");
            return 0;
        }

        int fd = mkstemp(g_pw_path);
        if (fd < 0) {
            err("cannot create password file: %s", strerror(errno));
            g_pw_path[0] = '\0';
            return 0;
        }
        close(fd);
    }

    int fd = open(g_pw_path, O_WRONLY | O_TRUNC);
    if (fd < 0 || fchmod(fd, S_IRUSR | S_IWUSR) != 0 ||
        !write_all(fd, g_pw_buf, strlen(g_pw_buf)) || !write_all(fd, "\n", 1)) {
        if (fd >= 0)
            close(fd);
        err("cannot write password file");
        return 0;
    }

    close(fd);
    return 1;
}

static int requires_password(const char *file, const char *pwfile)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    if (pwfile)
        cmd_addf(&c, "--password-file=%s", pwfile);
    cmd_add(&c, "--requires-password");
    cmd_add(&c, file);
    return execute(&c, NULL, 0);
}

static int qpdf_page_count(const char *file, int *count)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--show-npages");
    cmd_add(&c, file);

    char output[512];

    if (execute(&c, output, sizeof(output)) != 0) {
        err("cannot read PDF: %s", output);
        return 0;
    }

    char *end;
    long value = strtol(output, &end, 10);

    if (end == output || value < 1 || value > INT_MAX) {
        err("invalid PDF page count");
        return 0;
    }

    *count = (int)value;
    return 1;
}

static int check_pdf(const char *file)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, "--check");
    cmd_add(&c, file);

    char output[DIAG_SIZE];

    if (execute(&c, output, sizeof(output)) != 0) {
        err("output verification failed:\n%s", output);
        return 0;
    }

    return 1;
}

static int prepare_input(const Opts *o, const char *in, char *out, size_t sz)
{
    int st = requires_password(in, NULL);

    if (st == 2) {
        snprintf(out, sz, "%s", in);
        return 1;
    }

    if (st < 0 || st == 1 || st > 3) {
        err("cannot inspect PDF encryption of '%s'", in);
        return 0;
    }

    const char *pwf = NULL;

    if (st == 0) {
        int explicit_src = o->pw_file || o->pw_env || o->pw_stdin;
        int ok = 0;

        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            if (!g_pw_have && !acquire_password(o))
                return 0;
            if (!pw_write_file())
                return 0;

            if (requires_password(in, g_pw_path) == 3) {
                ok = 1;
                break;
            }

            g_pw_have = 0;
            memset(g_pw_buf, 0, sizeof(g_pw_buf));

            if (explicit_src || g_noprompt)
                break;
        }

        if (!ok) {
            err("incorrect PDF password for '%s'", in);
            return 0;
        }

        pwf = g_pw_path;
    }

    char *tmp = tmp_file(tmp_base(), "pdfcrop-dec", ".pdf");
    if (!tmp)
        return 0;

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    if (pwf)
        cmd_addf(&c, "--password-file=%s", pwf);
    cmd_add(&c, "--decrypt");
    cmd_add(&c, in);
    cmd_add(&c, tmp);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("cannot decrypt '%s':\n%s", in, diag);
        return 0;
    }

    snprintf(out, sz, "%s", tmp);
    return 1;
}

static int resolve_output(const Opts *o, const char *input,
                          const char *def_suffix, const char *ext,
                          int append_ext, char *out, size_t sz)
{
    int n;

    if (o->in_place) {
        n = snprintf(out, sz, "%s", input);
    } else if (o->output) {
        int add = append_ext && ext[0] && !has_ext(o->output, ext);
        n = snprintf(out, sz, "%s%s%s", o->output,
                     add ? "." : "", add ? ext : "");
    } else {
        char stem[PATH_MAX];
        file_stem(input, stem, sizeof(stem));

        const char *suf = o->suffix ? o->suffix : def_suffix;

        if (o->outdir)
            n = snprintf(out, sz, "%s/%s%s.%s", o->outdir, stem, suf, ext);
        else
            n = snprintf(out, sz, "%s%s.%s", stem, suf, ext);
    }

    if (n < 0 || (size_t)n >= sz) {
        err("output path is too long");
        return 0;
    }

    return 1;
}

static int check_output(const Opts *o, const char *input, const char *out)
{
    struct stat os;

    if (stat(out, &os) != 0)
        return 1;

    if (!S_ISREG(os.st_mode)) {
        err("output '%s' exists and is not a regular file", out);
        return 0;
    }

    if (o->in_place)
        return 1;

    struct stat is;

    if (input && stat(input, &is) == 0 &&
        is.st_dev == os.st_dev && is.st_ino == os.st_ino) {
        err("input and output must be different files "
            "(use --in-place to replace the original)");
        return 0;
    }

    if (!o->force && !o->dry_run) {
        err("output '%s' already exists (use --force to overwrite)", out);
        return 0;
    }

    return 1;
}

static int publish_file(const char *tmp, const char *dest, int force)
{
    mode_t mask = umask(0);
    umask(mask);
    chmod(tmp, 0666 & ~mask);

    if (force) {
        if (rename(tmp, dest) != 0) {
            err("cannot write '%s': %s", dest, strerror(errno));
            return 0;
        }
    } else {
        if (link(tmp, dest) != 0) {
            err("cannot write '%s': %s", dest, strerror(errno));
            return 0;
        }
        unlink(tmp);
    }

    tmp_forget(tmp);
    return 1;
}

static int finalize_pdf(const Opts *o, const char *tmp, const char *out,
                        int expect_pages)
{
    if (o->verify) {
        if (!check_pdf(tmp))
            return 0;

        if (expect_pages > 0) {
            int n = 0;
            if (!qpdf_page_count(tmp, &n) || n != expect_pages) {
                err("output has %d pages, expected %d", n, expect_pages);
                return 0;
            }
        }
    }

    return publish_file(tmp, out, o->force || o->in_place);
}

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_cat(Buf *b, const void *d, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) {
            perror(PROG ": memory allocation");
            exit(EXIT_ERR);
        }
        b->p = np;
        b->cap = nc;
    }

    memcpy(b->p + b->len, d, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp))
        n = (int)sizeof(tmp) - 1;

    buf_cat(b, tmp, (size_t)n);
}

static void pdf_escape(Buf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '(' || s[i] == ')' || s[i] == '\\')
            buf_cat(b, "\\", 1);
        buf_cat(b, s + i, 1);
    }
}

typedef int (*PdfOp)(const Opts *o, const char *in, const char *out,
                     int *expect);

static int op_extract(const Opts *o, const char *in, const char *out,
                      int *expect)
{
    int total;
    if (!qpdf_page_count(in, &total))
        return 0;

    PageList pages = {0};
    char *expr = NULL;
    int rc = 0;

    if (!parse_selection(o->pages ? o->pages : "all", total, &pages))
        goto done;

    expr = build_expression(&pages);

    if (!expr || strlen(expr) > MAX_EXPR) {
        err("page selection is too complex; split it into several runs");
        goto done;
    }

    say("Pages: %zu of %d selected\n", pages.count, total);

    if (!out) {
        say("Dry run: qpdf page selection would be '%s'\n", expr);
        rc = 1;
        goto done;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    if (o->linearize)
        cmd_add(&c, "--linearize");
    cmd_add(&c, in);
    cmd_add(&c, "--pages");
    cmd_add(&c, ".");
    cmd_add(&c, expr);
    cmd_add(&c, "--");
    cmd_add(&c, out);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("extraction failed:\n%s", diag);
        goto done;
    }

    *expect = (int)pages.count;
    rc = 1;

done:
    free(expr);
    pages_free(&pages);
    return rc;
}

static int op_remove(const Opts *o, const char *in, const char *out,
                     int *expect)
{
    if (!o->pages) {
        err("remove requires page selection (e.g. 2,5-7)");
        return 0;
    }

    int total;
    if (!qpdf_page_count(in, &total))
        return 0;

    PageList del = {0}, keep = {0};
    char *mark = NULL, *expr = NULL;
    int rc = 0;

    if (!parse_selection(o->pages, total, &del))
        goto done;

    mark = calloc((size_t)total + 2, 1);
    if (!mark)
        goto done;

    for (size_t i = 0; i < del.count; i++)
        mark[del.data[i]] = 1;

    for (int p = 1; p <= total; p++) {
        if (!mark[p] && !pages_add(&keep, p))
            goto done;
    }

    if (keep.count == 0) {
        err("cannot remove all pages from '%s'", in);
        goto done;
    }

    expr = build_expression(&keep);
    if (!expr || strlen(expr) > MAX_EXPR) {
        err("page selection is too complex");
        goto done;
    }

    say("Keeping %zu of %d pages (removing %zu)\n",
        keep.count, total, (size_t)total - keep.count);

    if (!out) {
        say("Dry run: would keep pages '%s'\n", expr);
        rc = 1;
        goto done;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    if (o->linearize)
        cmd_add(&c, "--linearize");
    cmd_add(&c, in);
    cmd_add(&c, "--pages");
    cmd_add(&c, ".");
    cmd_add(&c, expr);
    cmd_add(&c, "--");
    cmd_add(&c, out);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("remove failed:\n%s", diag);
        goto done;
    }

    *expect = (int)keep.count;
    rc = 1;

done:
    free(expr);
    free(mark);
    pages_free(&del);
    pages_free(&keep);
    return rc;
}

static int op_rotate(const Opts *o, const char *in, const char *out,
                     int *expect)
{
    double raw = isnan(o->st_angle) ? 90.0 : o->st_angle;
    int deg = (int)llround(raw);
    if (fabs(raw - deg) > 1e-6 || deg % 90 != 0) {
        err("--angle for rotate must be a multiple of 90 (e.g. 90, 180, 270, -90)");
        return 0;
    }
    deg = ((deg % 360) + 360) % 360;
    if (deg == 0) {
        say("Angle is 0; nothing to rotate\n");
        return 2;
    }

    int total;
    if (!qpdf_page_count(in, &total))
        return 0;

    const char *spec = o->st_on ? o->st_on : (o->pages ? o->pages : "all");
    PageList pl = {0};
    char *expr = NULL;
    int rc = 0;

    if (!parse_selection(spec, total, &pl))
        goto done;

    expr = build_set_expression(&pl, total);
    if (!expr)
        goto done;

    say("Rotating pages %s of %d by +%d degrees\n", expr, total, deg);

    if (!out) {
        rc = 1;
        goto done;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    if (o->linearize)
        cmd_add(&c, "--linearize");
    cmd_add(&c, in);
    cmd_addf(&c, "--rotate=+%d:%s", deg, expr);
    cmd_add(&c, out);

    char diag[DIAG_SIZE];
    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("rotation failed:\n%s", diag);
        goto done;
    }

    *expect = total;
    rc = 1;

done:
    free(expr);
    pages_free(&pl);
    return rc;
}

static int op_compress(const Opts *o, const char *in, const char *out,
                       int *expect)
{
    (void)expect;

    const char *level = o->level ? o->level : "medium";
    const char *preset = NULL;
    int lossless = 0;

    if (strcmp(level, "lossless") == 0)
        lossless = 1;
    else if (strcmp(level, "low") == 0)
        preset = "/printer";
    else if (strcmp(level, "medium") == 0)
        preset = "/ebook";
    else if (strcmp(level, "high") == 0)
        preset = "/screen";
    else {
        err("unknown compression level '%s' (lossless, low, medium, high)",
            level);
        return 0;
    }

    if (!lossless && !tool_exists("gs")) {
        err("'gs' (Ghostscript) is needed for lossy levels; install "
            "ghostscript, or use --level lossless");
        return 0;
    }

    if (!out) {
        say("Dry run: would compress with level '%s'%s\n", level,
            o->dpi ? " (custom DPI)" : "");
        return 1;
    }

    Cmd c;
    cmd_init(&c);

    if (lossless) {
        cmd_add(&c, "qpdf");
        cmd_add(&c, "--warning-exit-0");
        cmd_add(&c, "--recompress-flate");
        cmd_add(&c, "--compression-level=9");
        cmd_add(&c, "--object-streams=generate");
        cmd_add(&c, "--stream-data=compress");
        cmd_add(&c, in);
        cmd_add(&c, out);
    } else {
        cmd_add(&c, "gs");
        cmd_add(&c, "-sDEVICE=pdfwrite");
        cmd_add(&c, "-dCompatibilityLevel=1.5");
        cmd_addf(&c, "-dPDFSETTINGS=%s", preset);
        cmd_add(&c, "-dNOPAUSE");
        cmd_add(&c, "-dBATCH");
        cmd_add(&c, "-dQUIET");
        if (o->dpi > 0) {
            cmd_add(&c, "-dDownsampleColorImages=true");
            cmd_add(&c, "-dDownsampleGrayImages=true");
            cmd_add(&c, "-dDownsampleMonoImages=true");
            cmd_addf(&c, "-dColorImageResolution=%d", o->dpi);
            cmd_addf(&c, "-dGrayImageResolution=%d", o->dpi);
            cmd_addf(&c, "-dMonoImageResolution=%d", o->dpi);
        }
        if (o->grayscale) {
            cmd_add(&c, "-sColorConversionStrategy=Gray");
            cmd_add(&c, "-dProcessColorModel=/DeviceGray");
        }
        cmd_addf(&c, "-sOutputFile=%s", out);
        cmd_add(&c, in);
    }

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("compression failed:\n%s", diag);
        return 0;
    }

    long before = file_size(in), after = file_size(out);

    if (before < 0 || after < 0)
        return 0;

    char a[32], b[32];
    fmt_size((double)before, a, sizeof(a));
    fmt_size((double)after, b, sizeof(b));

    if (after >= before) {
        say("No gain (%s -> %s); original kept, nothing written\n", a, b);
        return 2;
    }

    say("Size: %s -> %s (-%.0f%%)\n", a, b,
        100.0 * (double)(before - after) / (double)before);
    return 1;
}

static int op_repair(const Opts *o, const char *in, const char *out,
                     int *expect)
{
    (void)o;
    (void)expect;

    if (!out) {
        say("Dry run: would attempt to repair PDF\n");
        return 1;
    }

    Cmd c;
    char diag[DIAG_SIZE];

    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, in);
    cmd_add(&c, out);

    if (execute(&c, diag, sizeof(diag)) == 0) {
        say("Repaired with qpdf\n");
        return 1;
    }

    if (!tool_exists("gs")) {
        err("qpdf repair failed and Ghostscript ('gs') is not installed:\n%s", diag);
        return 0;
    }

    trace("qpdf repair failed, falling back to Ghostscript");

    cmd_init(&c);
    cmd_add(&c, "gs");
    cmd_add(&c, "-sDEVICE=pdfwrite");
    cmd_add(&c, "-dNOPAUSE");
    cmd_add(&c, "-dBATCH");
    cmd_add(&c, "-dQUIET");
    cmd_addf(&c, "-sOutputFile=%s", out);
    cmd_add(&c, in);

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("repair failed:\n%s", diag);
        return 0;
    }

    say("Repaired with Ghostscript\n");
    return 1;
}

static int op_decrypt(const Opts *o, const char *in, const char *out,
                      int *expect)
{
    (void)o;
    (void)expect;

    if (!out) {
        say("Dry run: would remove password protection\n");
        return 1;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, "--decrypt");
    cmd_add(&c, in);
    cmd_add(&c, out);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("decryption failed:\n%s", diag);
        return 0;
    }

    return 1;
}

static int random_hex(char *out, size_t bytes)
{
    unsigned char raw[64];
    if (bytes > sizeof(raw))
        bytes = sizeof(raw);

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, bytes) != (ssize_t)bytes) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    close(fd);

    for (size_t i = 0; i < bytes; i++)
        sprintf(out + 2 * i, "%02x", raw[i]);

    return 1;
}

static int acquire_new_passwords(const Opts *o)
{
    if (g_npw_have)
        return 1;

    if (o->upw_file) {
        if (!read_first_line(o->upw_file, g_upw, sizeof(g_upw)))
            return 0;
    } else if (o->upw_env) {
        const char *v = getenv(o->upw_env);
        if (!v) {
            err("environment variable '%s' is not set", o->upw_env);
            return 0;
        }
        if (strlen(v) >= sizeof(g_upw)) {
            err("password too long");
            return 0;
        }
        strcpy(g_upw, v);
    } else {
        char again[MAX_PASSWORD];

        if (!prompt_secret("New PDF password: ", g_upw, sizeof(g_upw)) ||
            !prompt_secret("Repeat password:   ", again, sizeof(again))) {
            err("no password given; use --user-password-file, "
                "--user-password-env or run in a terminal");
            return 0;
        }

        if (strcmp(g_upw, again) != 0) {
            memset(again, 0, sizeof(again));
            err("passwords do not match");
            return 0;
        }
        memset(again, 0, sizeof(again));
    }

    if (!g_upw[0]) {
        err("the new password must not be empty");
        return 0;
    }

    if (o->opw_file) {
        if (!read_first_line(o->opw_file, g_opw, sizeof(g_opw)))
            return 0;
    } else if (o->opw_env) {
        const char *v = getenv(o->opw_env);
        if (!v || strlen(v) >= sizeof(g_opw)) {
            err("environment variable '%s' is not set or too long",
                o->opw_env);
            return 0;
        }
        strcpy(g_opw, v);
    } else if (o->deny) {
        if (!random_hex(g_opw, 16)) {
            err("cannot generate an owner password");
            return 0;
        }
        warn("no owner password given; using a random one, so the "
             "restrictions cannot be changed later");
    } else {
        strcpy(g_opw, g_upw);
    }

    if (strpbrk(g_upw, "\r\n") || strpbrk(g_opw, "\r\n")) {
        err("passwords containing newlines are not supported");
        return 0;
    }

    g_npw_have = 1;
    return 1;
}

static int op_encrypt(const Opts *o, const char *in, const char *out,
                      int *expect)
{
    (void)expect;

    int bits = o->bits ? o->bits : 256;

    if (bits != 128 && bits != 256) {
        err("--bits must be 128 or 256");
        return 0;
    }

    int d_print = 0, d_copy = 0, d_modify = 0, d_annot = 0;

    if (o->deny) {
        char *copy = strdup(o->deny);
        char *save = NULL;

        for (char *t = strtok_r(copy, ",", &save); t;
             t = strtok_r(NULL, ",", &save)) {
            t = trim(t);
            if (!strcasecmp(t, "print"))         d_print = 1;
            else if (!strcasecmp(t, "copy"))     d_copy = 1;
            else if (!strcasecmp(t, "modify"))   d_modify = 1;
            else if (!strcasecmp(t, "annotate")) d_annot = 1;
            else if (!strcasecmp(t, "all"))
                d_print = d_copy = d_modify = d_annot = 1;
            else {
                err("unknown permission '%s' (print, copy, modify, "
                    "annotate, all)", t);
                free(copy);
                return 0;
            }
        }
        free(copy);
    }

    if (!out) {
        say("Dry run: would encrypt with AES-%d%s%s%s%s%s\n", bits,
            o->deny ? ", denying:" : "",
            d_print ? " print" : "", d_copy ? " copy" : "",
            d_modify ? " modify" : "", d_annot ? " annotate" : "");
        return 1;
    }

    if (!acquire_new_passwords(o))
        return 0;

    char *args = tmp_file(tmp_base(), "pdfcrop-args", ".txt");
    if (!args)
        return 0;

    char content[2 * MAX_PASSWORD + 256];
    int n = snprintf(content, sizeof(content),
                     "--encrypt\n%s\n%s\n%d\n%s%s%s%s%s--\n",
                     g_upw, g_opw, bits,
                     bits == 128 ? "--use-aes=y\n" : "",
                     d_print ? "--print=none\n" : "",
                     d_copy ? "--extract=n\n" : "",
                     d_modify ? "--modify=none\n" : "",
                     d_annot ? "--annotate=n\n" : "");

    if (n < 0 || (size_t)n >= sizeof(content) ||
        !write_file(args, content, (size_t)n)) {
        memset(content, 0, sizeof(content));
        return 0;
    }
    memset(content, 0, sizeof(content));

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_addf(&c, "@%s", args);
    cmd_add(&c, in);
    cmd_add(&c, out);

    char diag[DIAG_SIZE];
    int st = execute(&c, diag, sizeof(diag));
    tmp_release(args);

    if (st != 0) {
        err("encryption failed:\n%s", diag);
        return 0;
    }

    return 1;
}

static void *find_bytes(const void *hay, size_t hn, const void *needle,
                        size_t nn)
{
    return memmem(hay, hn, needle, nn);
}

static size_t replace_in_strings(const unsigned char *src, size_t n,
                                 const char *old, const char *new_,
                                 Buf *out)
{
    size_t olen = strlen(old), count = 0;

    Buf esc = {0};
    pdf_escape(&esc, new_, strlen(new_));

    size_t i = 0;

    while (i < n) {
        if (src[i] != '(') {
            buf_cat(out, src + i, 1);
            i++;
            continue;
        }

        size_t j = i + 1;
        int depth = 1;

        while (j < n && depth > 0) {
            if (src[j] == '\\' && j + 1 < n)
                j += 2;
            else {
                if (src[j] == '(')
                    depth++;
                else if (src[j] == ')')
                    depth--;
                j++;
            }
        }

        if (depth != 0) {
            buf_cat(out, src + i, n - i);
            break;
        }

        const unsigned char *body = src + i + 1;
        size_t blen = j - i - 2;

        buf_cat(out, "(", 1);

        size_t k = 0;
        while (k < blen) {
            unsigned char *hit = olen ? find_bytes(body + k, blen - k, old, olen)
                                      : NULL;
            if (!hit) {
                buf_cat(out, body + k, blen - k);
                break;
            }
            buf_cat(out, body + k, (size_t)(hit - (body + k)));
            buf_cat(out, esc.p ? esc.p : "", esc.len);
            k = (size_t)(hit - body) + olen;
            count++;
        }

        buf_cat(out, ")", 1);
        i = j;
    }

    free(esc.p);
    return count;
}

static int op_edit(const Opts *o, const char *in, const char *out,
                   int *expect)
{
    (void)expect;

    if (o->nrepl == 0) {
        err("nothing to do: give at least one --replace 'OLD=>NEW'");
        return 0;
    }

    if (!need_tool("fix-qdf", "comes with qpdf"))
        return 0;

    char *qdf = tmp_file(tmp_base(), "pdfcrop-qdf", ".pdf");
    char *edited = tmp_file(tmp_base(), "pdfcrop-edit", ".pdf");
    char *fixed = tmp_file(tmp_base(), "pdfcrop-fixed", ".pdf");
    if (!qdf || !edited || !fixed)
        return 0;

    Cmd c;
    char diag[DIAG_SIZE];

    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, "--qdf");
    cmd_add(&c, "--object-streams=disable");
    cmd_add(&c, in);
    cmd_add(&c, qdf);

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("cannot open PDF for editing:\n%s", diag);
        return 0;
    }

    size_t len = 0;
    unsigned char *data = read_file(qdf, &len);
    if (!data)
        return 0;

    size_t total = 0;

    for (int r = 0; r < o->nrepl; r++) {
        char *spec = strdup(o->repl[r]);
        char *arrow = strstr(spec, "=>");

        if (!arrow || arrow == spec) {
            err("bad --replace '%s': use 'OLD=>NEW'", o->repl[r]);
            free(spec);
            free(data);
            return 0;
        }

        *arrow = '\0';

        Buf nb = {0};
        size_t cnt = replace_in_strings(data, len, spec, arrow + 2, &nb);

        say("  '%s' -> '%s': %zu replacement%s\n", spec, arrow + 2, cnt,
            cnt == 1 ? "" : "s");

        free(data);
        data = (unsigned char *)nb.p;
        len = nb.len;
        total += cnt;
        free(spec);

        if (!data)
            return 0;
    }

    if (total == 0) {
        free(data);
        err("text not found. Only text stored as plain strings can be "
            "replaced; text split by kerning, in subset/CID fonts, or in "
            "scans cannot (try 'ocr' first for scans)");
        return 0;
    }

    if (!out) {
        free(data);
        say("Dry run: %zu replacement%s would be made\n", total,
            total == 1 ? "" : "s");
        return 1;
    }

    int ok = write_file(edited, data, len);
    free(data);
    if (!ok)
        return 0;

    cmd_init(&c);
    cmd_add(&c, "fix-qdf");
    cmd_add(&c, edited);
    cmd_add(&c, fixed);

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("could not rebuild the edited PDF:\n%s", diag);
        return 0;
    }

    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, fixed);
    cmd_add(&c, out);

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("could not write the edited PDF:\n%s", diag);
        return 0;
    }

    return 1;
}

static int ocr_with_tesseract(const Opts *o, const char *in, const char *out)
{
    if (!need_tool("pdftoppm", "poppler-utils") ||
        !need_tool("tesseract", "tesseract-ocr"))
        return 0;

    const char *lang = o->lang ? o->lang : "eng";

    char *dir = tmp_dir(tmp_base(), "pdfcrop-ocr");
    if (!dir)
        return 0;

    Cmd c;
    char diag[DIAG_SIZE];

    cmd_init(&c);
    cmd_add(&c, "pdftoppm");
    cmd_add(&c, "-r");
    cmd_add(&c, "300");
    cmd_add(&c, "-png");
    cmd_add(&c, in);
    cmd_addf(&c, "%s/pg", dir);

    say("Rendering pages...\n");

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("rendering failed:\n%s", diag);
        return 0;
    }

    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s/pg-*.png", dir);

    glob_t g = {0};
    if (glob(pattern, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        err("no pages were rendered");
        return 0;
    }

    char **pdfs = calloc(g.gl_pathc, sizeof(char *));
    Buf side = {0};
    int ok = 1;

    if (!pdfs) {
        globfree(&g);
        return 0;
    }

    for (size_t i = 0; i < g.gl_pathc && ok; i++) {
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s/ocr-%06zu", dir, i + 1);

        say("OCR page %zu/%zu\r", i + 1, g.gl_pathc);
        fflush(stdout);

        cmd_init(&c);
        cmd_add(&c, "tesseract");
        cmd_add(&c, g.gl_pathv[i]);
        cmd_add(&c, base);
        cmd_add(&c, "-l");
        cmd_add(&c, lang);
        cmd_add(&c, "pdf");
        if (o->sidecar)
            cmd_add(&c, "txt");

        if (execute(&c, diag, sizeof(diag)) != 0) {
            err("tesseract failed on page %zu:\n%s", i + 1, diag);
            ok = 0;
            break;
        }

        char pp[PATH_MAX];
        snprintf(pp, sizeof(pp), "%s.pdf", base);
        pdfs[i] = strdup(pp);

        if (o->sidecar) {
            char tp[PATH_MAX];
            snprintf(tp, sizeof(tp), "%s.txt", base);
            size_t tl = 0;
            unsigned char *t = read_file(tp, &tl);
            if (t) {
                buf_cat(&side, t, tl);
                free(t);
            }
            buf_cat(&side, "\f", 1);
        }
    }

    say("\n");

    if (ok) {
        cmd_init(&c);
        cmd_add(&c, "qpdf");
        cmd_add(&c, "--warning-exit-0");
        cmd_add(&c, "--empty");
        cmd_add(&c, "--pages");
        for (size_t i = 0; i < g.gl_pathc; i++)
            cmd_add(&c, pdfs[i]);
        cmd_add(&c, "--");
        cmd_add(&c, out);

        if (execute(&c, diag, sizeof(diag)) != 0) {
            err("could not assemble the OCR'd PDF:\n%s", diag);
            ok = 0;
        }
    }

    if (ok && o->sidecar && !write_file(o->sidecar, side.p ? side.p : "",
                                        side.len))
        ok = 0;

    for (size_t i = 0; i < g.gl_pathc; i++)
        free(pdfs[i]);
    free(pdfs);
    free(side.p);
    globfree(&g);
    return ok;
}

static int op_ocr(const Opts *o, const char *in, const char *out,
                  int *expect)
{
    (void)expect;

    const char *engine = o->engine ? o->engine : "auto";
    int use_mypdf;

    if (strcmp(engine, "ocrmypdf") == 0)
        use_mypdf = 1;
    else if (strcmp(engine, "tesseract") == 0)
        use_mypdf = 0;
    else if (strcmp(engine, "auto") == 0)
        use_mypdf = tool_exists("ocrmypdf");
    else {
        err("unknown OCR engine '%s' (auto, ocrmypdf, tesseract)", engine);
        return 0;
    }

    if (!out) {
        say("Dry run: would OCR with %s, language '%s'\n",
            use_mypdf ? "ocrmypdf" : "tesseract", o->lang ? o->lang : "eng");
        return 1;
    }

    if (!use_mypdf) {
        if (strcmp(engine, "auto") == 0)
            trace("ocrmypdf not found, using the tesseract fallback "
                  "(pages are rasterised)");
        return ocr_with_tesseract(o, in, out);
    }

    if (!need_tool("ocrmypdf", "pip install ocrmypdf / apt install ocrmypdf"))
        return 0;

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "ocrmypdf");
    cmd_add(&c, "--quiet");
    cmd_add(&c, o->force_ocr ? "--force-ocr" : "--skip-text");
    cmd_add(&c, "-l");
    cmd_add(&c, o->lang ? o->lang : "eng");
    if (o->deskew)
        cmd_add(&c, "--deskew");
    if (o->rotate)
        cmd_add(&c, "--rotate-pages");
    if (o->sidecar) {
        cmd_add(&c, "--sidecar");
        cmd_add(&c, o->sidecar);
    }
    cmd_add(&c, in);
    cmd_add(&c, out);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("ocrmypdf failed:\n%s", diag);
        return 0;
    }

    return 1;
}

static const short helv[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584
};

static double text_width(const char *s, double size)
{
    double w = 0;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        w += (ch >= 32 && ch <= 126) ? helv[ch - 32] : 556;
    }
    return w * size / 1000.0;
}

static int valid_pos(const char *pos)
{
    static const char *const valid[] = {
        "center", "left", "right", "top", "bottom",
        "top-left", "top-right", "bottom-left", "bottom-right", NULL
    };
    for (int i = 0; valid[i]; i++) {
        if (strcmp(pos, valid[i]) == 0)
            return 1;
    }
    return 0;
}

static int parse_color(const char *s, double *r, double *g, double *b)
{
    static const struct { const char *n; double r, g, b; } named[] = {
        {"black", 0, 0, 0}, {"white", 1, 1, 1}, {"gray", .5, .5, .5},
        {"grey", .5, .5, .5}, {"red", .85, 0, 0}, {"green", 0, .6, 0},
        {"blue", 0, 0, .8}, {"navy", .05, .1, .45}, {"orange", 1, .55, 0}
    };

    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strcasecmp(s, named[i].n) == 0) {
            *r = named[i].r; *g = named[i].g; *b = named[i].b;
            return 1;
        }
    }

    unsigned v;
    if (s[0] == '#' && strlen(s) == 7 && sscanf(s + 1, "%6x", &v) == 1) {
        *r = ((v >> 16) & 255) / 255.0;
        *g = ((v >> 8) & 255) / 255.0;
        *b = (v & 255) / 255.0;
        return 1;
    }

    return 0;
}

static int jpeg_info(const unsigned char *d, size_t n, int *w, int *h,
                     int *comps)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return 0;

    size_t i = 2;

    while (i + 9 < n) {
        if (d[i] != 0xFF) { i++; continue; }

        int m = d[i + 1];

        if (m == 0xFF) { i++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;
            continue;
        }

        size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];

        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            *comps = d[i + 9];
            return *w > 0 && *h > 0;
        }

        i += 2 + len;
    }

    return 0;
}

static const char *ensure_jpeg(const char *img)
{
    size_t len = 0;
    unsigned char *d = read_file(img, &len);
    if (!d)
        return NULL;

    int w, h, c;
    int is_jpeg = jpeg_info(d, len, &w, &h, &c);
    free(d);

    if (is_jpeg)
        return img;

    const char *magick = tool_exists("magick") ? "magick" :
                         tool_exists("convert") ? "convert" : NULL;

    if (!magick) {
        err("'%s' is not a JPEG; convert it to JPEG first, or install "
            "ImageMagick so pdfcrop can do it", img);
        return NULL;
    }

    char *tmp = tmp_file(tmp_base(), "pdfcrop-img", ".jpg");
    if (!tmp)
        return NULL;

    Cmd cm;
    char diag[DIAG_SIZE];
    cmd_init(&cm);
    cmd_add(&cm, magick);
    cmd_add(&cm, img);
    cmd_add(&cm, "-background");
    cmd_add(&cm, "white");
    cmd_add(&cm, "-flatten");
    cmd_add(&cm, "-colorspace");
    cmd_add(&cm, "sRGB");
    cmd_add(&cm, tmp);

    if (execute(&cm, diag, sizeof(diag)) != 0) {
        err("image conversion failed:\n%s", diag);
        return NULL;
    }

    return tmp;
}

static int make_image_pdf(const char *jpeg_path, const char *out_pdf)
{
    size_t imglen = 0;
    unsigned char *img = read_file(jpeg_path, &imglen);
    int iw = 0, ih = 0, comps = 0;

    if (!img || !jpeg_info(img, imglen, &iw, &ih, &comps)) {
        free(img);
        err("cannot read image '%s'", jpeg_path);
        return 0;
    }

    if (comps != 1 && comps != 3) {
        free(img);
        err("only grayscale and RGB JPEG images are supported");
        return 0;
    }

    double W = (double)iw * 72.0 / 96.0;
    double H = (double)ih * 72.0 / 96.0;

    Buf cs = {0};
    buf_printf(&cs, "q %.4f 0 0 %.4f 0 0 cm /Im1 Do Q\n", W, H);

    Buf out = {0};
    size_t off[6] = {0};

    buf_printf(&out, "%%PDF-1.4\n");
    off[1] = out.len;
    buf_printf(&out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = out.len;
    buf_printf(&out, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = out.len;
    buf_printf(&out, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
                     "/Resources << /XObject << /Im1 5 0 R >> >> /Contents 4 0 R >>\nendobj\n",
               W, H);
    off[4] = out.len;
    buf_printf(&out, "4 0 obj\n<< /Length %zu >>\nstream\n", cs.len);
    buf_cat(&out, cs.p, cs.len);
    buf_printf(&out, "endstream\nendobj\n");
    off[5] = out.len;
    buf_printf(&out, "5 0 obj\n<< /Type /XObject /Subtype /Image /Width %d "
                     "/Height %d /ColorSpace /%s /BitsPerComponent 8 "
                     "/Filter /DCTDecode /Length %zu >>\nstream\n",
               iw, ih, comps == 1 ? "DeviceGray" : "DeviceRGB", imglen);
    buf_cat(&out, img, imglen);
    buf_printf(&out, "\nendstream\nendobj\n");

    size_t xref = out.len;
    buf_printf(&out, "xref\n0 6\n0000000000 65535 f \n");
    for (int i = 1; i < 6; i++)
        buf_printf(&out, "%010zu 00000 n \n", off[i]);
    buf_printf(&out, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
               xref);

    int ok = write_file(out_pdf, out.p, out.len);
    free(out.p);
    free(cs.p);
    free(img);
    return ok;
}

static int pdf_page_size(const char *pdf, double *w, double *h)
{
    *w = 595.28;
    *h = 841.89;

    if (!tool_exists("pdfinfo")) {
        warn("pdfinfo not found; assuming A4 page size");
        return 1;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pdfinfo");
    cmd_add(&c, "-f");
    cmd_add(&c, "1");
    cmd_add(&c, "-l");
    cmd_add(&c, "1");
    cmd_add(&c, pdf);

    char out[DIAG_SIZE];

    if (execute(&c, out, sizeof(out)) == 0) {
        char *p = strstr(out, "size:");
        double a, b;
        if (p && sscanf(p, "size: %lf x %lf", &a, &b) == 2 && a > 0 && b > 0) {
            *w = a;
            *h = b;
        }
    }

    return 1;
}

static void expand_text(const char *in, char *out, size_t sz)
{
    char date[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);

    size_t o = 0;

    for (const char *p = in; *p && o + 1 < sz;) {
        if (strncmp(p, "{date}", 6) == 0) {
            o += (size_t)snprintf(out + o, sz - o, "%s", date);
            p += 6;
        } else {
            unsigned char ch = (unsigned char)*p++;
            out[o++] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        }
    }

    out[o < sz ? o : sz - 1] = '\0';
}

static int make_stamp_pdf(const Opts *o, int is_sign, double W, double H,
                          const char *jpeg_path, const char *path)
{
    double size = o->st_size > 0 ? o->st_size : (is_sign ? 14 : 72);
    double angle = isnan(o->st_angle) ? (is_sign ? 0 : 45) : o->st_angle;
    double opacity = isnan(o->st_opacity) ? (is_sign ? 1.0 : 0.25)
                                           : o->st_opacity;
    double scale = isnan(o->st_scale) ? (is_sign ? 0.25 : 0.5) : o->st_scale;
    const char *pos = o->st_pos ? o->st_pos : (is_sign ? "bottom-right"
                                                       : "center");
    double cr = 0.5, cg = 0.5, cb = 0.5;

    if (is_sign)
        cr = cg = cb = 0;

    if (o->st_color && !parse_color(o->st_color, &cr, &cg, &cb)) {
        err("unknown color '%s' (name or #RRGGBB)", o->st_color);
        return 0;
    }

    if (opacity < 0 || opacity > 1) {
        err("--opacity must be between 0 and 1");
        return 0;
    }

    if (!valid_pos(pos)) {
        err("unknown position '%s'", pos);
        return 0;
    }

    char text[512] = "";
    double cw, ch;
    int iw = 0, ih = 0, comps = 0;
    unsigned char *img = NULL;
    size_t imglen = 0;

    if (jpeg_path) {
        img = read_file(jpeg_path, &imglen);
        if (!img || !jpeg_info(img, imglen, &iw, &ih, &comps)) {
            free(img);
            err("cannot read image '%s'", jpeg_path);
            return 0;
        }
        if (comps != 1 && comps != 3) {
            free(img);
            err("only grayscale and RGB JPEG images are supported");
            return 0;
        }
        cw = scale * W;
        ch = cw * ih / iw;
    } else {
        expand_text(o->st_text, text, sizeof(text));
        cw = text_width(text, size);
        ch = size;
    }

    const double margin = 36;
    double cx = W / 2, cy = H / 2;

    if (strstr(pos, "left"))        cx = margin + cw / 2;
    else if (strstr(pos, "right"))  cx = W - margin - cw / 2;
    if (strstr(pos, "top"))         cy = H - margin - ch / 2;
    else if (strstr(pos, "bottom")) cy = margin + ch / 2;

    double rad = angle * M_PI / 180.0, c = cos(rad), s = sin(rad);

    Buf cs = {0};
    buf_printf(&cs, "q\n/GS1 gs\n");

    if (img) {
        double tx = cx + (-cw / 2) * c - (-ch / 2) * s;
        double ty = cy + (-cw / 2) * s + (-ch / 2) * c;
        buf_printf(&cs, "q %.4f %.4f %.4f %.4f %.4f %.4f cm /Im1 Do Q\n",
                   cw * c, cw * s, -ch * s, ch * c, tx, ty);
    } else {
        double lx = -cw / 2, ly = -size * 0.35;
        double tx = cx + lx * c - ly * s;
        double ty = cy + lx * s + ly * c;
        buf_printf(&cs, "%.4f %.4f %.4f rg\nBT /F1 %.2f Tf "
                        "%.4f %.4f %.4f %.4f %.4f %.4f Tm (",
                   cr, cg, cb, size, c, s, -s, c, tx, ty);
        pdf_escape(&cs, text, strlen(text));
        buf_printf(&cs, ") Tj ET\n");
    }

    buf_printf(&cs, "Q\n");

    Buf out = {0};
    size_t off[8] = {0};

    buf_printf(&out, "%%PDF-1.4\n");

    off[1] = out.len;
    buf_printf(&out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = out.len;
    buf_printf(&out, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = out.len;
    buf_printf(&out, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
                     "/Resources << /Font << /F1 5 0 R >> %s"
                     "/ExtGState << /GS1 7 0 R >> >> /Contents 4 0 R >>\nendobj\n",
               W, H, img ? "/XObject << /Im1 6 0 R >> " : "");
    off[4] = out.len;
    buf_printf(&out, "4 0 obj\n<< /Length %zu >>\nstream\n", cs.len);
    buf_cat(&out, cs.p, cs.len);
    buf_printf(&out, "endstream\nendobj\n");
    off[5] = out.len;
    buf_printf(&out, "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                     "/Encoding /WinAnsiEncoding >>\nendobj\n");
    off[6] = out.len;
    if (img) {
        buf_printf(&out, "6 0 obj\n<< /Type /XObject /Subtype /Image /Width %d "
                         "/Height %d /ColorSpace /%s /BitsPerComponent 8 "
                         "/Filter /DCTDecode /Length %zu >>\nstream\n",
                   iw, ih, comps == 1 ? "DeviceGray" : "DeviceRGB", imglen);
        buf_cat(&out, img, imglen);
        buf_printf(&out, "\nendstream\nendobj\n");
    } else {
        buf_printf(&out, "6 0 obj\nnull\nendobj\n");
    }
    off[7] = out.len;
    buf_printf(&out, "7 0 obj\n<< /Type /ExtGState /ca %.3f /CA %.3f >>\nendobj\n",
               opacity, opacity);

    size_t xref = out.len;
    buf_printf(&out, "xref\n0 8\n0000000000 65535 f \n");
    for (int i = 1; i < 8; i++)
        buf_printf(&out, "%010zu 00000 n \n", off[i]);
    buf_printf(&out, "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
               xref);

    int ok = write_file(path, out.p, out.len);

    free(out.p);
    free(cs.p);
    free(img);
    return ok;
}

static void format_pagenum(const char *fmt, int n, int total, char *out,
                           size_t sz)
{
    size_t o = 0;
    for (const char *p = fmt; *p && o + 1 < sz;) {
        if (strncmp(p, "{n}", 3) == 0) {
            o += (size_t)snprintf(out + o, sz - o, "%d", n);
            p += 3;
        } else if (strncmp(p, "{total}", 7) == 0) {
            o += (size_t)snprintf(out + o, sz - o, "%d", total);
            p += 7;
        } else {
            unsigned char ch = (unsigned char)*p++;
            out[o++] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        }
    }
    out[o < sz ? o : sz - 1] = '\0';
}

static int op_pagenum(const Opts *o, const char *in, const char *out,
                      int *expect)
{
    const char *pos = o->st_pos ? o->st_pos : "bottom-right";
    const char *fmt = o->num_format ? o->num_format : "{n} / {total}";
    const char *spec = o->st_on ? o->st_on : "all";
    double size = o->st_size > 0 ? o->st_size : 10.0;
    double opacity = isnan(o->st_opacity) ? 1.0 : o->st_opacity;
    int start = o->num_start > 0 ? o->num_start : 1;
    double cr = 0.25, cg = 0.25, cb = 0.25;

    if (!valid_pos(pos)) {
        err("unknown position '%s'", pos);
        return 0;
    }

    if (o->st_color && !parse_color(o->st_color, &cr, &cg, &cb)) {
        err("unknown color '%s' (name or #RRGGBB)", o->st_color);
        return 0;
    }

    if (opacity < 0 || opacity > 1) {
        err("--opacity must be between 0 and 1");
        return 0;
    }

    int total;
    if (!qpdf_page_count(in, &total))
        return 0;

    PageList pl = {0};
    if (!parse_selection(spec, total, &pl))
        return 0;

    char *mark = calloc((size_t)total + 2, 1);
    if (!mark) {
        pages_free(&pl);
        return 0;
    }

    for (size_t i = 0; i < pl.count; i++)
        mark[pl.data[i]] = 1;
    pages_free(&pl);

    say("Numbering pages (%s) with format '%s'\n", spec, fmt);

    if (!out) {
        free(mark);
        return 1;
    }

    double W, H;
    pdf_page_size(in, &W, &H);

    char *stamp = tmp_file(tmp_base(), "pdfcrop-pnum", ".pdf");
    if (!stamp) {
        free(mark);
        return 0;
    }

    size_t nobj = 4 + (size_t)total * 2;
    size_t *off = calloc(nobj + 1, sizeof(size_t));
    if (!off) {
        free(mark);
        return 0;
    }

    Buf pdf = {0};
    buf_printf(&pdf, "%%PDF-1.4\n");

    off[1] = pdf.len;
    buf_printf(&pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    off[2] = pdf.len;
    buf_printf(&pdf, "2 0 obj\n<< /Type /Pages /Count %d /Kids [", total);
    for (int p = 0; p < total; p++)
        buf_printf(&pdf, "%d 0 R ", 5 + p * 2);
    buf_printf(&pdf, "] >>\nendobj\n");

    off[3] = pdf.len;
    buf_printf(&pdf, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                     "/Encoding /WinAnsiEncoding >>\nendobj\n");

    off[4] = pdf.len;
    buf_printf(&pdf, "4 0 obj\n<< /Type /ExtGState /ca %.3f /CA %.3f >>\nendobj\n",
               opacity, opacity);

    const double margin = 24.0;
    int num = start;

    for (int p = 1; p <= total; p++) {
        int pg_obj = 5 + (p - 1) * 2;
        int cs_obj = pg_obj + 1;

        Buf cs = {0};
        if (mark[p]) {
            char label[256];
            format_pagenum(fmt, num++, total, label, sizeof(label));
            double cw = text_width(label, size);
            double cx = W / 2.0, cy = H / 2.0;

            if (strstr(pos, "left"))        cx = margin + cw / 2.0;
            else if (strstr(pos, "right"))  cx = W - margin - cw / 2.0;
            if (strstr(pos, "top"))         cy = H - margin - size / 2.0;
            else if (strstr(pos, "bottom")) cy = margin + size / 2.0;

            double tx = cx - cw / 2.0;
            double ty = cy - size * 0.35;

            buf_printf(&cs, "q /GS1 gs %.4f %.4f %.4f rg BT /F1 %.2f Tf "
                            "1 0 0 1 %.4f %.4f Tm (",
                       cr, cg, cb, size, tx, ty);
            pdf_escape(&cs, label, strlen(label));
            buf_printf(&cs, ") Tj ET Q\n");
        }

        off[pg_obj] = pdf.len;
        buf_printf(&pdf, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] "
                         "/Resources << /Font << /F1 3 0 R >> /ExtGState << /GS1 4 0 R >> >> "
                         "/Contents %d 0 R >>\nendobj\n",
                   pg_obj, W, H, cs_obj);

        off[cs_obj] = pdf.len;
        buf_printf(&pdf, "%d 0 obj\n<< /Length %zu >>\nstream\n", cs_obj, cs.len);
        if (cs.len)
            buf_cat(&pdf, cs.p, cs.len);
        buf_printf(&pdf, "endstream\nendobj\n");
        free(cs.p);
    }

    size_t xref = pdf.len;
    buf_printf(&pdf, "xref\n0 %zu\n0000000000 65535 f \n", nobj + 1);
    for (size_t i = 1; i <= nobj; i++)
        buf_printf(&pdf, "%010zu 00000 n \n", off[i]);
    buf_printf(&pdf, "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
               nobj + 1, xref);

    int ok = write_file(stamp, pdf.p, pdf.len);
    free(pdf.p);
    free(off);
    free(mark);

    if (!ok)
        return 0;

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, in);
    cmd_add(&c, "--overlay");
    cmd_add(&c, stamp);
    cmd_add(&c, "--");
    cmd_add(&c, out);

    char diag[DIAG_SIZE];
    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("page numbering failed:\n%s", diag);
        return 0;
    }

    *expect = total;
    return 1;
}

static int op_crop(const Opts *o, const char *in, const char *out,
                   int *expect)
{
    if (!o->margins) {
        err("crop requires --margins (e.g. '36' or '36,24' or '36,24,36,24' in pt)");
        return 0;
    }

    double v[4] = {0};
    int cnt = sscanf(o->margins, "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]);
    double top, right, bottom, left;

    if (cnt == 1) {
        top = right = bottom = left = v[0];
    } else if (cnt == 2) {
        top = bottom = v[0];
        right = left = v[1];
    } else if (cnt == 4) {
        top = v[0]; right = v[1]; bottom = v[2]; left = v[3];
    } else {
        err("invalid --margins '%s' (use ALL, V,H, or T,R,B,L)", o->margins);
        return 0;
    }

    if (top < 0 || right < 0 || bottom < 0 || left < 0) {
        err("margins must be non-negative");
        return 0;
    }

    if (!need_tool("gs", "ghostscript"))
        return 0;

    double W, H;
    pdf_page_size(in, &W, &H);

    if (left + right >= W || top + bottom >= H) {
        err("margins (%.1f, %.1f, %.1f, %.1f) exceed page size %.1f x %.1f pt",
            top, right, bottom, left, W, H);
        return 0;
    }

    int total;
    if (!qpdf_page_count(in, &total))
        return 0;

    say("Cropping margins T=%.1f R=%.1f B=%.1f L=%.1f pt\n",
        top, right, bottom, left);

    if (!out)
        return 1;

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "gs");
    cmd_add(&c, "-sDEVICE=pdfwrite");
    cmd_add(&c, "-dNOPAUSE");
    cmd_add(&c, "-dBATCH");
    cmd_add(&c, "-dQUIET");
    cmd_addf(&c, "-sOutputFile=%s", out);
    cmd_add(&c, "-c");
    cmd_addf(&c, "[/CropBox [%.2f %.2f %.2f %.2f] /PAGES pdfmark",
             left, bottom, W - right, H - top);
    cmd_add(&c, "-f");
    cmd_add(&c, in);

    char diag[DIAG_SIZE];
    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("cropping failed:\n%s", diag);
        return 0;
    }

    *expect = total;
    return 1;
}

static int apply_stamp(const Opts *o, int is_sign, const char *in,
                       const char *out)
{
    const char *spec = o->st_on ? o->st_on : (is_sign ? "last" : "all");
    const char *layer = o->st_layer ? o->st_layer : "over";

    if (strcmp(layer, "over") && strcmp(layer, "under")) {
        err("--layer must be 'over' or 'under'");
        return 0;
    }

    int total;
    if (!qpdf_page_count(in, &total))
        return 0;

    PageList pl = {0};
    char *expr = NULL;
    int ok = 0;

    if (!parse_selection(spec, total, &pl))
        goto done;

    expr = build_set_expression(&pl, total);
    if (!expr)
        goto done;

    say("Stamping pages %s of %d (%s the page content)\n", expr, total,
        strcmp(layer, "over") == 0 ? "over" : "under");

    if (!out) {
        ok = 1;
        goto done;
    }

    const char *stamp = o->st_pdf;

    if (!stamp) {
        const char *jpg = NULL;

        if (o->st_image) {
            jpg = ensure_jpeg(o->st_image);
            if (!jpg)
                goto done;
        }

        double W, H;
        pdf_page_size(in, &W, &H);

        char *sp = tmp_file(tmp_base(), "pdfcrop-stamp", ".pdf");
        if (!sp || !make_stamp_pdf(o, is_sign, W, H, jpg, sp))
            goto done;
        stamp = sp;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_add(&c, in);
    cmd_add(&c, strcmp(layer, "over") == 0 ? "--overlay" : "--underlay");
    cmd_add(&c, stamp);
    cmd_addf(&c, "--to=%s", expr);
    cmd_add(&c, "--from=1");
    cmd_add(&c, "--repeat=1");
    cmd_add(&c, "--");
    cmd_add(&c, out);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("stamping failed:\n%s", diag);
        goto done;
    }

    ok = 1;

done:
    free(expr);
    pages_free(&pl);
    return ok;
}

static int op_watermark(const Opts *o, const char *in, const char *out,
                        int *expect)
{
    (void)expect;

    int sources = (o->st_text != NULL) + (o->st_image != NULL) +
                  (o->st_pdf != NULL);

    if (sources != 1) {
        err("give exactly one of --stamp-text, --stamp-image, --stamp-pdf");
        return 0;
    }

    return apply_stamp(o, 0, in, out);
}

static int op_sign(const Opts *o, const char *in, const char *out,
                   int *expect)
{
    int visual = o->st_text || o->st_image || o->st_pdf;

    if (!visual && !o->cert) {
        err("nothing to do: give --stamp-text/--stamp-image (visible "
            "signature) and/or --cert (cryptographic signature)");
        return 0;
    }

    if (o->cert && !out) {
        say("Dry run: would %ssign with certificate '%s'\n",
            visual ? "stamp, then " : "", o->cert);
        return visual ? apply_stamp(o, 1, in, NULL) : 1;
    }

    if (o->cert && !need_tool("pyhanko", "pip install pyhanko pyhanko-certvalidator"))
        return 0;

    int total;
    if (!qpdf_page_count(in, &total))
        return 0;
    *expect = total;

    const char *stage = in;

    if (visual) {
        const char *dest = out;

        if (o->cert) {
            dest = tmp_file(tmp_base(), "pdfcrop-vis", ".pdf");
            if (!dest)
                return 0;
        }

        if (!apply_stamp(o, 1, in, dest))
            return 0;

        stage = dest;
        if (!o->cert)
            return 1;
    }

    if (!out)
        return 1;

    const char *passfile = o->cert_pass_file;

    if (!passfile && o->cert_pass_env) {
        const char *v = getenv(o->cert_pass_env);
        if (!v) {
            err("environment variable '%s' is not set", o->cert_pass_env);
            return 0;
        }
        char *pf = tmp_file(tmp_base(), "pdfcrop-cp", ".txt");
        if (!pf || !write_file(pf, v, strlen(v)))
            return 0;
        passfile = pf;
    }

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pyhanko");
    cmd_add(&c, "sign");
    cmd_add(&c, "addsig");
    cmd_add(&c, "--field");
    cmd_add(&c, o->field ? o->field : "Signature1");
    if (o->reason) {
        cmd_add(&c, "--reason");
        cmd_add(&c, o->reason);
    }
    if (o->location) {
        cmd_add(&c, "--location");
        cmd_add(&c, o->location);
    }
    cmd_add(&c, "pkcs12");
    if (passfile) {
        cmd_add(&c, "--passfile");
        cmd_add(&c, passfile);
    }
    cmd_add(&c, stage);
    cmd_add(&c, out);
    cmd_add(&c, o->cert);

    int st;
    char diag[DIAG_SIZE];

    if (passfile) {
        st = execute(&c, diag, sizeof(diag));
    } else {
        st = execute_inherit(&c);
        diag[0] = '\0';
    }

    if (st != 0) {
        err("digital signing failed%s%s", diag[0] ? ":\n" : "", diag);
        return 0;
    }

    return 1;
}

static int process_file(const Opts *o, const char *arg, const char *suffix,
                        PdfOp op)
{
    char input[PATH_MAX], out[PATH_MAX], plain[PATH_MAX], dir[PATH_MAX];
    struct stat st;
    int ok = 0, expect = 0;
    char *tmp;

    if (!realpath(arg, input)) {
        err("cannot locate input PDF '%s': %s", arg, strerror(errno));
        return 0;
    }

    if (stat(input, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(input, R_OK) != 0) {
        err("'%s' is not a readable regular file", arg);
        return 0;
    }

    if (!resolve_output(o, input, suffix, "pdf", 1, out, sizeof(out)) ||
        !check_output(o, input, out))
        return 0;

    say("%s\n", arg);

    if (!prepare_input(o, input, plain, sizeof(plain)))
        goto end;

    if (o->dry_run) {
        ok = op(o, plain, NULL, &expect) != 0;
        if (ok)
            say("Dry run: would write %s\n", out);
        goto end;
    }

    path_dir(out, dir, sizeof(dir));

    if (!mkdir_p(dir)) {
        err("cannot create directory '%s': %s", dir, strerror(errno));
        goto end;
    }

    tmp = tmp_file(dir, ".pdfcrop", ".pdf");
    if (!tmp)
        goto end;

    int rc = op(o, plain, tmp, &expect);

    if (rc == 0)
        goto end;

    if (rc == 2) {
        ok = 1;
        goto end;
    }

    if (!finalize_pdf(o, tmp, out, expect))
        goto end;

    say("Saved: %s\n", out);
    ok = 1;

end:
    tmp_release_all();
    return ok;
}

static int process_split(const Opts *o, const char *arg)
{
    char input[PATH_MAX], plain[PATH_MAX], stem[PATH_MAX], dir[PATH_MAX];
    struct stat st;
    int ok = 0, total = 0;

    if (!realpath(arg, input) || stat(input, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        err("cannot read input PDF '%s'", arg);
        return 0;
    }

    file_stem(input, stem, sizeof(stem));
    snprintf(dir, sizeof(dir), "%s", o->outdir ? o->outdir : ".");

    if (!o->dry_run && !mkdir_p(dir)) {
        err("cannot create directory '%s': %s", dir, strerror(errno));
        return 0;
    }

    say("%s\n", arg);

    if (!prepare_input(o, input, plain, sizeof(plain)))
        goto end;

    if (!qpdf_page_count(plain, &total))
        goto end;

    if (o->split_ranges) {
        char *copy = strdup(o->split_ranges);
        if (!copy)
            goto end;

        char *save = NULL;
        int part = 0;
        ok = 1;

        for (char *tok = strtok_r(copy, ";", &save); tok && ok;
             tok = strtok_r(NULL, ";", &save)) {
            char *spec = trim(tok);
            if (!*spec)
                continue;

            part++;
            PageList pl = {0};
            if (!parse_selection(spec, total, &pl)) {
                pages_free(&pl);
                ok = 0;
                break;
            }

            char *expr = build_expression(&pl);
            int expect = (int)pl.count;
            pages_free(&pl);

            if (!expr) {
                ok = 0;
                break;
            }

            char out[PATH_MAX];
            snprintf(out, sizeof(out), "%s/%s-part%03d.pdf", dir, stem, part);

            if (!check_output(o, input, out)) {
                free(expr);
                ok = 0;
                break;
            }

            if (o->dry_run) {
                say("Dry run: would write %s (pages %s)\n", out, expr);
                free(expr);
                continue;
            }

            char *tmp = tmp_file(dir, ".pdfcrop-sp", ".pdf");
            if (!tmp) {
                free(expr);
                ok = 0;
                break;
            }

            Cmd c;
            cmd_init(&c);
            cmd_add(&c, "qpdf");
            cmd_add(&c, "--warning-exit-0");
            if (o->linearize)
                cmd_add(&c, "--linearize");
            cmd_add(&c, plain);
            cmd_add(&c, "--pages");
            cmd_add(&c, ".");
            cmd_add(&c, expr);
            cmd_add(&c, "--");
            cmd_add(&c, tmp);

            char diag[DIAG_SIZE];
            if (execute(&c, diag, sizeof(diag)) != 0) {
                err("split part %d failed:\n%s", part, diag);
                free(expr);
                ok = 0;
                break;
            }

            free(expr);

            if (!finalize_pdf(o, tmp, out, expect)) {
                ok = 0;
                break;
            }
            say("Saved: %s\n", out);
        }

        free(copy);
        if (part == 0) {
            err("no valid ranges in --ranges");
            ok = 0;
        }
    } else {
        int every = o->split_every > 0 ? o->split_every : 1;
        int part = 0;
        ok = 1;

        for (int p = 1; p <= total && ok; p += every) {
            int last = p + every - 1;
            if (last > total)
                last = total;
            part++;

            char expr[64], out[PATH_MAX];
            if (p == last)
                snprintf(expr, sizeof(expr), "%d", p);
            else
                snprintf(expr, sizeof(expr), "%d-%d", p, last);

            snprintf(out, sizeof(out), "%s/%s-part%03d.pdf", dir, stem, part);

            if (!check_output(o, input, out)) {
                ok = 0;
                break;
            }

            if (o->dry_run) {
                say("Dry run: would write %s (pages %s)\n", out, expr);
                continue;
            }

            char *tmp = tmp_file(dir, ".pdfcrop-sp", ".pdf");
            if (!tmp) {
                ok = 0;
                break;
            }

            Cmd c;
            cmd_init(&c);
            cmd_add(&c, "qpdf");
            cmd_add(&c, "--warning-exit-0");
            if (o->linearize)
                cmd_add(&c, "--linearize");
            cmd_add(&c, plain);
            cmd_add(&c, "--pages");
            cmd_add(&c, ".");
            cmd_add(&c, expr);
            cmd_add(&c, "--");
            cmd_add(&c, tmp);

            char diag[DIAG_SIZE];
            if (execute(&c, diag, sizeof(diag)) != 0) {
                err("split part %d failed:\n%s", part, diag);
                ok = 0;
                break;
            }

            if (!finalize_pdf(o, tmp, out, last - p + 1)) {
                ok = 0;
                break;
            }
            say("Saved: %s\n", out);
        }
    }

end:
    tmp_release_all();
    return ok;
}

static int process_text(const Opts *o, const char *arg)
{
    char input[PATH_MAX], out[PATH_MAX] = "", plain[PATH_MAX];
    struct stat st;
    int ok = 0, expect = 0;

    int to_stdout = o->output ? strcmp(o->output, "-") == 0 : !o->outdir;
    int saved_say = g_say_stderr;

    if (to_stdout)
        g_say_stderr = 1;

    if (!realpath(arg, input) || stat(input, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        err("cannot read input PDF '%s'", arg);
        g_say_stderr = saved_say;
        return 0;
    }

    if (!to_stdout) {
        if (!resolve_output(o, input, "", "txt", 0, out, sizeof(out)) ||
            !check_output(o, input, out)) {
            g_say_stderr = saved_say;
            return 0;
        }
    }

    if (!need_tool("pdftotext", "poppler-utils / poppler")) {
        g_say_stderr = saved_say;
        return 0;
    }

    if (!prepare_input(o, input, plain, sizeof(plain)))
        goto end;

    char *sel = tmp_file(tmp_base(), "pdfcrop-sel", ".pdf");
    if (!sel)
        goto end;

    if (o->dry_run) {
        ok = op_extract(o, plain, NULL, &expect) != 0;
        goto end;
    }

    if (!op_extract(o, plain, sel, &expect))
        goto end;

    char dir[PATH_MAX];
    if (to_stdout) {
        snprintf(dir, sizeof(dir), "%s", tmp_base());
    } else {
        path_dir(out, dir, sizeof(dir));
        if (!mkdir_p(dir)) {
            err("cannot create directory '%s'", dir);
            goto end;
        }
    }

    char *txt = tmp_file(dir, ".pdfcrop-txt", ".txt");
    if (!txt)
        goto end;

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pdftotext");
    if (o->layout)
        cmd_add(&c, "-layout");
    if (o->nopgbrk)
        cmd_add(&c, "-nopgbrk");
    cmd_add(&c, "-enc");
    cmd_add(&c, "UTF-8");
    cmd_add(&c, sel);
    cmd_add(&c, txt);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("text extraction failed:\n%s", diag);
        goto end;
    }

    if (to_stdout) {
        if (!copy_to_stdout(txt))
            goto end;
    } else {
        if (!publish_file(txt, out, o->force))
            goto end;
        say("Saved: %s\n", out);
    }

    ok = 1;

end:
    tmp_release_all();
    g_say_stderr = saved_say;
    return ok;
}

static int convert_to_images(const Opts *o, const char *src, const char *fmt,
                             const char *prefix)
{
    if (!need_tool("pdftoppm", "poppler-utils / poppler"))
        return 0;

    int dpi = o->dpi > 0 ? o->dpi : 150;
    char dpis[16];
    snprintf(dpis, sizeof(dpis), "%d", dpi);

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pdftoppm");
    cmd_addf(&c, "-%s", strcmp(fmt, "jpg") == 0 ? "jpeg" : fmt);
    cmd_add(&c, "-r");
    cmd_add(&c, dpis);
    if (strcmp(fmt, "jpg") == 0 && o->quality > 0) {
        cmd_add(&c, "-jpegopt");
        cmd_addf(&c, "quality=%d", o->quality);
    }
    cmd_add(&c, src);
    cmd_add(&c, prefix);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("conversion failed:\n%s", diag);
        return 0;
    }

    return 1;
}

static int convert_via_cairo(const char *src, const char *fmt,
                             const char *prefix)
{
    if (!need_tool("pdftocairo", "poppler-utils / poppler"))
        return 0;

    int n;
    if (!qpdf_page_count(src, &n))
        return 0;

    for (int p = 1; p <= n; p++) {
        Cmd c;
        char diag[DIAG_SIZE];

        cmd_init(&c);
        cmd_add(&c, "pdftocairo");
        cmd_addf(&c, "-%s", fmt);
        cmd_add(&c, "-f");
        cmd_addf(&c, "%d", p);
        cmd_add(&c, "-l");
        cmd_addf(&c, "%d", p);
        cmd_add(&c, src);
        cmd_addf(&c, "%s-%d.%s", prefix, p, fmt);

        if (execute(&c, diag, sizeof(diag)) != 0) {
            err("conversion of page %d failed:\n%s", p, diag);
            return 0;
        }
    }

    return 1;
}

static int convert_via_soffice(const char *src, const char *fmt,
                               const char *dest, int from_pdf)
{
    const char *bin = tool_exists("soffice") ? "soffice" :
                      tool_exists("libreoffice") ? "libreoffice" : NULL;

    if (!bin) {
        err("LibreOffice ('soffice') is needed for %s conversion", fmt);
        return 0;
    }

    char *work = tmp_dir(tmp_base(), "pdfcrop-lo");
    if (!work)
        return 0;

    char profile[PATH_MAX + 16];
    snprintf(profile, sizeof(profile), "%s/profile", work);

    Cmd c;
    char diag[DIAG_SIZE];

    cmd_init(&c);
    cmd_add(&c, bin);
    cmd_addf(&c, "-env:UserInstallation=file://%s", profile);
    cmd_add(&c, "--headless");
    if (from_pdf)
        cmd_add(&c, "--infilter=writer_pdf_import");
    cmd_add(&c, "--convert-to");
    cmd_add(&c, fmt);
    cmd_add(&c, "--outdir");
    cmd_addf(&c, "%s/out", work);
    cmd_add(&c, src);

    say("Converting with LibreOffice...\n");

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("LibreOffice conversion failed:\n%s", diag);
        return 0;
    }

    char pat[PATH_MAX + 16];
    snprintf(pat, sizeof(pat), "%s/out/*.%s", work, fmt);

    glob_t g = {0};
    if (glob(pat, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        err("LibreOffice produced no output");
        return 0;
    }

    int ok = rename(g.gl_pathv[0], dest) == 0;
    if (!ok) {
        size_t len;
        unsigned char *d = read_file(g.gl_pathv[0], &len);
        ok = d && write_file(dest, d, len);
        free(d);
    }

    globfree(&g);
    return ok;
}

static int process_convert(const Opts *o, const char *arg)
{
    static const char *fmts[] = {"png", "jpg", "tiff", "svg", "eps", "ps",
                                 "html", "txt", "pdfa", "docx", "odt", "rtf",
                                 NULL};
    char fmt[16] = "";
    char input[PATH_MAX], plain[PATH_MAX], stem[PATH_MAX];
    char target[PATH_MAX], dir[PATH_MAX];
    struct stat st;
    int ok = 0, expect = 0;

    if (!o->to) {
        err("convert needs --to FORMAT (png, jpg, tiff, svg, eps, ps, html, "
            "txt, pdfa, docx, odt, rtf)");
        return 0;
    }

    snprintf(fmt, sizeof(fmt), "%s", o->to);
    for (char *p = fmt; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    if (strcmp(fmt, "jpeg") == 0)
        strcpy(fmt, "jpg");
    if (strcmp(fmt, "tif") == 0)
        strcpy(fmt, "tiff");
    if (strcmp(fmt, "pdf/a") == 0)
        strcpy(fmt, "pdfa");

    int known = 0;
    for (int i = 0; fmts[i]; i++)
        known |= strcmp(fmts[i], fmt) == 0;

    if (!known) {
        err("unknown format '%s'", o->to);
        return 0;
    }

    int multi = !strcmp(fmt, "png") || !strcmp(fmt, "jpg") ||
                !strcmp(fmt, "tiff") || !strcmp(fmt, "svg") ||
                !strcmp(fmt, "eps");

    if (!realpath(arg, input) || stat(input, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        err("cannot read input PDF '%s'", arg);
        return 0;
    }

    file_stem(input, stem, sizeof(stem));

    const char *ext = strcmp(fmt, "pdfa") == 0 ? "pdf" : fmt;

    if (multi) {
        if (o->output) {
            snprintf(target, sizeof(target), "%s", o->output);
            size_t n = strlen(target);
            if (has_ext(target, ext))
                target[n - strlen(ext) - 1] = '\0';
        } else if (o->outdir) {
            snprintf(target, sizeof(target), "%s/%s", o->outdir, stem);
        } else {
            snprintf(target, sizeof(target), "%s", stem);
        }

        char pat[PATH_MAX + 32];
        snprintf(pat, sizeof(pat), "%s-*.%s", target, ext);
        glob_t g = {0};
        if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0 &&
            !o->force && !o->dry_run) {
            globfree(&g);
            err("files matching '%s' already exist (use --force)", pat);
            return 0;
        }
        globfree(&g);
    } else {
        if (!resolve_output(o, input, strcmp(fmt, "pdfa") == 0 ? "-pdfa" : "",
                            ext, 1, target, sizeof(target)) ||
            !check_output(o, input, target))
            return 0;
    }

    say("%s\n", arg);

    if (o->dry_run) {
        say("Dry run: would convert to %s -> %s%s\n", fmt, target,
            multi ? "-<page>" : "");
        return 1;
    }

    if (!prepare_input(o, input, plain, sizeof(plain)))
        goto end;

    const char *src = plain;

    if (o->pages && strcasecmp(o->pages, "all") != 0) {
        char *sel = tmp_file(tmp_base(), "pdfcrop-sel", ".pdf");
        if (!sel || !op_extract(o, plain, sel, &expect))
            goto end;
        src = sel;
    }

    path_dir(target, dir, sizeof(dir));
    if (!mkdir_p(dir)) {
        err("cannot create directory '%s'", dir);
        goto end;
    }

    if (!strcmp(fmt, "png") || !strcmp(fmt, "jpg") || !strcmp(fmt, "tiff")) {
        if (!convert_to_images(o, src, fmt, target))
            goto end;
        say("Wrote images: %s-<page>.%s\n", target, ext);

    } else if (!strcmp(fmt, "svg") || !strcmp(fmt, "eps")) {
        if (!convert_via_cairo(src, fmt, target))
            goto end;
        say("Wrote files: %s-<page>.%s\n", target, ext);

    } else {
        char *tmp = tmp_file(dir, ".pdfcrop", strcmp(fmt, "html") == 0 ? ".html"
                                                                        : ".out");
        if (!tmp)
            goto end;

        Cmd c;
        char diag[DIAG_SIZE];
        cmd_init(&c);

        if (!strcmp(fmt, "txt")) {
            if (!need_tool("pdftotext", "poppler-utils / poppler"))
                goto end;
            cmd_add(&c, "pdftotext");
            if (o->layout)
                cmd_add(&c, "-layout");
            cmd_add(&c, "-enc");
            cmd_add(&c, "UTF-8");
            cmd_add(&c, src);
            cmd_add(&c, tmp);

        } else if (!strcmp(fmt, "ps")) {
            if (!need_tool("pdftocairo", "poppler-utils / poppler"))
                goto end;
            cmd_add(&c, "pdftocairo");
            cmd_add(&c, "-ps");
            cmd_add(&c, src);
            cmd_add(&c, tmp);

        } else if (!strcmp(fmt, "html")) {
            if (!need_tool("pdftohtml", "poppler-utils / poppler"))
                goto end;
            cmd_add(&c, "pdftohtml");
            cmd_add(&c, "-q");
            cmd_add(&c, "-s");
            cmd_add(&c, "-noframes");
            cmd_add(&c, "-i");
            cmd_add(&c, "-enc");
            cmd_add(&c, "UTF-8");
            cmd_add(&c, src);
            cmd_add(&c, tmp);

        } else if (!strcmp(fmt, "pdfa")) {
            if (!need_tool("gs", "ghostscript"))
                goto end;
            cmd_add(&c, "gs");
            cmd_add(&c, "-dPDFA=2");
            cmd_add(&c, "-dBATCH");
            cmd_add(&c, "-dNOPAUSE");
            cmd_add(&c, "-dQUIET");
            cmd_add(&c, "-dNOOUTERSAVE");
            cmd_add(&c, "-dPDFACompatibilityPolicy=1");
            cmd_add(&c, "-sColorConversionStrategy=UseDeviceIndependentColor");
            cmd_add(&c, "-sDEVICE=pdfwrite");
            cmd_addf(&c, "-sOutputFile=%s", tmp);
            cmd_add(&c, src);

        } else {
            tmp_release(tmp);
            if (!convert_via_soffice(src, fmt, target, 1))
                goto end;
            say("Saved: %s\n", target);
            ok = 1;
            goto end;
        }

        if (execute(&c, diag, sizeof(diag)) != 0) {
            err("conversion failed:\n%s", diag);
            goto end;
        }

        if (!strcmp(fmt, "html")) {
            char base[PATH_MAX];
            snprintf(base, sizeof(base), "%s", tmp);
            char *dot = strrchr(base, '.');
            if (dot)
                *dot = '\0';

            char produced[PATH_MAX + 16];
            snprintf(produced, sizeof(produced), "%s-html.html", base);
            struct stat ps;

            if (stat(produced, &ps) == 0) {
                unlink(tmp);
                snprintf(tmp, PATH_MAX, "%s", produced);
            } else if (stat(tmp, &ps) != 0 || ps.st_size == 0) {
                err("pdftohtml produced no output");
                goto end;
            }
        }

        if (!publish_file(tmp, target, o->force))
            goto end;
        say("Saved: %s\n", target);
    }

    ok = 1;

end:
    tmp_release_all();
    return ok;
}

static int is_image_ext(const char *path)
{
    static const char *const exts[] = {
        "jpg", "jpeg", "png", "bmp", "gif", "webp", "tif", "tiff", NULL
    };
    for (int i = 0; exts[i]; i++) {
        if (has_ext(path, exts[i]))
            return 1;
    }
    return 0;
}

static int cmd_create(const Opts *o, int n, char **args)
{
    char out[PATH_MAX], dir[PATH_MAX];
    char (*pdfs)[PATH_MAX] = calloc((size_t)n, PATH_MAX);
    int ok = 0;

    if (!pdfs)
        return EXIT_ERR;

    if (o->output) {
        snprintf(out, sizeof(out), "%s%s", o->output,
                 has_ext(o->output, "pdf") ? "" : ".pdf");
    } else if (o->outdir) {
        snprintf(out, sizeof(out), "%s/created.pdf", o->outdir);
    } else {
        snprintf(out, sizeof(out), "created.pdf");
    }

    if (!check_output(o, NULL, out))
        goto end;

    if (o->dry_run) {
        say("Dry run: would create %s from %d file%s\n",
            out, n, n == 1 ? "" : "s");
        ok = 1;
        goto end;
    }

    for (int i = 0; i < n; i++) {
        char input[PATH_MAX];
        struct stat st;

        if (!realpath(args[i], input) || stat(input, &st) != 0 ||
            !S_ISREG(st.st_mode)) {
            err("cannot read input '%s'", args[i]);
            goto end;
        }

        if (has_ext(input, "pdf")) {
            if (!prepare_input(o, input, pdfs[i], PATH_MAX))
                goto end;
        } else if (is_image_ext(input)) {
            const char *jpg = ensure_jpeg(input);
            char *pg = tmp_file(tmp_base(), "pdfcrop-imgpdf", ".pdf");
            if (!jpg || !pg || !make_image_pdf(jpg, pg))
                goto end;
            snprintf(pdfs[i], PATH_MAX, "%s", pg);
        } else {
            char *doc = tmp_file(tmp_base(), "pdfcrop-docpdf", ".pdf");
            if (!doc || !convert_via_soffice(input, "pdf", doc, 0))
                goto end;
            snprintf(pdfs[i], PATH_MAX, "%s", doc);
        }
    }

    path_dir(out, dir, sizeof(dir));
    if (!mkdir_p(dir)) {
        err("cannot create directory '%s'", dir);
        goto end;
    }

    char *tmp = tmp_file(dir, ".pdfcrop", ".pdf");
    if (!tmp)
        goto end;

    Cmd *c = malloc(sizeof(Cmd));
    if (!c)
        goto end;

    cmd_init(c);
    cmd_add(c, "qpdf");
    cmd_add(c, "--warning-exit-0");
    if (o->linearize)
        cmd_add(c, "--linearize");
    cmd_add(c, "--empty");
    cmd_add(c, "--pages");
    for (int i = 0; i < n; i++)
        cmd_add(c, pdfs[i]);
    cmd_add(c, "--");
    cmd_add(c, tmp);

    char diag[DIAG_SIZE];
    int st = execute(c, diag, sizeof(diag));
    free(c);

    if (st != 0) {
        err("create failed:\n%s", diag);
        goto end;
    }

    if (!finalize_pdf(o, tmp, out, 0))
        goto end;

    say("Saved: %s\n", out);
    ok = 1;

end:
    free(pdfs);
    tmp_release_all();
    return ok ? EXIT_OK : EXIT_ERR;
}

static int process_verify(const Opts *o, const char *arg)
{
    (void)o;

    if (!need_tool("pdfsig", "poppler-utils / poppler"))
        return 0;

    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pdfsig");
    cmd_add(&c, arg);

    return execute_inherit(&c) == 0;
}

static int process_info(const Opts *o, const char *arg)
{
    char input[PATH_MAX], plain[PATH_MAX];
    struct stat st;
    int ok = 0, total = 0;

    if (!realpath(arg, input) || stat(input, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        err("cannot read input PDF '%s'", arg);
        return 0;
    }

    int pwst = requires_password(input, NULL);
    int encrypted = (pwst == 0 || pwst == 3);

    if (!prepare_input(o, input, plain, sizeof(plain)))
        goto end;

    if (!qpdf_page_count(plain, &total))
        goto end;

    char sz[32];
    fmt_size((double)st.st_size, sz, sizeof(sz));

    printf("File:      %s\n", input);
    printf("Size:      %s\n", sz);
    printf("Pages:     %d\n", total);
    printf("Encrypted: %s\n", encrypted ? "yes" : "no");

    if (tool_exists("pdfinfo")) {
        Cmd c;
        char out[DIAG_SIZE];
        cmd_init(&c);
        cmd_add(&c, "pdfinfo");
        cmd_add(&c, "-f");
        cmd_add(&c, "1");
        cmd_add(&c, "-l");
        cmd_add(&c, "1");
        cmd_add(&c, plain);

        if (execute(&c, out, sizeof(out)) == 0) {
            const char *keys[] = {"Title:", "Author:", "Creator:", "Producer:",
                                  "PDF version:", "Page    1 size:", NULL};
            for (int k = 0; keys[k]; k++) {
                char *line = strstr(out, keys[k]);
                if (line) {
                    char *nl = strchr(line, '\n');
                    if (nl)
                        *nl = '\0';
                    printf("%s\n", line);
                    if (nl)
                        *nl = '\n';
                }
            }
        }
    }

    ok = 1;

end:
    tmp_release_all();
    return ok;
}

static int valid_page_spec(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr(",;-", *s))
            return 0;
    }
    return 1;
}

static int cmd_merge(const Opts *o, int n, char **args)
{
    char out[PATH_MAX], dir[PATH_MAX];
    char (*plain)[PATH_MAX] = calloc((size_t)n, PATH_MAX);
    char **expr = calloc((size_t)n, sizeof(char *));
    int expect = 0, ok = 0;
    char *tmp;

    if (!plain || !expr) {
        free(plain);
        free(expr);
        return EXIT_ERR;
    }

    if (o->output) {
        snprintf(out, sizeof(out), "%s%s", o->output,
                 has_ext(o->output, "pdf") ? "" : ".pdf");
    } else if (o->outdir) {
        snprintf(out, sizeof(out), "%s/merged.pdf", o->outdir);
    } else {
        snprintf(out, sizeof(out), "merged.pdf");
    }

    if (!check_output(o, NULL, out))
        goto end;

    for (int i = 0; i < n; i++) {
        char file[PATH_MAX], input[PATH_MAX];
        const char *spec = NULL;

        snprintf(file, sizeof(file), "%s", args[i]);

        if (access(args[i], F_OK) != 0) {
            const char *colon = strrchr(args[i], ':');
            if (colon && colon != args[i] && valid_page_spec(colon + 1)) {
                snprintf(file, sizeof(file), "%.*s",
                         (int)(colon - args[i]), args[i]);
                spec = colon + 1;
            }
        }

        struct stat st, os;
        if (!realpath(file, input) || stat(input, &st) != 0 ||
            !S_ISREG(st.st_mode)) {
            err("cannot read input PDF '%s'", file);
            goto end;
        }

        if (stat(out, &os) == 0 && os.st_dev == st.st_dev &&
            os.st_ino == st.st_ino) {
            err("output would overwrite input '%s'", file);
            goto end;
        }

        if (!prepare_input(o, input, plain[i], PATH_MAX))
            goto end;

        int total;
        if (!qpdf_page_count(plain[i], &total))
            goto end;

        PageList pl = {0};
        if (!parse_selection(spec ? spec : "all", total, &pl)) {
            pages_free(&pl);
            goto end;
        }

        expr[i] = build_expression(&pl);
        expect += (int)pl.count;
        pages_free(&pl);

        if (!expr[i]) {
            err("page selection for '%s' is too complex", file);
            goto end;
        }

        say("  %s%s%s (%s pages)\n", file, spec ? ":" : "", spec ? spec : "",
            spec ? "selected" : "all");
    }

    say("Merging %d files, %d pages total\n", n, expect);

    if (o->dry_run) {
        say("Dry run: would write %s\n", out);
        ok = 1;
        goto end;
    }

    path_dir(out, dir, sizeof(dir));
    if (!mkdir_p(dir)) {
        err("cannot create directory '%s'", dir);
        goto end;
    }

    tmp = tmp_file(dir, ".pdfcrop", ".pdf");
    if (!tmp)
        goto end;

    Cmd *c = malloc(sizeof(Cmd));
    if (!c)
        goto end;

    cmd_init(c);
    cmd_add(c, "qpdf");
    cmd_add(c, "--warning-exit-0");
    if (o->linearize)
        cmd_add(c, "--linearize");
    cmd_add(c, "--empty");
    cmd_add(c, "--pages");
    for (int i = 0; i < n; i++) {
        cmd_add(c, plain[i]);
        cmd_add(c, expr[i]);
    }
    cmd_add(c, "--");
    cmd_add(c, tmp);

    char diag[DIAG_SIZE];
    int st = execute(c, diag, sizeof(diag));
    free(c);

    if (st != 0) {
        err("merge failed:\n%s", diag);
        goto end;
    }

    if (!finalize_pdf(o, tmp, out, expect))
        goto end;

    say("Saved: %s\n", out);
    ok = 1;

end:
    for (int i = 0; i < n; i++)
        free(expr[i]);
    free(expr);
    free(plain);
    tmp_release_all();
    return ok ? EXIT_OK : EXIT_ERR;
}

typedef int (*FileFn)(const Opts *o, const char *file);

static int run_files(const Opts *o, char **files, int nfiles, FileFn fn)
{
    int ok = 0, failed = 0, started = 0;

    if (nfiles <= 1 || o->jobs <= 1) {
        for (int i = 0; i < nfiles; i++) {
            if (fn(o, files[i]))
                ok++;
            else {
                failed++;
                if (!o->keep_going)
                    break;
            }
            started++;
        }
    } else {
        int running = 0;
        int next = 0;
        int stop = 0;

        g_noprompt = 1;

        while (next < nfiles || running > 0) {
            while (!stop && next < nfiles && running < o->jobs) {
                fflush(NULL);
                pid_t pid = fork();

                if (pid < 0) {
                    perror(PROG ": fork");
                    failed++;
                    stop = 1;
                    break;
                }

                if (pid == 0) {
                    int r = fn(o, files[next]);
                    fflush(NULL);
                    _exit(r ? 0 : 1);
                }

                running++;
                next++;
                started++;
            }

            if (running == 0)
                break;

            int status;
            pid_t w = waitpid(-1, &status, 0);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }

            running--;

            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                ok++;
            else {
                failed++;
                if (!o->keep_going)
                    stop = 1;
            }
        }
    }

    if (nfiles > 1) {
        say("\nProcessed %d file%s: %d ok, %d failed", started,
            started == 1 ? "" : "s", ok, failed);
        if (started < nfiles)
            say(", %d not attempted", nfiles - started);
        say("\n");
    }

    return failed ? EXIT_ERR : EXIT_OK;
}

static void usage(FILE *out)
{
    fprintf(out,
"pdfcrop " VERSION " - a command-line PDF toolkit\n"
"\n"
"Usage: " PROG " COMMAND [OPTIONS] FILE...\n"
"       " PROG " [OPTIONS] INPUT.pdf PAGES [OUTPUT]     (same as 'extract')\n"
"\n"
"Commands:\n"
"  extract    keep, reorder or reverse pages          extract INPUT PAGES\n"
"  remove     delete selected pages                   remove INPUT PAGES\n"
"  split      split into chunks or ranges             split --every 2 FILE...\n"
"  merge      combine PDFs (FILE or FILE:PAGES)       merge a.pdf b.pdf:1-3 -o all.pdf\n"
"  rotate     rotate pages by 90/180/270 deg          rotate --angle 90 FILE...\n"
"  crop       trim page margins (in pt)               crop --margins 36 FILE...\n"
"  pagenum    add page numbers                        pagenum --position bottom-right FILE...\n"
"  compress   shrink PDFs                             compress --level medium FILE...\n"
"  repair     rebuild damaged PDFs                    repair FILE...\n"
"  convert    PDF to png/jpg/tiff/svg/eps/ps/html/txt/pdfa/docx/odt/rtf\n"
"  create     images or Office/HTML/txt to PDF        create a.jpg b.png -o out.pdf\n"
"  text       dump the text of selected pages         text INPUT PAGES\n"
"  encrypt    add a password and permissions          encrypt FILE...\n"
"  decrypt    remove the password                     decrypt FILE...\n"
"  edit       replace text in a PDF (best effort)     edit --replace 'old=>new' FILE\n"
"  ocr        make scanned PDFs searchable            ocr --lang eng FILE...\n"
"  watermark  stamp text / image / PDF on pages       watermark --stamp-text DRAFT FILE...\n"
"  sign       visible and/or cryptographic signature  sign --stamp-image sig.png FILE\n"
"  verify     check digital signatures                verify FILE...\n"
"  info       page count, size, encryption, metadata  info FILE...\n"
"\n"
"Page selection (extract, remove, rotate, text, convert, watermark, pagenum, sign, merge):\n"
"  3   1-10   10-1 (reversed)   last   5-last   all   odd   even   1-3,7,9-last\n"
"\n"
"Common options:\n"
"  -o, --output FILE        output file (one input only; '-' = stdout for text)\n"
"  -O, --outdir DIR         write results into DIR (created if missing)\n"
"      --suffix TEXT        output name suffix (default depends on command)\n"
"      --in-place           replace the original file (atomically)\n"
"  -f, --force              overwrite existing outputs\n"
"  -n, --dry-run            show what would happen, write nothing\n"
"  -j, --jobs N             process N files in parallel\n"
"  -k, --keep-going         continue after a failed file\n"
"      --from-list FILE     read more input names from FILE ('-' = stdin)\n"
"      --password-file F    password of the INPUT pdf, first line of F\n"
"      --password-env VAR   ... taken from environment variable VAR\n"
"      --password-stdin     ... read from stdin (otherwise prompted)\n"
"      --no-verify          skip the qpdf --check of results\n"
"  -q, --quiet   -v, --verbose   -h, --help   -V, --version\n"
"\n"
"extract / remove / split / rotate / crop / pagenum / text / convert / merge:\n"
"      --pages EXPR         page selection (then all positionals are files)\n"
"      --every N            split: pages per output file (default 1)\n"
"      --ranges SPEC        split: semicolon-separated ranges ('1-3;4-last')\n"
"      --margins SPEC       crop: points to trim (ALL | V,H | T,R,B,L)\n"
"      --format TEXT        pagenum: format string (default '{n} / {total}')\n"
"      --start N            pagenum: starting number (default 1)\n"
"  -t, --text               (legacy) same as the 'text' command\n"
"  -i, --info               (legacy) same as the 'info' command\n"
"      --layout             text: keep physical layout\n"
"      --no-page-breaks     text: no form feed between pages\n"
"      --linearize          write web-optimised PDFs\n"
"\n"
"compress:   --level lossless|low|medium|high   --dpi N   --grayscale\n"
"convert:    --to FORMAT   --dpi N (images, default 150)   --quality N (jpg)\n"
"encrypt:    --user-password-file F | --user-password-env V (else prompted)\n"
"            --owner-password-file F | --owner-password-env V\n"
"            --deny print,copy,modify,annotate|all    --bits 128|256\n"
"edit:       --replace 'OLD=>NEW'   (repeatable)\n"
"ocr:        --lang CODES (eng, deu+eng)   --engine auto|ocrmypdf|tesseract\n"
"            --sidecar FILE.txt   --deskew   --rotate   --force-ocr\n"
"watermark / sign / pagenum / rotate:\n"
"            --stamp-text TEXT ({date} works)  --stamp-image JPG/PNG  --stamp-pdf FILE\n"
"            --on PAGES   --layer over|under\n"
"            --position center|top|bottom|left|right|top-left|top-right|\n"
"                       bottom-left|bottom-right\n"
"            --size PT  --angle DEG  --opacity 0-1  --scale F  --color NAME|#RRGGBB\n"
"sign (cryptographic, needs pyHanko):\n"
"            --cert FILE.p12   --cert-pass-file F | --cert-pass-env V\n"
"            --reason TEXT  --location TEXT  --field NAME\n"
"\n"
"Exit status: 0 success, 1 error, 2 usage error.\n");
}

enum {
    O_PWFILE = 1000, O_PWENV, O_PWSTDIN, O_LINEARIZE, O_NOVERIFY, O_LAYOUT,
    O_NOPGBRK, O_PAGES, O_SUFFIX, O_FROMLIST, O_INPLACE, O_LEVEL, O_DPI,
    O_GRAY, O_TO, O_QUALITY, O_UPWFILE, O_UPWENV, O_OPWFILE, O_OPWENV,
    O_DENY, O_BITS, O_REPLACE, O_LANG, O_ENGINE, O_SIDECAR, O_DESKEW,
    O_ROTATE, O_FORCEOCR, O_STTEXT, O_STIMAGE, O_STPDF, O_STCOLOR, O_STPOS,
    O_STON, O_STLAYER, O_STSIZE, O_STANGLE, O_STOPACITY, O_STSCALE, O_CERT,
    O_CERTPF, O_CERTPE, O_REASON, O_LOCATION, O_FIELD, O_EVERY, O_RANGES,
    O_FORMAT, O_START, O_MARGINS
};

static int parse_int(const char *s, int *v, int min, const char *name)
{
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);

    if (errno || *end || end == s || n < min || n > INT_MAX) {
        err("invalid value '%s' for %s", s, name);
        return 0;
    }

    *v = (int)n;
    return 1;
}

static int parse_double(const char *s, double *v, const char *name)
{
    char *end;
    errno = 0;
    double d = strtod(s, &end);

    if (errno || *end || end == s || !isfinite(d)) {
        err("invalid value '%s' for %s", s, name);
        return 0;
    }

    *v = d;
    return 1;
}

static int parse_options(int argc, char **argv, Opts *o)
{
    static const struct option longopts[] = {
        {"output", required_argument, 0, 'o'},
        {"outdir", required_argument, 0, 'O'},
        {"force", no_argument, 0, 'f'},
        {"dry-run", no_argument, 0, 'n'},
        {"info", no_argument, 0, 'i'},
        {"text", no_argument, 0, 't'},
        {"quiet", no_argument, 0, 'q'},
        {"verbose", no_argument, 0, 'v'},
        {"jobs", required_argument, 0, 'j'},
        {"keep-going", no_argument, 0, 'k'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {"password-file", required_argument, 0, O_PWFILE},
        {"password-env", required_argument, 0, O_PWENV},
        {"password-stdin", no_argument, 0, O_PWSTDIN},
        {"linearize", no_argument, 0, O_LINEARIZE},
        {"no-verify", no_argument, 0, O_NOVERIFY},
        {"layout", no_argument, 0, O_LAYOUT},
        {"no-page-breaks", no_argument, 0, O_NOPGBRK},
        {"pages", required_argument, 0, O_PAGES},
        {"suffix", required_argument, 0, O_SUFFIX},
        {"from-list", required_argument, 0, O_FROMLIST},
        {"in-place", no_argument, 0, O_INPLACE},
        {"level", required_argument, 0, O_LEVEL},
        {"dpi", required_argument, 0, O_DPI},
        {"grayscale", no_argument, 0, O_GRAY},
        {"to", required_argument, 0, O_TO},
        {"quality", required_argument, 0, O_QUALITY},
        {"user-password-file", required_argument, 0, O_UPWFILE},
        {"user-password-env", required_argument, 0, O_UPWENV},
        {"owner-password-file", required_argument, 0, O_OPWFILE},
        {"owner-password-env", required_argument, 0, O_OPWENV},
        {"deny", required_argument, 0, O_DENY},
        {"bits", required_argument, 0, O_BITS},
        {"replace", required_argument, 0, O_REPLACE},
        {"lang", required_argument, 0, O_LANG},
        {"engine", required_argument, 0, O_ENGINE},
        {"sidecar", required_argument, 0, O_SIDECAR},
        {"deskew", no_argument, 0, O_DESKEW},
        {"rotate", no_argument, 0, O_ROTATE},
        {"force-ocr", no_argument, 0, O_FORCEOCR},
        {"stamp-text", required_argument, 0, O_STTEXT},
        {"stamp-image", required_argument, 0, O_STIMAGE},
        {"stamp-pdf", required_argument, 0, O_STPDF},
        {"color", required_argument, 0, O_STCOLOR},
        {"position", required_argument, 0, O_STPOS},
        {"on", required_argument, 0, O_STON},
        {"layer", required_argument, 0, O_STLAYER},
        {"size", required_argument, 0, O_STSIZE},
        {"angle", required_argument, 0, O_STANGLE},
        {"opacity", required_argument, 0, O_STOPACITY},
        {"scale", required_argument, 0, O_STSCALE},
        {"cert", required_argument, 0, O_CERT},
        {"cert-pass-file", required_argument, 0, O_CERTPF},
        {"cert-pass-env", required_argument, 0, O_CERTPE},
        {"reason", required_argument, 0, O_REASON},
        {"location", required_argument, 0, O_LOCATION},
        {"field", required_argument, 0, O_FIELD},
        {"every", required_argument, 0, O_EVERY},
        {"ranges", required_argument, 0, O_RANGES},
        {"format", required_argument, 0, O_FORMAT},
        {"start", required_argument, 0, O_START},
        {"margins", required_argument, 0, O_MARGINS},
        {0, 0, 0, 0}
    };

    int c;

    while ((c = getopt_long(argc, argv, "o:O:fnitqvj:khV", longopts, NULL)) != -1) {
        switch (c) {
        case 'o': o->output = optarg; break;
        case 'O': o->outdir = optarg; break;
        case 'f': o->force = 1; break;
        case 'n': o->dry_run = 1; break;
        case 'i': o->info = 1; break;
        case 't': o->text = 1; break;
        case 'q': g_quiet = 1; break;
        case 'v': g_verbose = 1; break;
        case 'j': if (!parse_int(optarg, &o->jobs, 1, "--jobs")) return 0; break;
        case 'k': o->keep_going = 1; break;
        case 'h': usage(stdout); exit(EXIT_OK);
        case 'V': printf(PROG " " VERSION "\n"); exit(EXIT_OK);
        case O_PWFILE: o->pw_file = optarg; break;
        case O_PWENV: o->pw_env = optarg; break;
        case O_PWSTDIN: o->pw_stdin = 1; break;
        case O_LINEARIZE: o->linearize = 1; break;
        case O_NOVERIFY: o->verify = 0; break;
        case O_LAYOUT: o->layout = 1; break;
        case O_NOPGBRK: o->nopgbrk = 1; break;
        case O_PAGES: o->pages = optarg; break;
        case O_SUFFIX: o->suffix = optarg; break;
        case O_FROMLIST: o->from_list = optarg; break;
        case O_INPLACE: o->in_place = 1; break;
        case O_LEVEL: o->level = optarg; break;
        case O_DPI: if (!parse_int(optarg, &o->dpi, 10, "--dpi")) return 0; break;
        case O_GRAY: o->grayscale = 1; break;
        case O_TO: o->to = optarg; break;
        case O_QUALITY: if (!parse_int(optarg, &o->quality, 1, "--quality")) return 0; break;
        case O_UPWFILE: o->upw_file = optarg; break;
        case O_UPWENV: o->upw_env = optarg; break;
        case O_OPWFILE: o->opw_file = optarg; break;
        case O_OPWENV: o->opw_env = optarg; break;
        case O_DENY: o->deny = optarg; break;
        case O_BITS: if (!parse_int(optarg, &o->bits, 1, "--bits")) return 0; break;
        case O_REPLACE:
            if (o->nrepl >= MAX_REPL) {
                err("too many --replace options (max %d)", MAX_REPL);
                return 0;
            }
            o->repl[o->nrepl++] = optarg;
            break;
        case O_LANG: o->lang = optarg; break;
        case O_ENGINE: o->engine = optarg; break;
        case O_SIDECAR: o->sidecar = optarg; break;
        case O_DESKEW: o->deskew = 1; break;
        case O_ROTATE: o->rotate = 1; break;
        case O_FORCEOCR: o->force_ocr = 1; break;
        case O_STTEXT: o->st_text = optarg; break;
        case O_STIMAGE: o->st_image = optarg; break;
        case O_STPDF: o->st_pdf = optarg; break;
        case O_STCOLOR: o->st_color = optarg; break;
        case O_STPOS: o->st_pos = optarg; break;
        case O_STON: o->st_on = optarg; break;
        case O_STLAYER: o->st_layer = optarg; break;
        case O_STSIZE: if (!parse_double(optarg, &o->st_size, "--size")) return 0; break;
        case O_STANGLE: if (!parse_double(optarg, &o->st_angle, "--angle")) return 0; break;
        case O_STOPACITY: if (!parse_double(optarg, &o->st_opacity, "--opacity")) return 0; break;
        case O_STSCALE: if (!parse_double(optarg, &o->st_scale, "--scale")) return 0; break;
        case O_CERT: o->cert = optarg; break;
        case O_CERTPF: o->cert_pass_file = optarg; break;
        case O_CERTPE: o->cert_pass_env = optarg; break;
        case O_REASON: o->reason = optarg; break;
        case O_LOCATION: o->location = optarg; break;
        case O_FIELD: o->field = optarg; break;
        case O_EVERY: if (!parse_int(optarg, &o->split_every, 1, "--every")) return 0; break;
        case O_RANGES: o->split_ranges = optarg; break;
        case O_FORMAT: o->num_format = optarg; break;
        case O_START: if (!parse_int(optarg, &o->num_start, 1, "--start")) return 0; break;
        case O_MARGINS: o->margins = optarg; break;
        default: usage(stderr); return 0;
        }
    }

    return 1;
}

static int load_list(const char *path, char ***files, int n)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");

    if (!f) {
        err("cannot read list '%s': %s", path, strerror(errno));
        return -1;
    }

    char line[PATH_MAX + 2];
    int cap = n + 64;
    char **v = realloc(*files, (size_t)cap * sizeof(char *));
    if (!v)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;

        if (n >= cap) {
            cap *= 2;
            char **nv = realloc(v, (size_t)cap * sizeof(char *));
            if (!nv)
                return -1;
            v = nv;
        }

        v[n++] = strdup(s);
    }

    if (f != stdin)
        fclose(f);

    *files = v;
    return n;
}

static const char *const commands[] = {
    "extract", "remove", "split", "merge", "rotate", "crop", "pagenum",
    "compress", "repair", "convert", "create", "text", "encrypt", "decrypt",
    "edit", "ocr", "watermark", "sign", "verify", "info", "help", NULL
};

static int is_command(const char *s)
{
    for (int i = 0; commands[i]; i++) {
        if (strcmp(commands[i], s) == 0)
            return access(s, F_OK) != 0;
    }
    return 0;
}

static int f_extract(const Opts *o, const char *f)   { return process_file(o, f, "-pages", op_extract); }
static int f_remove(const Opts *o, const char *f)    { return process_file(o, f, "-trimmed", op_remove); }
static int f_rotate(const Opts *o, const char *f)    { return process_file(o, f, "-rotated", op_rotate); }
static int f_crop(const Opts *o, const char *f)      { return process_file(o, f, "-cropped", op_crop); }
static int f_pagenum(const Opts *o, const char *f)   { return process_file(o, f, "-numbered", op_pagenum); }
static int f_compress(const Opts *o, const char *f)  { return process_file(o, f, "-compressed", op_compress); }
static int f_repair(const Opts *o, const char *f)    { return process_file(o, f, "-repaired", op_repair); }
static int f_encrypt(const Opts *o, const char *f)   { return process_file(o, f, "-encrypted", op_encrypt); }
static int f_decrypt(const Opts *o, const char *f)   { return process_file(o, f, "-decrypted", op_decrypt); }
static int f_edit(const Opts *o, const char *f)      { return process_file(o, f, "-edited", op_edit); }
static int f_ocr(const Opts *o, const char *f)       { return process_file(o, f, "-ocr", op_ocr); }
static int f_watermark(const Opts *o, const char *f) { return process_file(o, f, "-watermarked", op_watermark); }
static int f_sign(const Opts *o, const char *f)      { return process_file(o, f, "-signed", op_sign); }

int main(int argc, char **argv)
{
    Opts o;
    memset(&o, 0, sizeof(o));
    o.verify = 1;
    o.jobs = 1;
    o.st_angle = o.st_opacity = o.st_scale = NAN;

    g_color = isatty(STDERR_FILENO) && !getenv("NO_COLOR");

    atexit(cleanup);
    install_signals();

    if (!parse_options(argc, argv, &o))
        return EXIT_USAGE;

    if (g_quiet && g_verbose) {
        err("--quiet and --verbose are mutually exclusive");
        return EXIT_USAGE;
    }

    int npos = argc - optind;
    char **pos = argv + optind;
    const char *cmd;

    if (npos > 0 && is_command(pos[0])) {
        cmd = pos[0];
        pos++;
        npos--;
    } else if (o.info) {
        cmd = "info";
    } else if (o.text) {
        cmd = "text";
    } else {
        cmd = "extract";
    }

    if (strcmp(cmd, "help") == 0) {
        usage(stdout);
        return EXIT_OK;
    }

    int sources = (o.pw_file != NULL) + (o.pw_env != NULL) + o.pw_stdin;
    if (sources > 1) {
        err("use only one of --password-file, --password-env, --password-stdin");
        return EXIT_USAGE;
    }

    if ((o.layout || o.nopgbrk) && strcmp(cmd, "text") && strcmp(cmd, "convert")) {
        err("--layout and --no-page-breaks only apply to text output");
        return EXIT_USAGE;
    }

    if (o.in_place && (!strcmp(cmd, "text") || !strcmp(cmd, "convert") ||
                       !strcmp(cmd, "merge") || !strcmp(cmd, "create") ||
                       !strcmp(cmd, "split") || !strcmp(cmd, "info") ||
                       !strcmp(cmd, "verify"))) {
        err("--in-place does not apply to '%s'", cmd);
        return EXIT_USAGE;
    }

    if (o.in_place && (o.output || o.outdir)) {
        err("--in-place cannot be combined with --output / --outdir");
        return EXIT_USAGE;
    }

    char **files = NULL;
    int nfiles = 0;

    if (!strcmp(cmd, "extract") || !strcmp(cmd, "remove") || !strcmp(cmd, "text")) {
        if (o.pages) {
            files = pos;
            nfiles = npos;
        } else {
            if (npos < 2 || npos > 3) {
                if (npos == 1 && o.from_list == NULL) {
                    err("missing page selection (e.g. 1-5, last, odd)");
                } else {
                    usage(stderr);
                }
                return EXIT_USAGE;
            }
            o.pages = pos[1];
            files = pos;
            nfiles = 1;
            if (npos == 3) {
                if (o.output) {
                    err("output given twice (-o and positional)");
                    return EXIT_USAGE;
                }
                o.output = pos[2];
            }
        }
    } else {
        files = pos;
        nfiles = npos;
    }

    char **list = malloc(((size_t)nfiles + 1) * sizeof(char *));
    if (!list)
        return EXIT_ERR;
    for (int i = 0; i < nfiles; i++)
        list[i] = files[i];

    if (o.from_list) {
        nfiles = load_list(o.from_list, &list, nfiles);
        if (nfiles < 0)
            return EXIT_ERR;
    }

    if (nfiles == 0) {
        err("no input files given (try '" PROG " --help')");
        return EXIT_USAGE;
    }

    int to_stdout = !strcmp(cmd, "text") &&
                    (o.output ? strcmp(o.output, "-") == 0 : !o.outdir);

    if (o.output && nfiles > 1 && strcmp(cmd, "merge") &&
        strcmp(cmd, "create") && !to_stdout) {
        err("-o works with a single input; use --outdir for several files");
        return EXIT_USAGE;
    }

    if (to_stdout && nfiles > 1 && o.jobs > 1) {
        err("text to stdout cannot be combined with --jobs");
        return EXIT_USAGE;
    }

    int needs_qpdf = strcmp(cmd, "verify") != 0;

    if (needs_qpdf && !need_tool("qpdf", "https://qpdf.sourceforge.io/"))
        return EXIT_ERR;

    int rc;

    if (!strcmp(cmd, "extract"))
        rc = run_files(&o, list, nfiles, f_extract);
    else if (!strcmp(cmd, "remove"))
        rc = run_files(&o, list, nfiles, f_remove);
    else if (!strcmp(cmd, "split"))
        rc = run_files(&o, list, nfiles, process_split);
    else if (!strcmp(cmd, "merge"))
        rc = cmd_merge(&o, nfiles, list);
    else if (!strcmp(cmd, "rotate"))
        rc = run_files(&o, list, nfiles, f_rotate);
    else if (!strcmp(cmd, "crop"))
        rc = run_files(&o, list, nfiles, f_crop);
    else if (!strcmp(cmd, "pagenum"))
        rc = run_files(&o, list, nfiles, f_pagenum);
    else if (!strcmp(cmd, "compress"))
        rc = run_files(&o, list, nfiles, f_compress);
    else if (!strcmp(cmd, "repair"))
        rc = run_files(&o, list, nfiles, f_repair);
    else if (!strcmp(cmd, "convert"))
        rc = run_files(&o, list, nfiles, process_convert);
    else if (!strcmp(cmd, "create"))
        rc = cmd_create(&o, nfiles, list);
    else if (!strcmp(cmd, "text"))
        rc = run_files(&o, list, nfiles, process_text);
    else if (!strcmp(cmd, "encrypt"))
        rc = run_files(&o, list, nfiles, f_encrypt);
    else if (!strcmp(cmd, "decrypt"))
        rc = run_files(&o, list, nfiles, f_decrypt);
    else if (!strcmp(cmd, "edit"))
        rc = run_files(&o, list, nfiles, f_edit);
    else if (!strcmp(cmd, "ocr"))
        rc = run_files(&o, list, nfiles, f_ocr);
    else if (!strcmp(cmd, "watermark"))
        rc = run_files(&o, list, nfiles, f_watermark);
    else if (!strcmp(cmd, "sign"))
        rc = run_files(&o, list, nfiles, f_sign);
    else if (!strcmp(cmd, "verify"))
        rc = run_files(&o, list, nfiles, process_verify);
    else if (!strcmp(cmd, "info"))
        rc = run_files(&o, list, nfiles, process_info);
    else
        rc = EXIT_USAGE;

    return rc;
}
