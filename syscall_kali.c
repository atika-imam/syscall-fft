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

#define N 1024
#define PI 3.14159265358979323846

typedef struct {
    double real;
    double imag;
} complex_num;

// ---------------- FFT ----------------
void run_fft(complex_num* data) {
    for (int k = 0; k < N; k++) {
        double sum_real = 0;
        double sum_imag = 0;

        for (int n = 0; n < N; n++) {
            double angle = 2 * PI * k * n / N;
            sum_real += data[n].real * cos(angle);
            sum_imag -= data[n].real * sin(angle);
        }

        data[k].real = sum_real;
        data[k].imag = sum_imag;
    }
}

int main() {
    printf("--- LINUX CCP PROJECT ---\n");

    struct timespec start, end;

    // ---------------- FILE I/O ----------------
    clock_gettime(CLOCK_MONOTONIC, &start);

    int fd = open("input.txt", O_CREAT | O_WRONLY, 0666);
    write(fd, "1 2 3 4 5 6 7 8", 15);
    close(fd);

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("File I/O Time: %f sec\n",
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9);

    // ---------------- PROCESS ----------------
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (fork() == 0) {
        char *args[] = {"/bin/echo", "Process_Started", NULL};
        execv(args[0], args);
        exit(0);
    } else {
        wait(NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("Process Time: %f sec\n",
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9);

    // ---------------- MEMORY ----------------
    clock_gettime(CLOCK_MONOTONIC, &start);

    complex_num* buffer = mmap(NULL,
        N * sizeof(complex_num),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0);

    clock_gettime(CLOCK_MONOTONIC, &end);

    printf("mmap Time: %f sec\n",
        (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9);

    // initialize data
    for (int i = 0; i < N; i++) {
        buffer[i].real = i;
        buffer[i].imag = 0;
    }

    // ---------------- FFT ----------------
    clock_t fft_start = clock();
    run_fft(buffer);
    clock_t fft_end = clock();

    printf("FFT Time: %f sec\n",
        (double)(fft_end - fft_start) / CLOCKS_PER_SEC);

    // ---------------- IPC ----------------
    int shmid = shmget(IPC_PRIVATE,
        N * sizeof(complex_num),
        IPC_CREAT | 0666);

    void* shm_addr = shmat(shmid, NULL, 0);

    memcpy(shm_addr, buffer, N * sizeof(complex_num));

    printf("Shared Memory Used\n");

    // ---------------- OUTPUT ----------------
    fd = open("output.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666);

    for (int i = 0; i < 10; i++) {
        char line[100];
        sprintf(line, "%f\n", buffer[i].real);
        write(fd, line, sizeof(char) * strlen(line));
    }

    close(fd);

    // ---------------- PERMISSIONS ----------------
    chmod("output.txt", S_IRUSR | S_IWUSR);
    umask(0022);

    // cleanup
    munmap(buffer, N * sizeof(complex_num));
    shmdt(shm_addr);

    printf("--- COMPLETED ---\n");
    return 0;
}
