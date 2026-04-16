#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0600 
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ─────────────── Constants ─────────────── */
#define N          1024
#define PI         3.14159265358979323846
#define SHM_NAME   "Local\\CT353_FFT_SHM"
#define LOG_FILE   "syscall_log.txt"
#define INPUT_FILE "input.txt"
#define OUTPUT_FILE "output.txt"

/* ─────────────── Complex struct ─────────────── */
typedef struct {
    double real;
    double imag;
} complex_num;

/* ─────────────── Timing ─────────────── */
static LARGE_INTEGER g_freq;

static double elapsed_sec(LARGE_INTEGER *s, LARGE_INTEGER *e) {
    return (double)(e->QuadPart - s->QuadPart) / (double)g_freq.QuadPart;
}

/* ─────────────── Error Logger ─────────────── */
static void print_win_error(const char *context) {
    DWORD err = GetLastError();
    char msg[512];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, msg, sizeof(msg), NULL);
    fprintf(stderr, "\n[ERROR] %s | Code=%lu | %s\n", context, err, msg);
}

/* ─────────────── Logging ─────────────── */
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void log_msg(const char *msg) {
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(g_log, msg, (DWORD)strlen(msg), &w, NULL);
    }
    printf("%s", msg);
}

static void log_metric(const char *label, double sec) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "  [TIME] %-40s : %.6f sec\n", label, sec);
    log_msg(buf);
}

/* ─────────────── DFT ─────────────── */
static void compute_dft_segment(complex_num *input, complex_num *output,
                                 int start, int end) {

    for (int k = start; k < end; k++) {
        double sum_r = 0, sum_i = 0;

        for (int n = 0; n < N; n++) {
            double angle = 2.0 * PI * k * n / N;
            sum_r += input[n].real * cos(angle) - input[n].imag * sin(angle);
            sum_i += input[n].real * sin(angle) + input[n].imag * cos(angle);
        }

        output[k].real = sum_r;
        output[k].imag = -sum_i;
    }
}

/* ─────────────── Worker Mode ─────────────── */
static int run_worker(int start) {

    int end = start + N / 2;

    log_msg("\n── [WORKER STARTED] ───────────────────────────\n");

    HANDLE hMap = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (!hMap) {
        print_win_error("OpenFileMapping");
        return 1;
    }

    size_t half = N * sizeof(complex_num);

    BYTE *view = (BYTE *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 2 * half);
    if (!view) {
        print_win_error("MapViewOfFile (worker)");
        CloseHandle(hMap);
        return 1;
    }

    complex_num *input  = (complex_num *)view;
    complex_num *output = (complex_num *)(view + half);

    char msg[128];
    snprintf(msg, sizeof(msg),
             "  Worker PID=%lu processing bins %d to %d\n",
             GetCurrentProcessId(), start, end);
    log_msg(msg);

    compute_dft_segment(input, output, start, end);

    log_msg("  Worker computation DONE\n");

    UnmapViewOfFile(view);
    CloseHandle(hMap);

    log_msg("── [WORKER EXIT] ─────────────────────────────\n");
    return 0;
}

/* ══════════════════════════════════════════════
 * MAIN PROCESS
 * ══════════════════════════════════════════════ */
