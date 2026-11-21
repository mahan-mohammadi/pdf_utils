
/*
 * pdfcrop - extract, reorder and reverse pages of a PDF, or dump their text
 * (qpdf frontend; text mode additionally uses pdftotext from poppler-utils)
 *
 * Build:   cc -O2 -Wall -Wextra -o pdfcrop pdfcrop.c
 * Install: install -m 755 pdfcrop /usr/local/bin/
 * Needs:   qpdf in $PATH; pdftotext in $PATH for --text
 */

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
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define PROG         "pdfcrop"
#define VERSION      "1.0.0"
#define MAX_PAGES    1000000
#define MAX_EXPR     100000      /* a single argv string is limited (~128 KiB) */
#define MAX_PASSWORD 4096
#define DIAG_SIZE    8192

enum { EXIT_OK = 0, EXIT_ERR = 1, EXIT_USAGE = 2 };

/* ---------- Global state (needed for signal/exit cleanup) ---------- */

static int g_quiet, g_verbose, g_color;
static char g_pw_tmp[PATH_MAX];
static char g_out_tmp[PATH_MAX];
static char g_txt_tmp[PATH_MAX];
static int g_say_stderr;
static struct termios g_term_saved;
static volatile sig_atomic_t g_term_active;

/* ---------- Messages ---------- */

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

static void err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, g_color ? "\033[1;31m" PROG ": error:\033[0m " : PROG ": error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------- Cleanup ---------- */

static void cleanup(void)
{
    if (g_pw_tmp[0]) {
        unlink(g_pw_tmp);
        g_pw_tmp[0] = '\0';
    }
    if (g_out_tmp[0]) {
        unlink(g_out_tmp);
        g_out_tmp[0] = '\0';
    }
    if (g_txt_tmp[0]) {
        unlink(g_txt_tmp);
        g_txt_tmp[0] = '\0';
    }
}

