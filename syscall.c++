#include <windows.h>
#include <iostream>
#include <complex>
#include <cmath>
#include <ctime>

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
        data[k] = sum;
    }
}

int main() {
    cout << "--- WINDOWS CCP PROJECT ---\n";

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);

    HANDLE hFile;
    DWORD written;

    // ---------------- FILE I/O ----------------
    QueryPerformanceCounter(&start);

    hFile = CreateFile("input.txt", GENERIC_WRITE | GENERIC_READ,
        0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    const char* msg = "1 2 3 4 5 6 7 8";
    WriteFile(hFile, msg, strlen(msg), &written, NULL);
    CloseHandle(hFile);

    QueryPerformanceCounter(&end);

    cout << "CreateFile/WriteFile Time: "
         << (double)(end.QuadPart - start.QuadPart) / freq.QuadPart << " sec\n";

    // ---------------- PROCESS ----------------
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    QueryPerformanceCounter(&start);

    CreateProcess(NULL, (LPSTR)"cmd.exe /c echo Process Started",
        NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    WaitForSingleObject(pi.hProcess, INFINITE);

    QueryPerformanceCounter(&end);

    cout << "CreateProcess Time: "
         << (double)(end.QuadPart - start.QuadPart) / freq.QuadPart << " sec\n";

    // ---------------- MEMORY ----------------
    QueryPerformanceCounter(&start);

    complex<double>* buffer = (complex<double>*)
        VirtualAlloc(NULL, N * sizeof(complex<double>), MEM_COMMIT, PAGE_READWRITE);

    QueryPerformanceCounter(&end);

    cout << "VirtualAlloc Time: "
         << (double)(end.QuadPart - start.QuadPart) / freq.QuadPart << " sec\n";

    // Initialize data
    for (int i = 0; i < N; i++) {
        buffer[i] = complex<double>(i, 0);
    }

    // ---------------- FFT ----------------
    clock_t fft_start = clock();
    run_fft(buffer);
    clock_t fft_end = clock();

    cout << "FFT Time: "
         << (double)(fft_end - fft_start) / CLOCKS_PER_SEC << " sec\n";

    // ---------------- IPC ----------------
HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, NULL,
    PAGE_READWRITE, 0, N * sizeof(complex<double>), "MySharedMem");

if (hMap == NULL) {
    cout << "CreateFileMapping failed!\n";
    return 1;
}

void* pBuf = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);

if (pBuf == NULL) {
    cout << "MapViewOfFile failed!\n";
    return 1;
}

// Copy correct size
memcpy(pBuf, buffer, N * sizeof(complex<double>));

cout << "Shared Memory Used\n";
    // ---------------- OUTPUT ----------------
    hFile = CreateFile("output.txt", GENERIC_WRITE,
    0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

if (hFile == INVALID_HANDLE_VALUE) {
    cout << "Error creating output file!\n";
    return 1;
}

for (int i = 0; i < 10; i++) {
    string line = to_string(abs(buffer[i])) + "\n";

    BOOL success = WriteFile(hFile, line.c_str(), line.length(), &written, NULL);

    if (!success) {
        cout << "WriteFile failed!\n";
    }
}

CloseHandle(hFile);
    // ---------------- CLEANUP ----------------
    VirtualFree(buffer, 0, MEM_RELEASE);
    UnmapViewOfFile(pBuf);
    CloseHandle(hMap);

    cout << "--- COMPLETED ---\n";

    return 0;
}
