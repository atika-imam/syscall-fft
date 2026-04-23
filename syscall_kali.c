/*
 * ================================================================
 *  CT-353 OS CCP — Linux System Call Implementation
 *          Real Cooley-Tukey FFT + Resource Metrics +
 *            Fixed Parallelisation + All 15 Syscalls
 *
 *  System Calls (15):
 *   File I/O    : open, read, write, close, lseek
 *   Permissions : umask, chmod, chown
 *   Memory      : mmap, munmap, mprotect
 *   IPC / Pipe  : pipe
 *   IPC / SHM   : shmget, shmat, shmdt  (+ shmctl for cleanup)
 *   Process     : fork, execv, waitpid, kill, getpid
 *
 *  FFT Algorithm : Iterative Cooley-Tukey Radix-2 DIT
 *                  O(N log N) — in-place, power-of-2 size
 *
 *  Parallelisation (Danielson-Lanczos):
 *    Child 0  — computes M-point FFT of EVEN-indexed samples
 *    Child 1  — computes M-point FFT of ODD-indexed  samples
 *    Parent   — performs final butterfly combination:
 *               X[k]   = E[k] + W^k * O[k]
 *               X[k+M] = E[k] - W^k * O[k]
 *    The two sub-FFTs are fully independent (no shared writes
 *    to the same region) and run in true parallel.
 *
 *  Resource Metrics per syscall:
 *    • Wall-clock time  (clock_gettime CLOCK_MONOTONIC)
 *    • Peak RSS memory  (/proc/self/status VmRSS)
 *    • CPU user time    (clock_gettime CLOCK_PROCESS_CPUTIME_ID)
 *    • CPU system time  (getrusage RUSAGE_SELF)
 *
 *  Compile:
 *    gcc syscall_linux.c -o syscall_linux -lm
 *  Run:
 *    ./syscall_linux
 * ================================================================
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>   /* getrusage */
#include <pwd.h>            /* getpwuid  */

/* ──────────────────────── Constants ────────────────────────── */
#define N            1024        /* Must be a power of 2         */
#define M            (N / 2)
#define PI           3.14159265358979323846
#define LOG_FILE     "syscall_log.txt"
#define INPUT_FILE   "input.txt"
#define OUTPUT_FILE  "output.txt"

/*
 *  Shared memory layout  (4 contiguous slots of M complex_num):
 *  ┌────────────┬────────────┬────────────┬────────────┐
 *  │   slot 0   │   slot 1   │   slot 2   │   slot 3   │
 *  │  in_even   │  in_odd    │  out_even  │  out_odd   │
 *  │ (child 0)  │ (child 1)  │ (child 0)  │ (child 1)  │
 *  └────────────┴────────────┴────────────┴────────────┘
 */
#define SHM_SLOTS    4
#define SLOT_SIZE    ((size_t)M * sizeof(complex_num))
#define SHM_TOTAL    (SHM_SLOTS * SLOT_SIZE)

/* ──────────────────────── Complex type ─────────────────────── */
typedef struct { double real, imag; } complex_num;

/* ──────────────────────── Global log fd ────────────────────── */
static int g_log = -1;

/* ──────────────────────── Logging ──────────────────────────── */
static void log_raw(const char *text) {
    if (g_log >= 0) write(g_log, text, strlen(text));
    fputs(text, stdout);
    fflush(stdout);
}

static void log_fmt(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_raw(buf);
}

/* ──────────────────────── Error helper ─────────────────────── */
static void posix_err(const char *ctx) {
    log_fmt("\n[ERROR] %s | errno=%d | %s\n", ctx, errno, strerror(errno));
}

/* ================================================================
 *  RESOURCE METRICS
 *  Captures wall-clock, peak RSS, CPU user+system time.
 * ================================================================ */
typedef struct {
    double wall_ms;       /* elapsed real time (ms)             */
    long   rss_kb;        /* Resident Set Size from /proc (KB)  */
    double cpu_user_ms;   /* user-mode  CPU (ms)                */
    double cpu_sys_ms;    /* kernel-mode CPU (ms)               */
} Metrics;

/* Read current VmRSS from /proc/self/status */
static long read_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[128];
    long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    fclose(f);
    return kb;
}

