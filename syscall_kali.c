#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ───────────────────────── CONSTANTS ───────────────────────── */
#define N          1024
#define PI         3.14159265358979323846
#define LOG_FILE   "syscall_log.txt"
#define INPUT_FILE "input.txt"
#define OUTPUT_FILE "output.txt"

/* ───────────────────────── COMPLEX TYPE ───────────────────────── */
typedef struct {
    double real;
    double imag;
} complex_num;

/* ───────────────────────── TIME HELPER ───────────────────────── */
static double elapsed_sec(struct timespec *s, struct timespec *e) {
    return (e->tv_sec - s->tv_sec) +
           (e->tv_nsec - s->tv_nsec) / 1e9;
}

/* ───────────────────────── LOGGING SYSTEM ───────────────────────── */
static int log_fd = -1;

static void log_msg(const char *msg) {
    if (log_fd >= 0) write(log_fd, msg, strlen(msg));
    printf("%s", msg);
}

static void log_metric(const char *label, double sec) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "  [TIME] %-40s : %.6f sec\n", label, sec);
    log_msg(buf);
}

/* ───────────────────────── DFT ───────────────────────── */
static void compute_dft_segment(complex_num *input,
                                complex_num *output,
                                int start, int end) {

    for (int k = start; k < end; k++) {
        double sum_r = 0.0, sum_i = 0.0;

        for (int n = 0; n < N; n++) {
            double angle = 2.0 * PI * k * n / N;

            sum_r += input[n].real * cos(angle)
                   - input[n].imag * sin(angle);

            sum_i += input[n].real * sin(angle)
                   + input[n].imag * cos(angle);
        }

        output[k].real = sum_r;
        output[k].imag = -sum_i;
    }
}

