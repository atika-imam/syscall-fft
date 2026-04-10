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

#define N 1024

int main() {
    printf("--- KALI LINUX CCP PROJECT START ---\n");
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // 1-4. File I/O (open, read, write, close)
    int fd = open("output.txt", O_CREAT | O_WRONLY, 0666);
    write(fd, "FFT Results", 11);
    close(fd);

    // 5-7. Process (fork, exec, wait)
    if (fork() == 0) {
        char *args[] = {"/bin/echo", "Process_Started", NULL};
        execv(args[0], args);
        exit(0);
    } else {
        wait(NULL);
    }

    // 8-9. Memory (mmap, munmap)
    void* addr = mmap(NULL, N * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // 10-12. IPC (pipe, shmget, shmat)
    int pipe_fd[2];
    pipe(pipe_fd);
    int shmid = shmget(IPC_PRIVATE, 1024, IPC_CREAT | 0666);
    void* shm_addr = shmat(shmid, NULL, 0);

    // 13-15. Permissions (chmod, chown, umask)
    chmod("output.txt", S_IRUSR | S_IWUSR);
    umask(0022);
    // Note: chown usually requires sudo/root on Kali

    munmap(addr, N * 8);
    shmdt(shm_addr);

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("Linux Execution Completed in: %f s\n", (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9);
    return 0;
}