int main(int argc, char *argv[]) {

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc == 3 && strcmp(argv[1], "worker") == 0) {
        return run_worker(atoi(argv[2]));
    }

    QueryPerformanceFrequency(&g_freq);

    LARGE_INTEGER ts, te;

    char out[300];

    g_log = CreateFileA(LOG_FILE, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    log_msg("╔══════════════════════════════════════════════╗\n");
    log_msg("║   CT-353 OS CCP — WINDOWS UPDATED VERSION    ║\n");
    log_msg("╚══════════════════════════════════════════════╝\n\n");

    /* ───────── FILE I/O ───────── */
    log_msg("── [1] FILE I/O ───────────────────────────────\n");

    SetFileAttributesA(INPUT_FILE, FILE_ATTRIBUTE_NORMAL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };

    QueryPerformanceCounter(&ts);
    HANDLE hFile = CreateFileA(INPUT_FILE, GENERIC_READ | GENERIC_WRITE,
                               0, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);

    HANDLE hFileDup;

    if (!DuplicateHandle(GetCurrentProcess(), hFile,
                        GetCurrentProcess(), &hFileDup,
                        0, FALSE, DUPLICATE_SAME_ACCESS)) {
        print_win_error("DuplicateHandle");
    }

    log_msg("  DuplicateHandle() created file descriptor copy\n");
    
    QueryPerformanceCounter(&te);

    if (hFile == INVALID_HANDLE_VALUE) {
        print_win_error("CreateFile");
        return 1;
    }

    log_msg("  CreateFile(input.txt) → OK\n");
    log_metric("CreateFile()", elapsed_sec(&ts, &te));

    const char *data =
        "1 2 3 4 5 6 7 8\r\n"
        "9 10 11 12 13 14 15 16\r\n";

    DWORD w;
    WriteFile(hFile, data, (DWORD)strlen(data), &w, NULL);

    BY_HANDLE_FILE_INFORMATION fi;

    if (GetFileInformationByHandle(hFile, &fi)) {
        ULONGLONG size =
            ((ULONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;

        snprintf(out, sizeof(out),
                "  File size = %llu bytes\n", size);
        log_msg(out);
    }

    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);

    char buf[256] = {0};
    DWORD r;

    ReadFile(hFile, buf, sizeof(buf) - 1, &r, NULL);

    snprintf(out, sizeof(out),
             "  ReadFile() → %lu bytes\n  DATA:\n%s\n", r, buf);
    log_msg(out);

    DWORD fileSize = GetFileSize(hFile, NULL);
    snprintf(out, sizeof(out),
            "  GetFileSize() = %lu bytes\n", fileSize);
    log_msg(out);

    CloseHandle(hFile);

    /* ───────── FILE PERMISSIONS (SetFileSecurity) ───────── */
    log_msg("\n── [1.1] FILE PERMISSIONS ──────────────────────\n");

    QueryPerformanceCounter(&ts);

    if (SetFileAttributesA(INPUT_FILE, FILE_ATTRIBUTE_READONLY)) {
        log_msg("   SetFileAttributes → READONLY applied\n");
    }

    QueryPerformanceCounter(&te);
    log_metric("SetFileAttributes()", elapsed_sec(&ts, &te));

    /* ───────── PIPE ───────── */
    log_msg("\n── [2] PIPE IPC ───────────────────────────────\n");

    HANDLE rPipe, wPipe;
    SECURITY_ATTRIBUTES ps = { sizeof(ps), NULL, TRUE };

    /* 🟢 START TIMER HERE */
    QueryPerformanceCounter(&ts);

    /* SYSTEM CALL YOU ARE MEASURING */
    if (!CreatePipe(&rPipe, &wPipe, &ps, 0)) {
        print_win_error("CreatePipe");
    }

    /* 🟢 END TIMER HERE */
    QueryPerformanceCounter(&te);

    log_metric("CreatePipe()", elapsed_sec(&ts, &te));

    const char *msg = "PIPE IPC TEST MESSAGE";
    WriteFile(wPipe, msg, strlen(msg) + 1, &w, NULL);
    CloseHandle(wPipe);

    char pbuf[128] = {0};
    ReadFile(rPipe, pbuf, sizeof(pbuf), &r, NULL);
    CloseHandle(rPipe);

    snprintf(out, sizeof(out),
            "  Pipe Received → %s\n", pbuf);

    log_msg(out);

    /* ───────── MEMORY ───────── */
    log_msg("\n── [3] MEMORY (VirtualAlloc) ──────────────────\n");

    size_t sz = N * sizeof(complex_num);

    QueryPerformanceCounter(&ts);
    complex_num *fft_input = (complex_num *)VirtualAlloc(
        NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    QueryPerformanceCounter(&te);

    log_msg("  VirtualAlloc → OK\n");
    log_metric("VirtualAlloc()", elapsed_sec(&ts, &te));

    /* ───────── USE FILE INPUT FOR FFT ───────── */
    log_msg("\n── [3.1] LOAD INPUT DATA FOR FFT ───────────────\n");

    int idx = 0;
    char *token = strtok(buf, " \r\n");

    while (token != NULL && idx < N) {
        fft_input[idx].real = atof(token);
        fft_input[idx].imag = 0;
        token = strtok(NULL, " \r\n");
        idx++;
    }

    // fallback if file has less data
    for (int i = idx; i < N; i++) {
        fft_input[i].real = 0;
        fft_input[i].imag = 0;
    }

    char msg2[128];
    snprintf(msg2, sizeof(msg2),
            "  Loaded %d values from input file into FFT\n", idx);
    log_msg(msg2);

    /* ───────── SHARED MEMORY ───────── */
    log_msg("\n── [4] SHARED MEMORY ──────────────────────────\n");

    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                     PAGE_READWRITE, 0, 2 * sz,
                                     SHM_NAME);

    BYTE *view = (BYTE *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS,
                                       0, 0, 2 * sz);

    complex_num *shm_in  = (complex_num *)view;
    complex_num *shm_out = (complex_num *)(view + sz);

    memcpy(shm_in, fft_input, sz);
    ZeroMemory(shm_out, sz);

    /* ───────── PROCESS ───────── */
    log_msg("  Spawning worker processes...\n");
    log_msg("\n── [5] PROCESS (CreateProcess x2) ─────────────\n");

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);

    char c1[MAX_PATH], c2[MAX_PATH];
    sprintf(c1, "\"%s\" worker 0", exe);
    sprintf(c2, "\"%s\" worker 512", exe);

    STARTUPINFOA si1 = { sizeof(si1) }, si2 = { sizeof(si2) };
    PROCESS_INFORMATION pi1, pi2;

    QueryPerformanceCounter(&ts);

    /* worker 1 */
    if (!CreateProcessA(NULL, c1, NULL, NULL, FALSE, 0, NULL, NULL, &si1, &pi1)) {
        print_win_error("CreateProcess worker1");
    }

    /* worker 2 */
    if (!CreateProcessA(NULL, c2, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi2)) {
        print_win_error("CreateProcess worker2");
    }

    WaitForSingleObject(pi1.hProcess, INFINITE);
    WaitForSingleObject(pi2.hProcess, INFINITE);

    QueryPerformanceCounter(&te);
    log_metric("WaitForSingleObject()", elapsed_sec(&ts, &te));

    TerminateProcess(pi1.hProcess, 0);
    TerminateProcess(pi2.hProcess, 0);

    log_msg("  TerminateProcess() used for cleanup\n");

    QueryPerformanceCounter(&te);
    log_metric("WaitForSingleObject()", elapsed_sec(&ts, &te));

    QueryPerformanceCounter(&te);

    log_msg("  Both workers completed FFT\n");
    log_metric("Parallel FFT", elapsed_sec(&ts, &te));

    /* ───────── OUTPUT ───────── */
    log_msg("\n── [6] OUTPUT RESULTS ─────────────────────────\n");

    HANDLE outFile = CreateFileA(OUTPUT_FILE, GENERIC_WRITE, 0, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    const char *hdr =
        "k\tReal\tImag\tMagnitude\r\n"
        "-----------------------------------\r\n";

    WriteFile(outFile, hdr, strlen(hdr), &w, NULL);

    for (int k = 0; k < 20; k++) {
        double mag = sqrt(shm_out[k].real * shm_out[k].real +
                          shm_out[k].imag * shm_out[k].imag);

        char line[128];
        sprintf(line, "%d\t%.4f\t%.4f\t%.4f\r\n",
                k, shm_out[k].real, shm_out[k].imag, mag);

        WriteFile(outFile, line, strlen(line), &w, NULL);
    }

    CloseHandle(outFile);

    log_msg("  FFT output saved (first 20 bins)\n");

    /* ───────── CLEANUP ───────── */
    log_msg("\n── [7] CLEANUP ────────────────────────────────\n");

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    VirtualFree(fft_input, 0, MEM_RELEASE);

    log_msg("  Cleanup completed\n");

    log_msg("\n✔ PROGRAM COMPLETED SUCCESSFULLY\n");

    if (g_log != INVALID_HANDLE_VALUE)
        CloseHandle(g_log);

    return 0;
}