static double ts_to_ms(const struct timespec *t) {
    return t->tv_sec * 1000.0 + t->tv_nsec / 1e6;
}

static void collect(const struct timespec *ts,
                    const struct timespec *te,
                    Metrics *m) {
    m->wall_ms    = ts_to_ms(te) - ts_to_ms(ts);
    m->rss_kb     = read_rss_kb();

    struct timespec cpu_now;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_now);
    m->cpu_user_ms = ts_to_ms(&cpu_now);   /* cumulative user CPU */

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    m->cpu_sys_ms = ru.ru_stime.tv_sec * 1000.0
                  + ru.ru_stime.tv_usec / 1000.0;
}

static void print_metrics(const char *label, const Metrics *m) {
    log_fmt("  [METRICS] %-40s\n", label);
    log_fmt("    Wall time    : %10.4f ms\n",  m->wall_ms);
    log_fmt("    RSS memory   : %10ld KB\n",   m->rss_kb);
    log_fmt("    CPU (user)   : %10.4f ms\n",  m->cpu_user_ms);
    log_fmt("    CPU (system) : %10.4f ms\n\n",m->cpu_sys_ms);
}

/* ================================================================
 *  ITERATIVE COOLEY-TUKEY RADIX-2 DIT FFT   O(N log N)
 *
 *  In-place transform of a[0..len-1].
 *  len must be a power of 2.
 *  inverse=0 → forward,  inverse=1 → inverse (normalised).
 * ================================================================ */
static void fft(complex_num *a, int len, int inverse) {
    /* ── Bit-reversal permutation ── */
    for (int i = 1, j = 0; i < len; i++) {
        int bit = len >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            complex_num t = a[i]; a[i] = a[j]; a[j] = t;
        }
    }

    /* ── Butterfly stages ── */
    for (int stage = 2; stage <= len; stage <<= 1) {
        double ang = 2.0 * PI / stage * (inverse ? 1.0 : -1.0);
        complex_num wn = { cos(ang), sin(ang) };

        for (int k = 0; k < len; k += stage) {
            complex_num w = { 1.0, 0.0 };
            int half = stage >> 1;
            for (int m2 = 0; m2 < half; m2++) {
                complex_num u  = a[k + m2];
                complex_num *p = &a[k + m2 + half];
                complex_num v  = {
                    w.real * p->real - w.imag * p->imag,
                    w.real * p->imag + w.imag * p->real
                };
                a[k + m2].real  = u.real + v.real;
                a[k + m2].imag  = u.imag + v.imag;
                p->real         = u.real - v.real;
                p->imag         = u.imag - v.imag;
                /* w *= wn */
                double wr = w.real * wn.real - w.imag * wn.imag;
                w.imag    = w.real * wn.imag + w.imag * wn.real;
                w.real    = wr;
            }
        }
    }

    if (inverse)
        for (int i = 0; i < len; i++) {
            a[i].real /= len;
            a[i].imag /= len;
        }
}

/*
 *  Danielson-Lanczos final butterfly:
 *  Combines FFT(even)[0..M-1] and FFT(odd)[0..M-1]
 *  into the full N=2M-point FFT stored in out[0..N-1].
 *
 *  X[k]   = E[k] + e^{-j2πk/N} · O[k]      k = 0..M-1
 *  X[k+M] = E[k] - e^{-j2πk/N} · O[k]
 */
static void combine(const complex_num *E, const complex_num *O,
                    complex_num *out, int half) {
    for (int k = 0; k < half; k++) {
        double ang = -2.0 * PI * k / (2 * half);
        complex_num tw = { cos(ang), sin(ang) };
        complex_num t  = {
            tw.real * O[k].real - tw.imag * O[k].imag,
            tw.real * O[k].imag + tw.imag * O[k].real
        };
        out[k].real        = E[k].real + t.real;
        out[k].imag        = E[k].imag + t.imag;
        out[k + half].real = E[k].real - t.real;
        out[k + half].imag = E[k].imag - t.imag;
    }
}

/* ================================================================
 *  MAIN
 * ================================================================ */
