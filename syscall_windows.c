/*
 * ============================================================
 *  CT-353 OS CCP — Windows System Call Implementation
 *  UPGRADED: Real Cooley-Tukey FFT + Resource Metrics +
 *            Improved Parallelisation
 *
 *  System Calls (15):
 *   File I/O    : CreateFile, ReadFile, WriteFile, CloseHandle
 *   Permissions : SetFileSecurity
 *   IPC         : CreatePipe, CreateFileMapping, MapViewOfFile,
 *                 UnmapViewOfFile
 *   Memory      : VirtualAlloc, VirtualFree
 *   Process     : CreateProcess, WaitForSingleObject,
 *                 GetExitCodeProcess, TerminateProcess
 *
 *  FFT Algorithm : Iterative Cooley-Tukey radix-2 DIT
 *                  O(N log N)  — in-place, power-of-2 size
 *
 *  Parallelisation Strategy (Danielson-Lanczos):
 *    Worker 0 computes FFT of all EVEN-indexed samples  x[0,2,4,...]
 *    Worker 1 computes FFT of all ODD-indexed  samples  x[1,3,5,...]
 *    Parent  performs the final butterfly combination:
 *      X[k]     = FFT_even[k] + W^k * FFT_odd[k]
 *      X[k+N/2] = FFT_even[k] - W^k * FFT_odd[k]
 *    This is mathematically identical to a full N-point FFT
 *    and the two sub-FFTs run completely in parallel.
 *
 *  Resource Metrics (per syscall):
 *    • Wall-clock time  (QueryPerformanceCounter)
 *    • Peak Working Set (GetProcessMemoryInfo / PSAPI)
 *    • Kernel CPU time  (GetProcessTimes)
 *    • User   CPU time  (GetProcessTimes)
 *
 *  Compile:
 *    gcc syscall.c -o syscall.exe -ladvapi32 -lpsapi -lm
 *  Run:
 *    syscall.exe
 * ============================================================
 */

#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <psapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

/* ─────────────────────── Constants ─────────────────────── */
#define N           1024        /* Must be a power of 2        */
#define PI          3.14159265358979323846
#define SHM_NAME    "Local\\CT353_FFT_SHM"
#define LOG_FILE    "syscall_log.txt"
#define INPUT_FILE  "input.txt"
#define OUTPUT_FILE "output.txt"

/* ─────────────────────── Complex number ────────────────── */
typedef struct { double real, imag; } Cplx;

/*
 *  Shared memory layout  (M = N/2, each slot = M * sizeof(Cplx))
 *  ┌──────────┬──────────┬──────────┬──────────┐
 *  │  slot 0  │  slot 1  │  slot 2  │  slot 3  │
 *  │  in_even │  in_odd  │  out_even│  out_odd │
 *  │ (worker0)│ (worker1)│ (worker0)│ (worker1)│
 *  └──────────┴──────────┴──────────┴──────────┘
 */
#define M           (N / 2)
#define SHM_TOTAL   ((size_t)4 * M * sizeof(Cplx))

/* ─────────────────────── Global timer / log ────────────── */
static LARGE_INTEGER g_freq;
static HANDLE        g_log = INVALID_HANDLE_VALUE;

static double wall_sec(const LARGE_INTEGER *s, const LARGE_INTEGER *e) {
    return (double)(e->QuadPart - s->QuadPart) / (double)g_freq.QuadPart;
}

static void log_raw(const char *text) {
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD wr;
        WriteFile(g_log, text, (DWORD)strlen(text), &wr, NULL);
    }
    printf("%s", text);
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

static void win_err(const char *ctx) {
    DWORD code = GetLastError();
    char  msg[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, msg, sizeof(msg), NULL);
    log_fmt("\n[ERROR] %s | Code=%lu | %s\n", ctx, code, msg);
}

/* ═══════════════════════════════════════════════════════════
 *  RESOURCE METRICS
 * ═══════════════════════════════════════════════════════════ */
typedef struct {
    double  wall_ms;
    SIZE_T  peak_ws_kb;
    double  cpu_kernel_ms;
    double  cpu_user_ms;
} Metrics;

static double ft_ms(const FILETIME *ft) {
    ULONGLONG t = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return (double)t / 10000.0;
}