static void on_signal(int sig)
{
    if (g_term_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_term_saved);
    if (g_pw_tmp[0])
        unlink(g_pw_tmp);
    if (g_out_tmp[0])
        unlink(g_out_tmp);
    if (g_txt_tmp[0])
        unlink(g_txt_tmp);
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

/* ---------- Page list ---------- */

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

/* ---------- Command execution ---------- */

typedef struct {
    char *v[48];
    int n;
    char pw[PATH_MAX + 32];
} Cmd;

static void cmd_init(Cmd *c)
{
    c->n = 0;
    c->v[0] = NULL;
    c->pw[0] = '\0';
}

static void cmd_add(Cmd *c, const char *arg)
{
    if (c->n >= (int)(sizeof(c->v) / sizeof(c->v[0])) - 1) {
        err("internal error: too many command arguments");
        exit(EXIT_ERR);
    }
    c->v[c->n++] = (char *)arg;
    c->v[c->n] = NULL;
}

static void cmd_password(Cmd *c, const char *password_file)
{
    if (!password_file)
        return;
    snprintf(c->pw, sizeof(c->pw), "--password-file=%s", password_file);
    cmd_add(c, c->pw);
}

/* Runs the command; stdout+stderr are captured into diag. Returns exit status or -1. */
static int execute(Cmd *c, char *diag, size_t diag_size)
{
    int pipes[2];

    if (g_verbose) {
        fprintf(stderr, "  > run:");
        for (int i = 0; i < c->n; i++)
            fprintf(stderr, " %s", c->v[i]);
        fputc('\n', stderr);
    }

    if (diag && diag_size)
        diag[0] = '\0';

    if (pipe(pipes) != 0) {
        perror(PROG ": pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror(PROG ": fork");
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipes[0]);
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

        if (diag && diag_size > 0 && used < diag_size - 1) {
            size_t remaining = diag_size - used - 1;
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

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    return -1;
}

/* ---------- Parsing helpers ---------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';

    return s;
}

/* A page token is a positive number or the keyword "last". */
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

/*
 * Page expression grammar (separators: , ; newline):
 *   N          single page            3
 *   A-B        range, either way      1-10   10-1
 *   last       final page             last   5-last   last-1
 *   all        every page
 *   odd/even   odd or even pages
 */
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

/* Collapses ascending runs (1,2,3 -> "1-3") so huge selections stay small. */
static char *build_expression(const PageList *list)
{
    size_t capacity = list->count * 24 + 1;
    char *buf = malloc(capacity);
    if (!buf)
        return NULL;

    size_t used = 0, i = 0;

    while (i < list->count) {
        size_t j = i;
        while (j + 1 < list->count &&
               list->data[j + 1] == list->data[j] + 1)
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

/* ---------- Password handling ---------- */

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

/* Password goes to a 0600 temp file so it never appears in argv/ps. */
static int password_file_create(const char *password)
{
    if (strchr(password, '\n') || strchr(password, '\r')) {
        err("passwords containing newlines are not supported");
        return 0;
    }

    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";

    if (snprintf(g_pw_tmp, sizeof(g_pw_tmp), "%s/pdfcrop-pw-XXXXXX", dir)
        >= (int)sizeof(g_pw_tmp)) {
        g_pw_tmp[0] = '\0';
        err("temporary directory path too long");
        return 0;
    }

    int fd = mkstemp(g_pw_tmp);
    if (fd < 0) {
        perror(PROG ": mkstemp");
        g_pw_tmp[0] = '\0';
        return 0;
    }

    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0 ||
        !write_all(fd, password, strlen(password)) ||
        !write_all(fd, "\n", 1)) {
        close(fd);
        unlink(g_pw_tmp);
        g_pw_tmp[0] = '\0';
        return 0;
    }

    close(fd);
    return 1;
}

static int prompt_password(char *password, size_t size)
{
    struct termios hidden;

    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) {
        err("PDF is password protected; use --password-file, "
            "--password-env or --password-stdin");
        return 0;
    }

    if (tcgetattr(STDIN_FILENO, &g_term_saved) != 0) {
        perror(PROG ": tcgetattr");
        return 0;
    }

    hidden = g_term_saved;
    hidden.c_lflag &= ~ECHO;

    fprintf(stderr, "Enter PDF password: ");
    fflush(stderr);

    g_term_active = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
        g_term_active = 0;
        perror(PROG ": tcsetattr");
        return 0;
    }

    char *result = fgets(password, (int)size, stdin);

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_term_saved);
    g_term_active = 0;
    fputc('\n', stderr);

    if (!result)
        return 0;

    password[strcspn(password, "\r\n")] = '\0';
    return 1;
}

static int read_password_stdin(char *password, size_t size)
{
    if (!fgets(password, (int)size, stdin)) {
        err("could not read password from stdin");
        return 0;
    }
    password[strcspn(password, "\r\n")] = '\0';
    return 1;
}

/* ---------- qpdf operations ---------- */

/*
 * qpdf --requires-password exit codes:
 *   0 password required (missing/wrong), 2 not encrypted,
 *   3 encrypted, no password needed / supplied one is correct.
 */
static int requires_password(const char *file, const char *password_file)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_password(&c, password_file);
    cmd_add(&c, "--requires-password");
    cmd_add(&c, file);

    return execute(&c, NULL, 0);
}

static int qpdf_page_count(const char *file, const char *password_file,
                           int *count)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_password(&c, password_file);
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

static int extract_pdf(const char *input, const char *output,
                       const char *expression, const char *password_file,
                       int linearize)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "qpdf");
    cmd_add(&c, "--warning-exit-0");
    cmd_password(&c, password_file);
    cmd_add(&c, input);
    cmd_add(&c, "--decrypt");
    if (linearize)
        cmd_add(&c, "--linearize");
    cmd_add(&c, "--pages");
    cmd_add(&c, ".");
    cmd_add(&c, expression);
    cmd_add(&c, "--");
    cmd_add(&c, output);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("extraction failed:\n%s", diag);
        return 0;
    }

    if (diag[0])
        trace("qpdf: %s", diag);

    return 1;
}

/* ---------- Temp files / publishing / text ---------- */

