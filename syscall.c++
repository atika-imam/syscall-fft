#include <windows.h>
#include <iostream>
#include <vector>
#include <complex>
#include <time.h>

using namespace std;

#define N 1024
#define PI 3.14159265358979323846

void run_fft(complex<double>* data) {
    for (int k = 0; k < N; k++) {
        complex<double> sum(0, 0);
        for (int n = 0; n < N; n++) {
            double angle = 2 * PI * k * n / N;
            sum += data[n] * complex<double>(cos(angle), -sin(angle));
        }
    }
}

int main() {
    printf("--- WINDOWS CCP PROJECT START ---\n");
    clock_t total_start = clock();

    // 1-4. File I/O Calls
    HANDLE hFile = CreateFile("output.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written;
    WriteFile(hFile, "FFT Results", 11, &written, NULL);
    CloseHandle(hFile);

    // 5-7. Process Calls
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CreateProcess(NULL, (LPSTR)"cmd.exe /c echo Process Started", NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 8-9. Memory Calls
    complex<double>* buffer = (complex<double>*)VirtualAlloc(NULL, N * sizeof(complex<double>), MEM_COMMIT, PAGE_READWRITE);

    // 10-12. IPC Calls
    HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 1024, "Global\\MySharedMem");
    void* pBuf = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 1024);

    // 13-15. Permissions (Applied via Attributes)
    SECURITY_ATTRIBUTES sa; 
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;

    run_fft(buffer);

    VirtualFree(buffer, 0, MEM_RELEASE);
    UnmapViewOfFile(pBuf);
    CloseHandle(hMap);

    printf("Windows Execution Completed in: %f s\n", (double)(clock() - total_start) / CLOCKS_PER_SEC);
    return 0;
}