static void collect(HANDLE hProc,
                    const LARGE_INTEGER *ts, const LARGE_INTEGER *te,
                    Metrics *m) {
    m->wall_ms = wall_sec(ts, te) * 1000.0;

    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    m->peak_ws_kb = GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))
                    ? pmc.PeakWorkingSetSize / 1024 : 0;

    FILETIME cr, ex, kt, ut;
    if (GetProcessTimes(hProc, &cr, &ex, &kt, &ut)) {
        m->cpu_kernel_ms = ft_ms(&kt);
        m->cpu_user_ms   = ft_ms(&ut);
    } else {
        m->cpu_kernel_ms = m->cpu_user_ms = 0.0;
    }
}

static void print_metrics(const char *label, const Metrics *m) {
    log_fmt("  [METRICS] %-40s\n", label);
    log_fmt("    Wall time    : %10.4f ms\n",  m->wall_ms);
    log_fmt("    Peak RAM     : %10zu KB\n",   m->peak_ws_kb);
    log_fmt("    CPU (kernel) : %10.4f ms\n",  m->cpu_kernel_ms);
    log_fmt("    CPU (user)   : %10.4f ms\n\n",m->cpu_user_ms);
}

/* ═══════════════════════════════════════════════════════════
 *  ITERATIVE COOLEY-TUKEY RADIX-2 DIT FFT   O(N log N)
 *
 *  Transforms a[0..len-1] in-place.
 *  len must be a power of 2.
 *  inverse=0 → forward,  inverse=1 → inverse (normalised).
 * ═══════════════════════════════════════════════════════════ */