static int make_temp(char *path, size_t size, const char *dir,
                     const char *prefix)
{
    if (snprintf(path, size, "%s/%s-XXXXXX", dir, prefix) >= (int)size) {
        path[0] = '\0';
        err("temporary path is too long");
        return 0;
    }

    int fd = mkstemp(path);

    if (fd < 0) {
        err("cannot create temporary file in '%s': %s", dir, strerror(errno));
        path[0] = '\0';
        return 0;
    }

    close(fd);
    return 1;
}

/* Atomically moves tmp to dest; refuses to clobber unless force is set. */
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

static int pdftotext_available(void)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pdftotext");
    cmd_add(&c, "-v");

    char out[256];
    /* pdftotext -v prints its version and exits 0; 127 means not found. */
    int st = execute(&c, out, sizeof(out));
    return st == 0;
}

static int text_extract(const char *pdf, const char *txt,
                        int layout, int nopgbrk)
{
    Cmd c;
    cmd_init(&c);
    cmd_add(&c, "pdftotext");
    if (layout)
        cmd_add(&c, "-layout");
    if (nopgbrk)
        cmd_add(&c, "-nopgbrk");
    cmd_add(&c, "-enc");
    cmd_add(&c, "UTF-8");
    cmd_add(&c, pdf);
    cmd_add(&c, txt);

    char diag[DIAG_SIZE];

    if (execute(&c, diag, sizeof(diag)) != 0) {
        err("text extraction failed:\n%s", diag);
        return 0;
    }

    return 1;
}

/* ---------- Options ---------- */

typedef struct {
    const char *input;
    const char *expr;
    const char *output;
    const char *password_file;
    const char *password_env;
    int password_stdin;
    int force;
    int dry_run;
    int info;
    int verify;
    int linearize;
    int text;
    int layout;
    int nopgbrk;
} Options;

static void usage(FILE *out)
{
    fprintf(out,
"Usage: " PROG " [OPTIONS] INPUT.pdf PAGES\n"
"       " PROG " [OPTIONS] INPUT.pdf PAGES OUTPUT      (legacy form)\n"
"       " PROG " --text [OPTIONS] INPUT.pdf PAGES\n"
"       " PROG " --info INPUT.pdf\n"
"\n"
"Extract, reorder or reverse pages of a PDF, or dump their text. PDF output\n"
"is written decrypted; nothing is overwritten unless --force is given.\n"
"\n"
"PAGES is a list separated by comma, semicolon or newline:\n"
"  3             single page          1-10    range\n"
"  10-1          reversed range       5-last  to the final page\n"
"  last          final page           all     every page\n"
"  odd | even    odd or even pages    1-3,7,9-last   combinations\n"
"\n"
"Options:\n"
"  -o, --output FILE        output file (default: <input>-pages.pdf in cwd;\n"
"                           with --text: stdout, or '-' explicitly)\n"
"  -t, --text               extract the text of the selected pages instead of\n"
"                           writing a PDF (needs pdftotext / poppler-utils)\n"
"      --layout             with --text: keep the physical page layout\n"
"      --no-page-breaks     with --text: no form feed between pages\n"
"  -f, --force              overwrite the output if it exists\n"
"  -n, --dry-run            show what would be done, write nothing\n"
"  -i, --info               print page count / encryption status and exit\n"
"      --password-file F    read the PDF password from the first line of F\n"
"      --password-env VAR   read the PDF password from environment variable VAR\n"
"      --password-stdin     read the PDF password from one line on stdin\n"
"      --linearize          write a web-optimised (linearized) PDF\n"
"      --no-verify          skip qpdf --check on the result\n"
"  -q, --quiet              only print errors\n"
"  -v, --verbose            show the qpdf commands being run\n"
"  -h, --help               show this help\n"
"  -V, --version            show version\n"
"\n"
"If the PDF needs a password and none was supplied, you are prompted\n"
"(terminal only).\n"
"\n"
"Examples:\n"
"  " PROG " book.pdf 1-20,30,40-35 -o part.pdf\n"
"  " PROG " book.pdf odd -o odd.pdf\n"
"  " PROG " -t book.pdf 5-9 > chapter.txt\n"
"  " PROG " -t --layout book.pdf all -o book.txt\n"
"  " PROG " book.pdf last-1 -o reversed.pdf   (whole book, reversed)\n"
"  " PROG " --password-env PDFPW secret.pdf all -o open.pdf\n"
"\n"
"Exit status: 0 success, 1 error, 2 usage error.\n");
}