/* ═══════════════════════════════════════════════════════════════ */
int main(void)
/* ═══════════════════════════════════════════════════════════════ */
{
    struct timespec ts, te;
    char log_buf[512];

    log_fd = open(LOG_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);

    log_msg("══════════════════════════════════════════════╗\n");
    log_msg("║     CT-353 OS CCP — LINUX SYSTEM PROJECT     ║\n");
    log_msg("╚══════════════════════════════════════════════╝\n\n");

    /* ───────── 1. umask ───────── */
    log_msg("── [0] PROCESS INFO ─────────────────────────\n");

    snprintf(log_buf, sizeof(log_buf),
            "  PID = %d | PPID = %d\n",
            getpid(), getppid());
    log_msg(log_buf);

    log_msg("── [1] PERMISSIONS CONTROL (umask) ───────────\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);
    mode_t old_mask = umask(0022);
    clock_gettime(CLOCK_MONOTONIC, &te);

    snprintf(log_buf, sizeof(log_buf),
             "  umask set to 0022 | previous=0%03o\n", old_mask);
    log_msg(log_buf);
    log_metric("umask()", elapsed_sec(&ts, &te));

    clock_gettime(CLOCK_MONOTONIC, &ts);
    chmod(INPUT_FILE, 0666);
    clock_gettime(CLOCK_MONOTONIC, &te);
    log_metric("chmod()", elapsed_sec(&ts, &te));

    /* ───────── 2. FILE I/O ───────── */
    log_msg("\n── [2] FILE I/O ──────────────────────────────\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);
    int fd = open(INPUT_FILE, O_CREAT | O_RDWR | O_TRUNC, 0666);
    clock_gettime(CLOCK_MONOTONIC, &te);

    log_metric("open()", elapsed_sec(&ts, &te));

    int fd2 = dup(fd);
    log_msg("  dup() created fd copy\n");

    const char *data =
        "1 2 3 4 5 6 7 8\n"
        "9 10 11 12 13 14 15 16\n";

    write(fd, data, strlen(data));

   struct stat st;
   fstat(fd, &st);

   snprintf(log_buf, sizeof(log_buf),
           "  File size = %ld bytes\n", st.st_size);
   log_msg(log_buf);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    lseek(fd, 0, SEEK_SET);
    clock_gettime(CLOCK_MONOTONIC, &te);

    log_metric("lseek()", elapsed_sec(&ts, &te));
    char read_buf[256] = {0};
    read(fd, read_buf, sizeof(read_buf));

    log_msg("  File data loaded\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);
    close(fd);
    clock_gettime(CLOCK_MONOTONIC, &te);
    log_metric("close()", elapsed_sec(&ts, &te));

    /* ───────── LOAD INPUT FOR FFT ───────── */
    log_msg("\n── [2.1] LOAD INPUT INTO FFT ────────────────\n");

    size_t size = N * sizeof(complex_num);

    complex_num *input =
        mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    mprotect(input, size, PROT_READ | PROT_WRITE);

    int idx = 0;
    char *token = strtok(read_buf, " \n");

    while (token && idx < N) {
        input[idx].real = atof(token);
        input[idx].imag = 0;
        token = strtok(NULL, " \n");
        idx++;
    }

    for (int i = idx; i < N; i++) {
        input[i].real = 0;
        input[i].imag = 0;
    }

    /* ───────── SHARED MEMORY ───────── */
    log_msg("\n── [3] SHARED MEMORY ─────────────────────────\n");

    int shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0666);
    complex_num *out = (complex_num *)shmat(shmid, NULL, 0);

    memset(out, 0, size);

    /* ───────── PIPE IPC (NEW) ───────── */
    log_msg("\n── [3.1] PIPE IPC ───────────────────────────\n");

    int pipefd[2];
    pipe(pipefd);

    clock_gettime(CLOCK_MONOTONIC, &ts);

    pid_t p = fork();

    if (p == 0) {
        close(pipefd[0]);
        const char *m = "Pipe IPC Test Message";
        write(pipefd[1], m, strlen(m) + 1);
        close(pipefd[1]);
        exit(0);
    }

    close(pipefd[1]);
    char pipe_buf[128];
    read(pipefd[0], pipe_buf, sizeof(pipe_buf));
    close(pipefd[0]);
    wait(NULL);

    clock_gettime(CLOCK_MONOTONIC, &te);
    log_msg(pipe_buf);
    log_metric("pipe()", elapsed_sec(&ts, &te));

    /* ───────── FORK+EXEC TEST ───────── */
    log_msg("\n── [3.2] FORK + EXEC ────────────────────────\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);

    pid_t ex = fork();

    if (ex == 0) {
        char *args[] = {"/bin/echo", "execv test OK", NULL};
        execv(args[0], args);
        exit(0);
    }

    wait(NULL);

    clock_gettime(CLOCK_MONOTONIC, &te);
    log_metric("fork+exec+wait", elapsed_sec(&ts, &te));

    /* ───────── PARALLEL FFT ───────── */
    log_msg("\n── [4] PARALLEL FFT ──────────────────────────\n");

    clock_gettime(CLOCK_MONOTONIC, &ts);

    pid_t c1 = fork();
    if (c1 == 0) {
        compute_dft_segment(input, out, 0, N / 2);
        exit(0);
    }

    pid_t c2 = fork();
    if (c2 == 0) {
        compute_dft_segment(input, out, N / 2, N);
        exit(0);
    }

    waitpid(c1, NULL, 0);
    waitpid(c2, NULL, 0);

    clock_gettime(CLOCK_MONOTONIC, &te);

    log_msg("  FFT completed\n");

    kill(getpid(), SIGTERM);
    log_msg("  kill() signal sent to self (demo)\n");

    pid_t self = getpid();
    kill(self, SIGTERM);
    log_msg("  kill() called (self signal demo)\n");

    log_metric("Parallel FFT", elapsed_sec(&ts, &te));

    /* ───────── OUTPUT FILE ───────── */
    log_msg("\n── [5] OUTPUT FILE ──────────────────────────\n");

    int out_fd = open(OUTPUT_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0666);

    for (int i = 0; i < 20; i++) {
        double mag = sqrt(out[i].real * out[i].real +
                          out[i].imag * out[i].imag);

        snprintf(log_buf, sizeof(log_buf),
                 "%d %.4f %.4f %.4f\n",
                 i, out[i].real, out[i].imag, mag);

        write(out_fd, log_buf, strlen(log_buf));
    }

    close(out_fd);

    log_msg("  Output saved\n");

    /* ───────── CLEANUP ───────── */
    munmap(input, size);
    shmdt(out);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    shmctl(shmid, IPC_RMID, NULL);
    clock_gettime(CLOCK_MONOTONIC, &te);
    log_metric("shmctl()", elapsed_sec(&ts, &te));

    log_msg("\n✔ PROGRAM COMPLETED SUCCESSFULLY\n");

    if (log_fd >= 0) close(log_fd);

    return 0;
}