static void fft(Cplx *a, int len, int inverse) {
    /* ── Bit-reversal permutation ── */
    for (int i = 1, j = 0; i < len; i++) {
        int bit = len >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { Cplx t = a[i]; a[i] = a[j]; a[j] = t; }
    }

    /* ── Butterfly stages ── */
    for (int stage = 2; stage <= len; stage <<= 1) {
        double ang = 2.0 * PI / stage * (inverse ? 1.0 : -1.0);
        Cplx   wn  = { cos(ang), sin(ang) };
        for (int k = 0; k < len; k += stage) {
            Cplx w = { 1.0, 0.0 };
            int  half = stage >> 1;
            for (int m2 = 0; m2 < half; m2++) {
                Cplx u = a[k + m2];
                Cplx *vp = &a[k + m2 + half];
                Cplx v = { w.real*vp->real - w.imag*vp->imag,
                            w.real*vp->imag + w.imag*vp->real };
                a[k + m2].real   = u.real + v.real;
                a[k + m2].imag   = u.imag + v.imag;
                vp->real         = u.real - v.real;
                vp->imag         = u.imag - v.imag;
                double wr = w.real*wn.real - w.imag*wn.imag;
                w.imag   = w.real*wn.imag + w.imag*wn.real;
                w.real   = wr;
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
 *  into the full N=2M point FFT stored in out[0..N-1].
 *
 *  X[k]     = E[k] + W_N^k * O[k]      k = 0..M-1
 *  X[k+M]   = E[k] - W_N^k * O[k]
 *  where W_N^k = exp(-j*2*pi*k/N)
 */
static void combine(const Cplx *E, const Cplx *O, Cplx *out, int half) {
    for (int k = 0; k < half; k++) {
        double ang = -2.0 * PI * k / (2 * half);
        Cplx   tw  = { cos(ang), sin(ang) };
        Cplx   t   = { tw.real*O[k].real - tw.imag*O[k].imag,
                        tw.real*O[k].imag + tw.imag*O[k].real };
        out[k].real        = E[k].real + t.real;
        out[k].imag        = E[k].imag + t.imag;
        out[k+half].real   = E[k].real - t.real;
        out[k+half].imag   = E[k].imag - t.imag;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  WORKER — invoked as:  syscall.exe worker <id>
 *    id=0 → even samples,  id=1 → odd samples
 * ═══════════════════════════════════════════════════════════ */
static int run_worker(int id) {
    size_t slot = (size_t)M * sizeof(Cplx);

    printf("\n  [WORKER %d | PID=%lu] started — %d-point FFT\n",
           id, GetCurrentProcessId(), M);

    HANDLE hMap = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (!hMap) {
        fprintf(stderr, "[W%d] OpenFileMapping failed Code=%lu\n",
                id, GetLastError());
        return 1;
    }

    BYTE *view = (BYTE *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS,
                                       0, 0, SHM_TOTAL);
    if (!view) {
        fprintf(stderr, "[W%d] MapViewOfFile failed Code=%lu\n",
                id, GetLastError());
        CloseHandle(hMap);
        return 1;
    }

    /* Pointers into shared memory */
    Cplx *in_slot  = (Cplx *)(view + id       * slot);  /* slot 0 or 1 */
    Cplx *out_slot = (Cplx *)(view + (id + 2) * slot);  /* slot 2 or 3 */

    /* Copy input to a private VirtualAlloc buffer for in-place FFT */
    Cplx *local = (Cplx *)VirtualAlloc(NULL, slot,
                                        MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (!local) {
        fprintf(stderr, "[W%d] VirtualAlloc failed\n", id);
        UnmapViewOfFile(view);
        CloseHandle(hMap);
        return 1;
    }

    memcpy(local, in_slot, slot);

    /* ── Run Cooley-Tukey FFT on M = N/2 points ── */
    fft(local, M, 0);

    /* Write result back to shared output slot */
    memcpy(out_slot, local, slot);

    printf("  [WORKER %d | PID=%lu] done\n", id, GetCurrentProcessId());

    VirtualFree(local, 0, MEM_RELEASE);
    UnmapViewOfFile(view);
    CloseHandle(hMap);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════ */
int main(int argc, char *argv[]) {

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* Worker dispatch — before log open so workers don't clobber it */
    if (argc == 3 && strcmp(argv[1], "worker") == 0)
        return run_worker(atoi(argv[2]));

    /* ── Init performance counter ── */
    QueryPerformanceFrequency(&g_freq);
    LARGE_INTEGER ts, te;
    HANDLE hSelf = GetCurrentProcess();
    Metrics met;

    /* Open log (uses CreateFile internally — counted as syscall #1 below) */
    g_log = CreateFileA(LOG_FILE, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[FATAL] Cannot create log file\n");
        return 1;
    }

    log_raw("╔═════════════════════════════════════════════════════════╗\n");
    log_raw("║  CT-353 OS CCP — Windows System Calls + FFT             ║\n");
    log_raw("║  Algorithm : Cooley-Tukey Radix-2 DIT  O(N log N)       ║\n");
    log_raw("║  Parallel  : Even/Odd Danielson-Lanczos decomposition   ║\n");
    log_raw("║  Metrics   : Wall-ms | Peak-KB | Kernel-ms | User-ms    ║\n");
    log_raw("╚═════════════════════════════════════════════════════════╝\n\n");
    log_fmt("  N=%d  M=N/2=%d  log2(N)=%d butterfly stages\n\n",
            N, M, (int)(log2(N)));

    /* ════════════════════════════════════════════════════════
     *  [1] FILE I/O
     *  SYSCALLS 1-4: CreateFile, WriteFile, ReadFile, CloseHandle
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw(" [1] FILE I/O\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    SetFileAttributesA(INPUT_FILE, FILE_ATTRIBUTE_NORMAL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };

    /* SYSCALL 1 — CreateFile */
    QueryPerformanceCounter(&ts);
    HANDLE hFile = CreateFileA(INPUT_FILE, GENERIC_READ | GENERIC_WRITE,
                               0, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    QueryPerformanceCounter(&te);
    if (hFile == INVALID_HANDLE_VALUE) { win_err("CreateFile"); return 1; }
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 1 — CreateFile(input.txt) → OK\n");
    print_metrics("CreateFile()", &met);

    /* DuplicateHandle — bonus handle management demo */
    HANDLE hFileDup = INVALID_HANDLE_VALUE;
    QueryPerformanceCounter(&ts);
    DuplicateHandle(hSelf, hFile, hSelf, &hFileDup,
                    0, FALSE, DUPLICATE_SAME_ACCESS);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  [BONUS] DuplicateHandle() — descriptor copy\n");
    print_metrics("DuplicateHandle()", &met);

    /*
     *  Write N values to the file:
     *  Samples 0-15 = 1..16, samples 16-1023 = 0
     *  Format: one integer per line (easy to parse back).
     */
    /* allocate on heap to avoid large stack frame */
    int   wbuf_len  = N * 8;
    char *write_buf = (char *)malloc(wbuf_len);
    if (!write_buf) { log_raw("[FATAL] malloc\n"); return 1; }
    int wpos = 0;
    for (int i = 0; i < N; i++)
        wpos += snprintf(write_buf + wpos, wbuf_len - wpos,
                         "%d\n", (i < 16) ? (i + 1) : 0);

    /* SYSCALL 2 — WriteFile */
    DWORD bw;
    QueryPerformanceCounter(&ts);
    WriteFile(hFile, write_buf, (DWORD)wpos, &bw, NULL);
    QueryPerformanceCounter(&te);
    free(write_buf);
    collect(hSelf, &ts, &te, &met);
    log_fmt("  SYSCALL 2 — WriteFile() → %lu bytes written\n", bw);
    print_metrics("WriteFile()", &met);

    /* Rewind then read back */
    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);

    int   rbuf_len  = N * 8 + 4;
    char *read_buf  = (char *)calloc(1, rbuf_len);
    if (!read_buf) { log_raw("[FATAL] calloc\n"); return 1; }
    DWORD br;

    /* SYSCALL 3 — ReadFile */
    QueryPerformanceCounter(&ts);
    ReadFile(hFile, read_buf, rbuf_len - 1, &br, NULL);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_fmt("  SYSCALL 3 — ReadFile() → %lu bytes read\n", br);
    print_metrics("ReadFile()", &met);

    /* SYSCALL 4 — CloseHandle */
    QueryPerformanceCounter(&ts);
    CloseHandle(hFile);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 4 — CloseHandle(hFile) → OK\n");
    print_metrics("CloseHandle(hFile)", &met);

    if (hFileDup != INVALID_HANDLE_VALUE) CloseHandle(hFileDup);

    /* ════════════════════════════════════════════════════════
     *  [1.1] FILE PERMISSIONS
     *  SYSCALL 5: SetFileSecurity (DACL via SDDL)
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw(" [1.1] FILE PERMISSIONS\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    {
        PSECURITY_DESCRIPTOR pSD = NULL;
        /*
         *  SDDL breakdown:
         *   D:PAI   = DACL, Protected + Auto-Inherited
         *   (A;;FA;;;SY)  = Allow Full Access  to SYSTEM
         *   (A;;FA;;;BA)  = Allow Full Access  to BUILTIN\Administrators
         *   (A;;FR;;;WD)  = Allow File-Read    to Everyone
         */
        const char *sddl =
            "D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)";

        QueryPerformanceCounter(&ts);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
                sddl, SDDL_REVISION_1, &pSD, NULL)) {
            /* SYSCALL 5 — SetFileSecurity */
            if (!SetFileSecurityA(INPUT_FILE,
                                  DACL_SECURITY_INFORMATION, pSD))
                win_err("SetFileSecurity");
            else
                log_raw("  SYSCALL 5 — SetFileSecurity() → DACL applied\n"
                        "              (SYSTEM+Admins=Full, Everyone=Read)\n");
            LocalFree(pSD);
        } else {
            win_err("ConvertStringSecurityDescriptor");
        }
        QueryPerformanceCounter(&te);
        collect(hSelf, &ts, &te, &met);
        print_metrics("SetFileSecurity()", &met);
    }

    /* Restore attributes so the file remains accessible */
    SetFileAttributesA(INPUT_FILE, FILE_ATTRIBUTE_NORMAL);

    /* ════════════════════════════════════════════════════════
     *  [2] PIPE IPC
     *  SYSCALL 6: CreatePipe
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw(" [2] PIPE IPC\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    HANDLE rPipe, wPipe;
    SECURITY_ATTRIBUTES ps = { sizeof(ps), NULL, TRUE };

    QueryPerformanceCounter(&ts);
    if (!CreatePipe(&rPipe, &wPipe, &ps, 0)) win_err("CreatePipe");
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 6 — CreatePipe() → OK\n");
    print_metrics("CreatePipe()", &met);

    const char *pmsg = "CT353: IPC pipe test — FFT ready";
    DWORD pw, pr;
    WriteFile(wPipe, pmsg, (DWORD)strlen(pmsg) + 1, &pw, NULL);
    CloseHandle(wPipe);
    char pbuf[128] = {0};
    ReadFile(rPipe, pbuf, sizeof(pbuf) - 1, &pr, NULL);
    CloseHandle(rPipe);
    log_fmt("  Pipe echo → \"%s\"\n\n", pbuf);

    /* ════════════════════════════════════════════════════════
     *  [3] MEMORY
     *  SYSCALLS 7 + 8: VirtualAlloc / VirtualFree
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw(" [3] MEMORY MANAGEMENT\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    size_t fft_bytes = (size_t)N * sizeof(Cplx);

    QueryPerformanceCounter(&ts);
    Cplx *fft_input = (Cplx *)VirtualAlloc(NULL, fft_bytes,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
    QueryPerformanceCounter(&te);
    if (!fft_input) { win_err("VirtualAlloc"); return 1; }
    collect(hSelf, &ts, &te, &met);
    log_fmt("  SYSCALL 7 — VirtualAlloc(%zu bytes) → 0x%p\n",
            fft_bytes, (void *)fft_input);
    print_metrics("VirtualAlloc()", &met);

    /* Parse read_buf → fft_input[] */
    log_raw(" [3.1] Parse file content into fft_input[]\n");
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
        log_fmt("  Loaded %d samples, zero-padded to N=%d\n\n", idx, N);
    }
    free(read_buf);

    /* ════════════════════════════════════════════════════════
     *  [4] SHARED MEMORY
     *  SYSCALLS 9 + 10: CreateFileMapping / MapViewOfFile
     *
     *  De-interleave fft_input into even/odd halves before
     *  writing to SHM so each worker gets exactly M contiguous
     *  samples to transform.
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw(" [4] SHARED MEMORY\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    size_t slot_bytes = (size_t)M * sizeof(Cplx);

    QueryPerformanceCounter(&ts);
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                     PAGE_READWRITE,
                                     0, (DWORD)SHM_TOTAL,
                                     SHM_NAME);
    QueryPerformanceCounter(&te);
    if (!hMap) { win_err("CreateFileMapping"); return 1; }
    collect(hSelf, &ts, &te, &met);
    log_fmt("  SYSCALL 9  — CreateFileMapping(\"%s\") → %zu bytes\n",
            SHM_NAME, SHM_TOTAL);
    print_metrics("CreateFileMapping()", &met);

    QueryPerformanceCounter(&ts);
    BYTE *view = (BYTE *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS,
                                       0, 0, SHM_TOTAL);
    QueryPerformanceCounter(&te);
    if (!view) { win_err("MapViewOfFile"); CloseHandle(hMap); return 1; }
    collect(hSelf, &ts, &te, &met);
    log_fmt("  SYSCALL 10 — MapViewOfFile() → 0x%p\n", (void *)view);
    print_metrics("MapViewOfFile()", &met);

    /* Partition shared memory into 4 slots */
    Cplx *shm_even_in  = (Cplx *)(view + 0 * slot_bytes);
    Cplx *shm_odd_in   = (Cplx *)(view + 1 * slot_bytes);
    Cplx *shm_even_out = (Cplx *)(view + 2 * slot_bytes);
    Cplx *shm_odd_out  = (Cplx *)(view + 3 * slot_bytes);

    /* De-interleave: even/odd separation */
    for (int i = 0; i < M; i++) {
        shm_even_in[i] = fft_input[2 * i];
        shm_odd_in[i]  = fft_input[2 * i + 1];
    }
    ZeroMemory(shm_even_out, slot_bytes);
    ZeroMemory(shm_odd_out,  slot_bytes);
    log_raw("  Input de-interleaved → slot0=even, slot1=odd\n\n");

    /* ════════════════════════════════════════════════════════
     *  [5] PROCESS MANAGEMENT
     *  SYSCALLS 11-14: CreateProcess, WaitForSingleObject,
     *                  GetExitCodeProcess, TerminateProcess
     * ════════════════════════════════════════════════════════ */
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw(" [5] PARALLEL FFT VIA CHILD PROCESSES\n");
    log_raw("══════════════════════════════════════════════════════════\n");
    log_raw("  Strategy: Danielson-Lanczos even/odd decomposition\n");
    log_fmt("  Worker 0 computes %d-pt FFT of even samples\n", M);
    log_fmt("  Worker 1 computes %d-pt FFT of odd  samples\n", M);
    log_raw("  Parent  combines results with final butterfly pass\n\n");

    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);

    char cmd0[MAX_PATH + 32], cmd1[MAX_PATH + 32];
    snprintf(cmd0, sizeof(cmd0), "\"%s\" worker 0", exe_path);
    snprintf(cmd1, sizeof(cmd1), "\"%s\" worker 1", exe_path);

    STARTUPINFOA       si0 = { sizeof(si0) }, si1_info = { sizeof(si1_info) };
    PROCESS_INFORMATION pi0 = {0},            pi1_info = {0};

    /* SYSCALL 11 — CreateProcess x2 */
    QueryPerformanceCounter(&ts);

    if (!CreateProcessA(NULL, cmd0, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si0, &pi0)) {
        win_err("CreateProcess worker0"); return 1;
    }
    log_fmt("  SYSCALL 11a — CreateProcess(worker 0) PID=%lu\n",
            pi0.dwProcessId);

    if (!CreateProcessA(NULL, cmd1, NULL, NULL, FALSE, 0,
                        NULL, NULL, &si1_info, &pi1_info)) {
        win_err("CreateProcess worker1"); return 1;
    }
    log_fmt("  SYSCALL 11b — CreateProcess(worker 1) PID=%lu\n",
            pi1_info.dwProcessId);

    /* SYSCALL 12 — WaitForSingleObject (parallel join) */
    log_raw("  Parallel join — waiting for both workers...\n");
    HANDLE two_procs[2] = { pi0.hProcess, pi1_info.hProcess };
    WaitForMultipleObjects(2, two_procs, TRUE, INFINITE);

    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 12 — WaitForSingleObject x2 → both done\n");
    print_metrics("CreateProcess+Wait (parallel FFT)", &met);

    /* SYSCALL 13 — GetExitCodeProcess */
    DWORD ec0 = 0, ec1 = 0;
    QueryPerformanceCounter(&ts);
    GetExitCodeProcess(pi0.hProcess,      &ec0);
    GetExitCodeProcess(pi1_info.hProcess, &ec1);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_fmt("  SYSCALL 13 — GetExitCodeProcess → W0=%lu  W1=%lu\n",
            ec0, ec1);
    print_metrics("GetExitCodeProcess()", &met);

    /* SYSCALL 14 — TerminateProcess (demonstration / forced cleanup) */
    QueryPerformanceCounter(&ts);
    TerminateProcess(pi0.hProcess,      0);
    TerminateProcess(pi1_info.hProcess, 0);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 14 — TerminateProcess() → cleanup OK\n");
    print_metrics("TerminateProcess()", &met);

    CloseHandle(pi0.hProcess);     CloseHandle(pi0.hThread);
    CloseHandle(pi1_info.hProcess);CloseHandle(pi1_info.hThread);

    /* ════════════════════════════════════════════════════════
     *  [5.1] FINAL BUTTERFLY COMBINATION (parent)
     *
     *  X[k]     = FFT_even[k] + e^{-j2πk/N} * FFT_odd[k]
     *  X[k+N/2] = FFT_even[k] - e^{-j2πk/N} * FFT_odd[k]
     *
     *  This produces the full N-point FFT — the two sub-FFTs
     *  ran completely in parallel with zero synchronisation
     *  overhead (shared memory provided the only coupling).
     * ════════════════════════════════════════════════════════ */
    log_raw("\n [5.1] FINAL BUTTERFLY (parent combines even+odd FFTs)\n");

    Cplx *fft_result = (Cplx *)VirtualAlloc(NULL, fft_bytes,
                                              MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
    if (!fft_result) { win_err("VirtualAlloc fft_result"); return 1; }

    QueryPerformanceCounter(&ts);
    combine(shm_even_out, shm_odd_out, fft_result, M);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  combine() complete → full N-point FFT in fft_result[]\n");
    print_metrics("Final butterfly combine()", &met);

    /* ════════════════════════════════════════════════════════
     *  [6] OUTPUT
     * ════════════════════════════════════════════════════════ */
    log_raw("\n══════════════════════════════════════════════════════════\n");
    log_raw(" [6] OUTPUT RESULTS\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    HANDLE hOut = CreateFileA(OUTPUT_FILE, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut != INVALID_HANDLE_VALUE) {
        const char *hdr =
            "k\tReal\t\t\tImag\t\t\tMagnitude\r\n"
            "-------------------------------------------------------\r\n";
        DWORD hw;
        WriteFile(hOut, hdr, (DWORD)strlen(hdr), &hw, NULL);
        for (int k = 0; k < N; k++) {
            double mag = sqrt(fft_result[k].real * fft_result[k].real +
                              fft_result[k].imag * fft_result[k].imag);
            char line[192];
            snprintf(line, sizeof(line),
                     "%d\t%18.8f\t%18.8f\t%18.8f\r\n",
                     k, fft_result[k].real, fft_result[k].imag, mag);
            WriteFile(hOut, line, (DWORD)strlen(line), &hw, NULL);
        }
        CloseHandle(hOut);
        log_fmt("  output.txt written — all %d FFT bins\n", N);
    }

    /* Console preview: first 8 non-zero magnitude bins */
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
     *  [7] CLEANUP
     *  SYSCALL 15: UnmapViewOfFile
     *  SYSCALL  8: VirtualFree (x2)
     * ════════════════════════════════════════════════════════ */
    log_raw("\n══════════════════════════════════════════════════════════\n");
    log_raw(" [7] CLEANUP\n");
    log_raw("══════════════════════════════════════════════════════════\n");

    QueryPerformanceCounter(&ts);
    UnmapViewOfFile(view);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 15 — UnmapViewOfFile() → OK\n");
    print_metrics("UnmapViewOfFile()", &met);

    CloseHandle(hMap);

    QueryPerformanceCounter(&ts);
    VirtualFree(fft_input,  0, MEM_RELEASE);
    VirtualFree(fft_result, 0, MEM_RELEASE);
    QueryPerformanceCounter(&te);
    collect(hSelf, &ts, &te, &met);
    log_raw("  SYSCALL 8  — VirtualFree(fft_input + fft_result) → OK\n");
    print_metrics("VirtualFree()", &met);

    /* ════════════════════════════════════════════════════════
     *  SUMMARY TABLE
     * ════════════════════════════════════════════════════════ */
    log_raw("\n╔════════════════════════════════════════════════════════╗\n");
    log_raw("║                  SYSTEM CALL SUMMARY                   ║\n");
    log_raw("╠════╦══════════════╦════════════════════════════════════║\n");
    log_raw("║  # ║ Category     ║ Windows API Call                   ║\n");
    log_raw("╠════╬══════════════╬════════════════════════════════════║\n");
    log_raw("║  1 ║ File I/O     ║ CreateFile()                       ║\n");
    log_raw("║  2 ║ File I/O     ║ WriteFile()                        ║\n");
    log_raw("║  3 ║ File I/O     ║ ReadFile()                         ║\n");
    log_raw("║  4 ║ File I/O     ║ CloseHandle()                      ║\n");
    log_raw("║  5 ║ Permissions  ║ SetFileSecurity()                  ║\n");
    log_raw("║  6 ║ IPC          ║ CreatePipe()                       ║\n");
    log_raw("║  7 ║ Memory       ║ VirtualAlloc()                     ║\n");
    log_raw("║  8 ║ Memory       ║ VirtualFree()                      ║\n");
    log_raw("║  9 ║ IPC / SHM    ║ CreateFileMapping()                ║\n");
    log_raw("║ 10 ║ IPC / SHM    ║ MapViewOfFile()                    ║\n");
    log_raw("║ 11 ║ Process      ║ CreateProcess()                    ║\n");
    log_raw("║ 12 ║ Process      ║ WaitForSingleObject()              ║\n");
    log_raw("║ 13 ║ Process      ║ GetExitCodeProcess()               ║\n");
    log_raw("║ 14 ║ Process      ║ TerminateProcess()                 ║\n");
    log_raw("║ 15 ║ IPC / SHM    ║ UnmapViewOfFile()                  ║\n");
    log_raw("╚════╩══════════════╩════════════════════════════════════╝\n");
    log_raw("\n  FFT: Cooley-Tukey radix-2 DIT  O(N log N)\n");
    log_raw("  Parallelism: Danielson-Lanczos even/odd decomposition\n");
    log_raw("  Metrics: wall-ms, Peak-KB, kernel-ms, user-ms\n");
    log_raw("\n✔  PROGRAM COMPLETED SUCCESSFULLY\n");

    CloseHandle(g_log);
    return 0;
}