static int parse_options(int argc, char **argv, Options *o)
{
    static const struct option longopts[] = {
        {"output",         required_argument, 0, 'o'},
        {"force",          no_argument,       0, 'f'},
        {"dry-run",        no_argument,       0, 'n'},
        {"info",           no_argument,       0, 'i'},
        {"password-file",  required_argument, 0, 1000},
        {"password-env",   required_argument, 0, 1001},
        {"password-stdin", no_argument,       0, 1002},
        {"linearize",      no_argument,       0, 1003},
        {"no-verify",      no_argument,       0, 1004},
        {"text",           no_argument,       0, 't'},
        {"layout",         no_argument,       0, 1005},
        {"no-page-breaks", no_argument,       0, 1006},
        {"quiet",          no_argument,       0, 'q'},
        {"verbose",        no_argument,       0, 'v'},
        {"help",           no_argument,       0, 'h'},
        {"version",        no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    memset(o, 0, sizeof(*o));
    o->verify = 1;

    int c;
    while ((c = getopt_long(argc, argv, "o:fnitqvhV", longopts, NULL)) != -1) {
        switch (c) {
        case 'o': o->output = optarg; break;
        case 'f': o->force = 1; break;
        case 'n': o->dry_run = 1; break;
        case 'i': o->info = 1; break;
        case 'q': g_quiet = 1; break;
        case 'v': g_verbose = 1; break;
        case 1000: o->password_file = optarg; break;
        case 1001: o->password_env = optarg; break;
        case 1002: o->password_stdin = 1; break;
        case 1003: o->linearize = 1; break;
        case 1004: o->verify = 0; break;
        case 't': o->text = 1; break;
        case 1005: o->layout = 1; break;
        case 1006: o->nopgbrk = 1; break;
        case 'h': usage(stdout); exit(EXIT_OK);
        case 'V': printf(PROG " " VERSION "\n"); exit(EXIT_OK);
        default:  usage(stderr); return 0;
        }
    }

    int rest = argc - optind;

    if (o->info) {
        if (rest != 1) {
            err("--info takes exactly one INPUT.pdf");
            return 0;
        }
        o->input = argv[optind];
    } else {
        if (rest != 2 && rest != 3) {
            usage(stderr);
            return 0;
        }
        o->input = argv[optind];
        o->expr = argv[optind + 1];

        if (rest == 3) {
            if (o->output) {
                err("output given twice (-o and positional)");
                return 0;
            }
            o->output = argv[optind + 2];
        }
    }

    int sources = (o->password_file != NULL) + (o->password_env != NULL) +
                  o->password_stdin;
    if (sources > 1) {
        err("use only one of --password-file, --password-env, --password-stdin");
        return 0;
    }

    if (o->text && o->info) {
        err("--text and --info cannot be combined");
        return 0;
    }

    if ((o->layout || o->nopgbrk) && !o->text) {
        err("--layout and --no-page-breaks require --text");
        return 0;
    }

    if (g_quiet && g_verbose) {
        err("--quiet and --verbose are mutually exclusive");
        return 0;
    }

    return 1;
}

/* ---------- Output path helpers ---------- */

static int has_pdf_extension(const char *s)
{
    size_t n = strlen(s);
    return n >= 4 && strcasecmp(s + n - 4, ".pdf") == 0;
}

static int resolve_output(const Options *o, const char *input,
                          char *out, size_t size)
{
    int n;

    if (o->output) {
        n = snprintf(out, size, "%s%s", o->output,
                     has_pdf_extension(o->output) ? "" : ".pdf");
    } else {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", input);

        char *base = basename(tmp);
        size_t len = strlen(base);
        if (has_pdf_extension(base))
            len -= 4;

        n = snprintf(out, size, "%.*s-pages.pdf", (int)len, base);
    }

    if (n < 0 || (size_t)n >= size) {
        err("output path is too long");
        return 0;
    }

    return 1;
}

/* ---------- Main workflow ---------- */

static int run(Options *o)
{
    char input[PATH_MAX];

    if (!realpath(o->input, input)) {
        err("cannot locate input PDF '%s': %s", o->input, strerror(errno));
        return EXIT_ERR;
    }

    struct stat in_st;

    if (stat(input, &in_st) != 0 || !S_ISREG(in_st.st_mode) ||
        access(input, R_OK) != 0) {
        err("input is not a readable regular file");
        return EXIT_ERR;
    }

    char output[PATH_MAX] = {0};
    int to_stdout = 0;

    if (o->text && (!o->output || strcmp(o->output, "-") == 0)) {
        to_stdout = 1;
        g_say_stderr = 1;          /* keep stdout clean for the text itself */
        snprintf(output, sizeof(output), "(stdout)");
    }

    if (!o->info && !to_stdout) {
        if (o->text) {
            if (snprintf(output, sizeof(output), "%s", o->output)
                >= (int)sizeof(output)) {
                err("output path is too long");
                return EXIT_ERR;
            }
        } else if (!resolve_output(o, input, output, sizeof(output))) {
            return EXIT_ERR;
        }

        struct stat out_st;

        if (stat(output, &out_st) == 0) {
            if (out_st.st_dev == in_st.st_dev &&
                out_st.st_ino == in_st.st_ino) {
                err("input and output must be different files");
                return EXIT_ERR;
            }
            if (!o->force && !o->dry_run) {
                err("output '%s' already exists (use --force to overwrite)",
                    output);
                return EXIT_ERR;
            }
            if (!S_ISREG(out_st.st_mode)) {
                err("output '%s' exists and is not a regular file", output);
                return EXIT_ERR;
            }
        }
    }

    /* qpdf present? */

    Cmd vc;
    cmd_init(&vc);
    cmd_add(&vc, "qpdf");
    cmd_add(&vc, "--version");

    char version[256];

    if (execute(&vc, version, sizeof(version)) != 0) {
        err("qpdf is not installed or not executable");
        return EXIT_ERR;
    }

    char *vline = version;
    char *nl = strchr(vline, '\n');
    if (nl)
        *nl = '\0';
    trace("%s", trim(vline));

    if (o->text && !pdftotext_available()) {
        err("pdftotext is not installed (package: poppler-utils / poppler)");
        return EXIT_ERR;
    }

    /* Password handling. */

    char password[MAX_PASSWORD] = {0};
    const char *password_file = o->password_file;
    int rc = EXIT_ERR;
    int supplied = 0;

    if (o->password_file) {
        if (access(o->password_file, R_OK) != 0) {
            err("cannot read password file '%s': %s",
                o->password_file, strerror(errno));
            return EXIT_ERR;
        }
        supplied = 1;
    } else if (o->password_env) {
        const char *v = getenv(o->password_env);
        if (!v) {
            err("environment variable '%s' is not set", o->password_env);
            return EXIT_ERR;
        }
        if (strlen(v) >= sizeof(password)) {
            err("password too long");
            return EXIT_ERR;
        }
        strcpy(password, v);
        if (!password_file_create(password))
            goto done;
        password_file = g_pw_tmp;
        supplied = 1;
    } else if (o->password_stdin) {
        if (!read_password_stdin(password, sizeof(password)) ||
            !password_file_create(password))
            goto done;
        password_file = g_pw_tmp;
        supplied = 1;
    }

    int pw_status = requires_password(input, password_file);

    if (pw_status == 0 && !supplied) {
        if (!prompt_password(password, sizeof(password)) ||
            !password_file_create(password))
            goto done;
        password_file = g_pw_tmp;
        pw_status = requires_password(input, password_file);
    }

    if (pw_status == 0) {
        err("incorrect PDF password");
        goto done;
    }

    if (pw_status < 0 || pw_status == 1 || pw_status > 3) {
        err("cannot inspect PDF encryption");
        goto done;
    }

    int encrypted = (pw_status == 3);

    int total = 0;

    if (!qpdf_page_count(input, password_file, &total))
        goto done;

    if (o->info) {
        printf("File:      %s\n", input);
        printf("Pages:     %d\n", total);
        printf("Encrypted: %s\n", encrypted ? "yes" : "no");
        rc = EXIT_OK;
        goto done;
    }

    /* Page selection. */

    PageList pages = {0};
    char *expression = NULL;

    if (!parse_selection(o->expr, total, &pages))
        goto cleanup_pages;

    expression = build_expression(&pages);

    if (!expression || strlen(expression) > MAX_EXPR) {
        err("page selection is too complex; split it into several runs");
        goto cleanup_pages;
    }

    size_t selected = pages.count;

    say("========== PDF CROP ==========\n");
    say("Input:            %s\n", input);
    say("%s%s\n", o->text ? "Text output:      " : "Output:           ", output);
    say("Total pages:      %d\n", total);
    say("Selected pages:   %zu\n", selected);
    say("Source encrypted: %s\n", encrypted ? "yes" : "no");
    if (!o->text)
        say("Output encrypted: no\n");
    say("==============================\n");

    if (o->dry_run) {
        say("Dry run: qpdf page selection would be '%s'\n", expression);
        rc = EXIT_OK;
        goto cleanup_pages;
    }

    /*
     * PDF mode: temp file lives in the destination directory so the final
     * step is atomic. Text mode: the intermediate PDF goes to $TMPDIR.
     */

    char tmpdir[PATH_MAX];
    char dircopy[PATH_MAX];
    const char *tdir;

    if (o->text) {
        const char *t = getenv("TMPDIR");
        snprintf(tmpdir, sizeof(tmpdir), "%s", (t && *t) ? t : "/tmp");
        tdir = tmpdir;
    } else {
        snprintf(dircopy, sizeof(dircopy), "%s", output);
        tdir = dirname(dircopy);
    }

    if (!make_temp(g_out_tmp, sizeof(g_out_tmp), tdir, ".pdfcrop"))
        goto cleanup_pages;

    say("\nExtracting pages...\n");

    if (!extract_pdf(input, g_out_tmp, expression, password_file,
                     o->linearize))
        goto cleanup_pages;

    if (o->text) {
        char tdir2[PATH_MAX];
        const char *txt_dir = tmpdir;

        if (!to_stdout) {
            snprintf(tdir2, sizeof(tdir2), "%s", output);
            txt_dir = dirname(tdir2);
        }

        if (!make_temp(g_txt_tmp, sizeof(g_txt_tmp), txt_dir, ".pdfcrop-txt"))
            goto cleanup_pages;

        say("Extracting text...\n");

        if (!text_extract(g_out_tmp, g_txt_tmp, o->layout, o->nopgbrk))
            goto cleanup_pages;

        if (to_stdout) {
            if (!copy_to_stdout(g_txt_tmp))
                goto cleanup_pages;
        } else {
            if (!publish_file(g_txt_tmp, output, o->force))
                goto cleanup_pages;
            g_txt_tmp[0] = '\0';
            say("\nSUCCESS\nSaved: %s (text of %zu pages)\n", output, selected);
        }

        rc = EXIT_OK;
        goto cleanup_pages;
    }

    if (o->verify) {
        say("Verifying output PDF...\n");

        int out_pages = 0;

        if (!check_pdf(g_out_tmp))
            goto cleanup_pages;

        if (!qpdf_page_count(g_out_tmp, NULL, &out_pages) ||
            (size_t)out_pages != selected) {
            err("output page count does not match the selection");
            goto cleanup_pages;
        }
    }

    if (!publish_file(g_out_tmp, output, o->force))
        goto cleanup_pages;

    g_out_tmp[0] = '\0';

    say("\nSUCCESS\nSaved: %s (%zu pages)\n", output, selected);
    rc = EXIT_OK;

cleanup_pages:
    free(expression);
    pages_free(&pages);

done:
    memset(password, 0, sizeof(password));
    return rc;
}

int main(int argc, char **argv)
{
    Options opts;

    g_color = isatty(STDERR_FILENO) && !getenv("NO_COLOR");

    atexit(cleanup);
    install_signals();

    if (!parse_options(argc, argv, &opts))
        return EXIT_USAGE;

    return run(&opts);
}
