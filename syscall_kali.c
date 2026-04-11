#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>

#define N 1024
#define PI 3.14159265358979323846

typedef struct {
    double real;
    double imag;
} complex_num;

// ---------------- FFT ----------------
void run_fft_segment(complex_num* input, complex_num* output, int start, int end) {
    for (int k = start; k < end; k++) {
        double sum_real = 0;
        double sum_imag = 0;

        for (int n = 0; n < N; n++) {
            double angle = 2 * PI * k * n / N;
            sum_real += input[n].real * cos(angle);
            sum_imag -= input[n].real * sin(angle);
        }

        output[k].real = sum_real;
        output[k].imag = sum_imag;
    }
}

int main() {
    printf("--- LINUX CCP PROJECT START ---\n");

    struct timespec start, end;

    // ---------------- FILE I/O ----------------
    clock_gettime(CLOCK_MONOTONIC, &start);

    int fd = open("input.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);

    if (fd < 0) {
        perror("open failed");
        return 1;
    }

    write(fd, "1 2 3 4 5 6 7 8", 15);

    lseek(fd, 0, SEEK_SET);

    char readBuffer[100] = {0};
    read(fd, readBuffer, sizeof(readBuffer));

    printf("File Read: %s\n", readBuffer);

    close(fd);

    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("File I/O Time: %f sec\n",
           (end.tv_sec - start.tv_sec) +
           (end.tv_nsec - start.tv_nsec) / 1e9);

    // ---------------- PROCESS ----------------
    clock_gettime(CLOCK_MONOTONIC, &start);

    pid_t pid = fork();

    if (pid == 0) {
        printf("Child PID: %d\n", getpid());

        char *args[] = {"/bin/echo", "Process_Started", NULL};
        execv(args[0], args);
        exit(0);
    } else {
        printf("Parent PID: %d\n", getpid());
        wait(NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("fork/exec/wait Time: %f sec\n",
           (end.tv_sec - start.tv_sec) +
           (end.tv_nsec - start.tv_nsec) / 1e9);

    // ---------------- PIPE IPC ----------------
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("pipe failed");
        return 1;
    }

    printf("Pipe Created\n");

    const char* pipeMsg = "Pipe Data Transfer";

    write(pipefd[1], pipeMsg, strlen(pipeMsg) + 1);

    char pipeBuffer[100] = {0};
    read(pipefd[0], pipeBuffer, sizeof(pipeBuffer));

    printf("Pipe Data: %s\n", pipeBuffer);

    close(pipefd[0]);
    close(pipefd[1]);

    // ---------------- MEMORY (mmap) ----------------
    clock_gettime(CLOCK_MONOTONIC, &start);

    complex_num* buffer = mmap(NULL,
        N * sizeof(complex_num),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);

    complex_num* result = mmap(NULL,
        N * sizeof(complex_num),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);

    if (buffer == MAP_FAILED || result == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("mmap Time: %f sec\n",
           (end.tv_sec - start.tv_sec) +
           (end.tv_nsec - start.tv_nsec) / 1e9);

    // ---------------- INIT DATA ----------------
    for (int i = 0; i < N; i++) {
        buffer[i].real = i;
        buffer[i].imag = 0;
    }

    // ---------------- PARALLEL FFT (IMPORTANT FIX) ----------------
    pid_t fft_pid = fork();

    if (fft_pid == 0) {
        printf("FFT Child PID: %d\n", getpid());
        run_fft_segment(buffer, result, 0, N/2);
        exit(0);
    } else {
        run_fft_segment(buffer, result, N/2, N);
        wait(NULL);
    }

    // ---------------- SHARED MEMORY ----------------
    int shmid = shmget(IPC_PRIVATE,
        N * sizeof(complex_num),
        IPC_CREAT | 0666);

    if (shmid < 0) {
        perror("shmget failed");
        return 1;
    }

    void* shm_addr = shmat(shmid, NULL, 0);

    if (shm_addr == (void*)-1) {
        perror("shmat failed");
        return 1;
    }

    memcpy(shm_addr, result, N * sizeof(complex_num));

    printf("Shared Memory Used\n");

    // ---------------- CLEAN SHM ----------------
    shmdt(shm_addr);
    shmctl(shmid, IPC_RMID, NULL);

    // ---------------- OUTPUT ----------------
    fd = open("output.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);

    if (fd < 0) {
        perror("output open failed");
        return 1;
    }

    for (int i = 0; i < 10; i++) {
        char line[100];
        sprintf(line, "%f\n", result[i].real);
        write(fd, line, strlen(line));
    }

    close(fd);

    printf("Output File Written\n");

    // ---------------- PERMISSIONS ----------------
    chown("output.txt", getuid(), getgid());
    chmod("output.txt", S_IRUSR | S_IWUSR);

    printf("chown + chmod applied\n");

    // ---------------- CLEANUP ----------------
    munmap(buffer, N * sizeof(complex_num));
    munmap(result, N * sizeof(complex_num));

    printf("--- COMPLETED ---\n");

    return 0;
}