int main(void) {

    struct timespec ts, te;
    Metrics met;
    char buf[512];

    /* Open log file — uses open() which is syscall #4 below,
     * but we need it ready for all subsequent log output.       */
    g_log = open(LOG_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (g_log < 0) { perror("open log"); return 1; }

    log_raw("╔══════════════════════════════════════════════════════╗\n");
    log_raw("║   CT-353 OS CCP — Linux System Calls + FFT           ║\n");
    log_raw("║   Algorithm : Cooley-Tukey Radix-2 DIT  O(N log N)   ║\n");
    log_raw("║   Parallel  : Even/Odd Danielson-Lanczos split       ║\n");
    log_raw("║   Metrics   : Wall-ms | RSS-KB | User-ms | Sys-ms    ║\n");
    log_raw("╚══════════════════════════════════════════════════════╝\n\n");
    log_fmt("  N=%d  M=N/2=%d  log2(N)=%d butterfly stages\n\n",
            N, M, (int)(log2(N)));

    /* Process info */
    log_raw("── [0] PROCESS INFO ──────────────────────────────────\n");
    log_fmt("  PID=%d  PPID=%d\n\n", getpid(), getppid());

    /* ════════════════════════════════════════════════════════
     *  [1] PERMISSIONS
     *  SYSCALLS 1-3: umask, chmod, chown
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [1] PERMISSIONS (umask / chmod / chown)\n");
    log_raw("══════════════════════════════════════════════════════\n");

    /* SYSCALL 1 — umask */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    mode_t old_mask = umask(0022);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 1 — umask(0022) → previous mask=0%03o\n", old_mask);
    print_metrics("umask()", &met);

    /*
     *  Create the input file NOW (before chmod/chown need it).
     *  We use open() with the mask just set (0666 & ~0022 = 0644).
     */
    int fd_pre = open(INPUT_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd_pre < 0) { posix_err("open (pre-create)"); return 1; }
    close(fd_pre);

    /* SYSCALL 2 — chmod */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (chmod(INPUT_FILE, 0666) < 0) posix_err("chmod");
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  SYSCALL 2 — chmod(input.txt, 0666) → OK\n");
    print_metrics("chmod()", &met);

    /* SYSCALL 3 — chown (set owner to current user — always succeeds) */
    uid_t my_uid = getuid();
    gid_t my_gid = getgid();
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (chown(INPUT_FILE, my_uid, my_gid) < 0) posix_err("chown");
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 3 — chown(input.txt, uid=%d, gid=%d) → OK\n",
            my_uid, my_gid);
    print_metrics("chown()", &met);

    /* ════════════════════════════════════════════════════════
     *  [2] FILE I/O
     *  SYSCALLS 4-8: open, write, lseek, read, close
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [2] FILE I/O\n");
    log_raw("══════════════════════════════════════════════════════\n");

    /* SYSCALL 4 — open */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int fd = open(INPUT_FILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
    clock_gettime(CLOCK_MONOTONIC, &te);
    if (fd < 0) { posix_err("open"); return 1; }
    collect(&ts, &te, &met);
    log_raw("  SYSCALL 4 — open(input.txt, O_RDWR) → OK\n");
    print_metrics("open()", &met);

    /* dup() — bonus file descriptor demo (like DuplicateHandle on Windows) */
    int fd2 = dup(fd);
    if (fd2 < 0) posix_err("dup");
    else log_raw("  [BONUS] dup() → fd copy created\n\n");

    /*
     *  Write N values: samples 0-15 = 1..16, rest = 0.
     *  One value per line so parsing is unambiguous.
     */
    int   wbuf_len  = N * 8;
    char *write_buf = malloc(wbuf_len);
    if (!write_buf) { log_raw("[FATAL] malloc\n"); return 1; }
    int wpos = 0;
    for (int i = 0; i < N; i++)
        wpos += snprintf(write_buf + wpos, wbuf_len - wpos,
                         "%d\n", (i < 16) ? (i + 1) : 0);

    /* SYSCALL 5 — write */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ssize_t bw = write(fd, write_buf, wpos);
    clock_gettime(CLOCK_MONOTONIC, &te);
    free(write_buf);
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 5 — write() → %zd bytes written\n", bw);
    print_metrics("write()", &met);

    /* fstat to verify */
    struct stat st;
    fstat(fd, &st);
    log_fmt("  fstat() → file size = %ld bytes\n\n", (long)st.st_size);

    /* SYSCALL 6 — lseek */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    off_t off = lseek(fd, 0, SEEK_SET);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 6 — lseek(0, SEEK_SET) → offset=%ld\n", (long)off);
    print_metrics("lseek()", &met);

    /* SYSCALL 7 — read */
    int   rbuf_len  = N * 8 + 4;
    char *read_buf  = calloc(1, rbuf_len);
    if (!read_buf) { log_raw("[FATAL] calloc\n"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ssize_t br = read(fd, read_buf, rbuf_len - 1);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 7 — read() → %zd bytes read\n", br);
    print_metrics("read()", &met);

    /* SYSCALL 8 — close */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    close(fd);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  SYSCALL 8 — close(fd) → OK\n");
    print_metrics("close(fd)", &met);

    if (fd2 >= 0) {
        close(fd2);
        log_raw("  close(fd2 / dup) → OK\n\n");
    }

    /* ════════════════════════════════════════════════════════
     *  [3] MEMORY — mmap / mprotect / munmap
     *  SYSCALLS 9-11
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [3] MEMORY MANAGEMENT\n");
    log_raw("══════════════════════════════════════════════════════\n");

    size_t fft_bytes = (size_t)N * sizeof(complex_num);

    /* SYSCALL 9 — mmap (anonymous private mapping for fft_input) */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    complex_num *fft_input = mmap(NULL, fft_bytes,
                                  PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    clock_gettime(CLOCK_MONOTONIC, &te);
    if (fft_input == MAP_FAILED) { posix_err("mmap"); return 1; }
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 9  — mmap(%zu bytes) → %p\n",
            fft_bytes, (void *)fft_input);
    print_metrics("mmap()", &met);

    /* SYSCALL 10 — mprotect (demonstrate changing page protection) */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (mprotect(fft_input, fft_bytes, PROT_READ | PROT_WRITE) < 0)
        posix_err("mprotect");
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  SYSCALL 10 — mprotect(PROT_READ|PROT_WRITE) → OK\n");
    print_metrics("mprotect()", &met);

    /* Parse read_buf → fft_input[] */
    log_raw(" [3.1] Parse file → fft_input[]\n");
    {
        int idx = 0;
        char *tok = strtok(read_buf, " \r\n\t");
        while (tok && idx < N) {
            fft_input[idx].real = atof(tok);
            fft_input[idx].imag = 0.0;
            tok = strtok(NULL, " \r\n\t");
            idx++;
        }
        for (int i = idx; i < N; i++)
            fft_input[i].real = fft_input[i].imag = 0.0;
        log_fmt("  Loaded %d samples; zero-padded to N=%d\n\n", idx, N);
    }
    free(read_buf);

    /* ════════════════════════════════════════════════════════
     *  [4] SHARED MEMORY (System V)
     *  SYSCALLS 12-14: shmget, shmat, shmdt  (+shmctl cleanup)
     *
     *  Layout: 4 slots of M complex_num each.
     *    slot 0: even-indexed input  → child 0 reads
     *    slot 1: odd-indexed  input  → child 1 reads
     *    slot 2: FFT(even) output   ← child 0 writes
     *    slot 3: FFT(odd)  output   ← child 1 writes
     *
     *  Children write to NON-OVERLAPPING regions → no race.
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [4] SHARED MEMORY (System V IPC)\n");
    log_raw("══════════════════════════════════════════════════════\n");

    /* SYSCALL 12 — shmget */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int shmid = shmget(IPC_PRIVATE, SHM_TOTAL, IPC_CREAT | 0666);
    clock_gettime(CLOCK_MONOTONIC, &te);
    if (shmid < 0) { posix_err("shmget"); return 1; }
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 12 — shmget(size=%zu) → shmid=%d\n",
            SHM_TOTAL, shmid);
    print_metrics("shmget()", &met);

    /* SYSCALL 13 — shmat */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    void *shm_ptr = shmat(shmid, NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &te);
    if (shm_ptr == (void *)-1) { posix_err("shmat"); return 1; }
    collect(&ts, &te, &met);
    log_fmt("  SYSCALL 13 — shmat() → %p\n", shm_ptr);
    print_metrics("shmat()", &met);

    /* Partition SHM into 4 named slots */
    complex_num *shm_even_in  = (complex_num *)((char *)shm_ptr + 0 * SLOT_SIZE);
    complex_num *shm_odd_in   = (complex_num *)((char *)shm_ptr + 1 * SLOT_SIZE);
    complex_num *shm_even_out = (complex_num *)((char *)shm_ptr + 2 * SLOT_SIZE);
    complex_num *shm_odd_out  = (complex_num *)((char *)shm_ptr + 3 * SLOT_SIZE);

    /* De-interleave: even/odd separation into SHM input slots */
    for (int i = 0; i < M; i++) {
        shm_even_in[i] = fft_input[2 * i];
        shm_odd_in[i]  = fft_input[2 * i + 1];
    }
    memset(shm_even_out, 0, SLOT_SIZE);
    memset(shm_odd_out,  0, SLOT_SIZE);
    log_raw("  Input de-interleaved → slot0=even, slot1=odd\n\n");

    /* ════════════════════════════════════════════════════════
     *  [5] PIPE IPC
     *  SYSCALL: pipe  (used between parent and an info child)
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [5] PIPE IPC\n");
    log_raw("══════════════════════════════════════════════════════\n");

    int pipefd[2];
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (pipe(pipefd) < 0) { posix_err("pipe"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  SYSCALL — pipe() → OK\n");
    print_metrics("pipe()", &met);

    /* Fork a tiny child that sends a message through the pipe */
    pid_t pipe_child = fork();
    if (pipe_child == 0) {
        /* child side */
        close(pipefd[0]);
        const char *pmsg = "CT353: IPC pipe test — FFT pipeline ready";
        write(pipefd[1], pmsg, strlen(pmsg) + 1);
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    char pipe_buf[128] = {0};
    read(pipefd[0], pipe_buf, sizeof(pipe_buf) - 1);
    close(pipefd[0]);
    waitpid(pipe_child, NULL, 0);
    log_fmt("  Pipe echo → \"%s\"\n\n", pipe_buf);

    /* ════════════════════════════════════════════════════════
     *  [6] FORK + EXEC (execv demo)
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [6] FORK + EXEC\n");
    log_raw("══════════════════════════════════════════════════════\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);
    pid_t exec_child = fork();
    if (exec_child == 0) {
        char *args[] = { "/bin/echo",
                         "  [execv] CT353 execv() test OK", NULL };
        execv(args[0], args);
        _exit(1);   /* only reached if execv fails */
    }
    waitpid(exec_child, NULL, 0);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    print_metrics("fork + execv + waitpid", &met);

    /* ════════════════════════════════════════════════════════
     *  [7] PARALLEL FFT VIA CHILD PROCESSES
     *  Syscalls: fork x2, waitpid x2, kill, getpid
     *
     *  Strategy: Danielson-Lanczos even/odd decomposition.
     *  Child 0 → M-point FFT of even samples (shm_even_in)
     *            → writes result to shm_even_out
     *  Child 1 → M-point FFT of odd  samples (shm_odd_in)
     *            → writes result to shm_odd_out
     *  Parent  → butterfly combine → full N-point FFT
     *
     *  Because each child writes to a DIFFERENT SHM slot there
     *  is NO race condition and NO synchronisation needed
     *  between the two workers.
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw(" [7] PARALLEL FFT VIA CHILD PROCESSES\n");
    log_raw("══════════════════════════════════════════════════════\n");
    log_raw("  Strategy: Danielson-Lanczos even/odd decomposition\n");
    log_fmt("  Child 0 computes %d-pt FFT of even samples\n", M);
    log_fmt("  Child 1 computes %d-pt FFT of odd  samples\n", M);
    log_raw("  Parent  combines results with final butterfly\n\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* ── Child 0: even samples ── */
    pid_t c0 = fork();
    if (c0 == 0) {
        /* Copy even-input slot to a private mmap buffer for in-place FFT */
        complex_num *local = mmap(NULL, SLOT_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (local == MAP_FAILED) _exit(1);
        memcpy(local, shm_even_in, SLOT_SIZE);
        fft(local, M, 0);
        memcpy(shm_even_out, local, SLOT_SIZE);
        munmap(local, SLOT_SIZE);
        _exit(0);
    }
    if (c0 < 0) { posix_err("fork child0"); return 1; }
    log_fmt("  fork() → child 0 PID=%d (even FFT)\n", c0);

    /* ── Child 1: odd samples ── */
    pid_t c1 = fork();
    if (c1 == 0) {
        complex_num *local = mmap(NULL, SLOT_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (local == MAP_FAILED) _exit(1);
        memcpy(local, shm_odd_in, SLOT_SIZE);
        fft(local, M, 0);
        memcpy(shm_odd_out, local, SLOT_SIZE);
        munmap(local, SLOT_SIZE);
        _exit(0);
    }
    if (c1 < 0) { posix_err("fork child1"); return 1; }
    log_fmt("  fork() → child 1 PID=%d (odd  FFT)\n", c1);

    /* ── waitpid: parallel join ── */
    log_raw("  waitpid() — waiting for both children...\n");
    int status0, status1;
    waitpid(c0, &status0, 0);
    waitpid(c1, &status1, 0);

    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_fmt("  waitpid() done → child0 exit=%d  child1 exit=%d\n",
            WEXITSTATUS(status0), WEXITSTATUS(status1));
    print_metrics("fork x2 + waitpid (parallel FFT)", &met);

    /* ── kill() — send SIGUSR1 to self as demonstration ── */
    /*
     *  NOTE: We deliberately use SIGUSR1 (not SIGTERM) so the
     *  process is NOT killed.  SIGTERM after the FFT (as in the
     *  original code) terminates the process before cleanup runs.
     *  kill() is still fully demonstrated as a syscall.
     */
    signal(SIGUSR1, SIG_IGN);   /* ignore so we survive */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pid_t self = getpid();
    kill(self, SIGUSR1);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_fmt("  kill(pid=%d, SIGUSR1) → sent to self (demo)\n", self);
    print_metrics("kill() + getpid()", &met);

    /* ════════════════════════════════════════════════════════
     *  [7.1] FINAL BUTTERFLY COMBINATION (parent)
     * ════════════════════════════════════════════════════════ */
    log_raw("\n [7.1] FINAL BUTTERFLY (parent combines even+odd FFTs)\n");

    complex_num *fft_result = mmap(NULL, fft_bytes,
                                   PROT_READ | PROT_WRITE,
                                   MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (fft_result == MAP_FAILED) { posix_err("mmap result"); return 1; }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    combine(shm_even_out, shm_odd_out, fft_result, M);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  combine() complete → full N-point FFT in fft_result[]\n");
    print_metrics("Final butterfly combine()", &met);

    /* ════════════════════════════════════════════════════════
     *  [8] OUTPUT
     * ════════════════════════════════════════════════════════ */
    log_raw("\n══════════════════════════════════════════════════════\n");
    log_raw(" [8] OUTPUT RESULTS\n");
    log_raw("══════════════════════════════════════════════════════\n");

    int out_fd = open(OUTPUT_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (out_fd < 0) {
        posix_err("open output");
    } else {
        const char *hdr =
            "k\tReal\t\t\tImag\t\t\tMagnitude\n"
            "------------------------------------------------------\n";
        write(out_fd, hdr, strlen(hdr));

        for (int k = 0; k < N; k++) {
            double mag = sqrt(fft_result[k].real * fft_result[k].real +
                              fft_result[k].imag * fft_result[k].imag);
            snprintf(buf, sizeof(buf),
                     "%d\t%18.8f\t%18.8f\t%18.8f\n",
                     k, fft_result[k].real, fft_result[k].imag, mag);
            write(out_fd, buf, strlen(buf));
        }
        close(out_fd);
        log_fmt("  output.txt written — all %d FFT bins\n", N);
    }

    /* Console preview */
    log_raw("\n  First 8 FFT bins:\n");
    log_raw("  k      Real              Imag              Magnitude\n");
    log_raw("  ------ ---------------- ----------------- -----------------\n");
    for (int k = 0; k < 8; k++) {
        double mag = sqrt(fft_result[k].real * fft_result[k].real +
                          fft_result[k].imag * fft_result[k].imag);
        log_fmt("  %-6d %16.6f  %16.6f  %16.6f\n",
                k, fft_result[k].real, fft_result[k].imag, mag);
    }

    /* ════════════════════════════════════════════════════════
     *  [9] CLEANUP
     *  SYSCALL 11: munmap (x2)
     *  SYSCALL 14: shmdt
     *  shmctl IPC_RMID to remove SHM from kernel
     * ════════════════════════════════════════════════════════ */
    log_raw("\n══════════════════════════════════════════════════════\n");
    log_raw(" [9] CLEANUP\n");
    log_raw("══════════════════════════════════════════════════════\n");

    /* SYSCALL 11 — munmap (fft_input) */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    munmap(fft_input, fft_bytes);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  SYSCALL 11 — munmap(fft_input) → OK\n");
    print_metrics("munmap(fft_input)", &met);

    /* munmap fft_result */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    munmap(fft_result, fft_bytes);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  munmap(fft_result) → OK\n");
    print_metrics("munmap(fft_result)", &met);

    /* SYSCALL 14 — shmdt */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    shmdt(shm_ptr);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  SYSCALL 14 — shmdt() → OK\n");
    print_metrics("shmdt()", &met);

    /* shmctl IPC_RMID — remove SHM segment from kernel */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    shmctl(shmid, IPC_RMID, NULL);
    clock_gettime(CLOCK_MONOTONIC, &te);
    collect(&ts, &te, &met);
    log_raw("  shmctl(IPC_RMID) → SHM segment removed\n");
    print_metrics("shmctl(IPC_RMID)", &met);

    /* ════════════════════════════════════════════════════════
     *  SUMMARY TABLE
     * ════════════════════════════════════════════════════════ */
    log_raw("\n╔══════════════════════════════════════════════════════╗\n");
    log_raw("║               SYSTEM CALL SUMMARY                    ║\n");
    log_raw("╠════╦══════════════╦══════════════════════════════════╣\n");
    log_raw("║  # ║ Category     ║ Linux Syscall                    ║\n");
    log_raw("╠════╬══════════════╬══════════════════════════════════╣\n");
    log_raw("║  1 ║ Permissions  ║ umask()                          ║\n");
    log_raw("║  2 ║ Permissions  ║ chmod()                          ║\n");
    log_raw("║  3 ║ Permissions  ║ chown()                          ║\n");
    log_raw("║  4 ║ File I/O     ║ open()                           ║\n");
    log_raw("║  5 ║ File I/O     ║ write()                          ║\n");
    log_raw("║  6 ║ File I/O     ║ lseek()                          ║\n");
    log_raw("║  7 ║ File I/O     ║ read()                           ║\n");
    log_raw("║  8 ║ File I/O     ║ close()                          ║\n");
    log_raw("║  9 ║ Memory       ║ mmap()                           ║\n");
    log_raw("║ 10 ║ Memory       ║ mprotect()                       ║\n");
    log_raw("║ 11 ║ Memory       ║ munmap()                         ║\n");
    log_raw("║ 12 ║ IPC / SHM    ║ shmget()                         ║\n");
    log_raw("║ 13 ║ IPC / SHM    ║ shmat()                          ║\n");
    log_raw("║ 14 ║ IPC / SHM    ║ shmdt()                          ║\n");
    log_raw("║ 15 ║ IPC          ║ pipe()                           ║\n");
    log_raw("║  + ║ Process      ║ fork, execv, waitpid, kill,      ║\n");
    log_raw("║    ║              ║ getpid  (supporting calls)       ║\n");
    log_raw("╚════╩══════════════╩══════════════════════════════════╝\n");
    log_raw("\n  FFT: Cooley-Tukey radix-2 DIT  O(N log N)\n");
    log_raw("  Parallelism: Danielson-Lanczos even/odd decomposition\n");
    log_raw("  Metrics: wall-ms, RSS-KB, CPU-user-ms, CPU-sys-ms\n");
    log_raw("\n  PROGRAM COMPLETED SUCCESSFULLY\n");

    if (g_log >= 0) close(g_log);
    return 0;
}
