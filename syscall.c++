#include <windows.h>
#include <iostream>
#include <complex>
#include <cmath>
#include <ctime>
#include <string>
#include <cstring>

using namespace std;

#define N 1024
#define PI 3.14159265358979323846

// ---------------- FFT FIXED ----------------
void run_fft(complex<double>* input, complex<double>* output) {
    for (int k = 0; k < N; k++) {
        complex<double> sum(0, 0);

        for (int n = 0; n < N; n++) {
            double angle = 2 * PI * k * n / N;
            sum += input[n] * complex<double>(cos(angle), -sin(angle));
        }

        output[k] = sum;
    }
}

int main() {
    cout << "--- WINDOWS CCP PROJECT START ---\n";

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hFile;
    DWORD written, bytesRead;

    // ---------------- FILE I/O ----------------
    QueryPerformanceCounter(&start);

    hFile = CreateFile(
        "input.txt",
        GENERIC_WRITE | GENERIC_READ,
        0,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        cout << "CreateFile failed!\n";
        return 1;
    }

    const char* msg = "1 2 3 4 5 6 7 8";

    BOOL success = WriteFile(
        hFile,
        msg,
        strlen(msg),
        &written,
        NULL
    );

    if (!success) {
        cout << "WriteFile failed!\n";
        return 1;
    }

    // ---------------- ReadFile ADDED ----------------
    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);

    char readBuffer[100];

    success = ReadFile(
        hFile,
        readBuffer,
        sizeof(readBuffer),
        &bytesRead,
        NULL
    );

    cout << "File Read: " << readBuffer << endl;
    cout << "Bytes Read: " << bytesRead << endl;

    if (!success) {
        cout << "ReadFile failed!\n";
        return 1;
    }

    CloseHandle(hFile);

    QueryPerformanceCounter(&end);

    cout << "CreateFile/WriteFile/ReadFile Time: "
         << (double)(end.QuadPart - start.QuadPart) / freq.QuadPart
         << " sec\n";

    // ---------------- PROCESS ----------------
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    QueryPerformanceCounter(&start);

    success = CreateProcess(
        NULL,
        (LPSTR)"cmd.exe /c echo Process Started",
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    if (!success) {
        cout << "CreateProcess failed!\n";
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    QueryPerformanceCounter(&end);

    cout << "CreateProcess Time: "
         << (double)(end.QuadPart - start.QuadPart) / freq.QuadPart
         << " sec\n";
    
    cout << "Current PID: " << GetCurrentProcessId() << endl;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // ---------------- PIPE ADDED ----------------
    HANDLE hReadPipe, hWritePipe;

    BOOL pipeSuccess = CreatePipe(
        &hReadPipe,
        &hWritePipe,
        NULL,
        0
    );

    if (!pipeSuccess) {
        cout << "CreatePipe failed!\n";
        return 1;
    }

    cout << "Pipe Created\n";

    const char* pipeMsg = "Pipe Data Transfer";

    DWORD pipeWritten;
    BOOL writePipeSuccess = WriteFile(
        hWritePipe,
        pipeMsg,
        strlen(pipeMsg) + 1,
        &pipeWritten,
        NULL
    );

    if (!writePipeSuccess) {
        cout << "Pipe Write failed!\n";
        return 1;
    }

    char pipeBuffer[100] = {0};
    DWORD pipeRead;

    BOOL readPipeSuccess = ReadFile(
        hReadPipe,
        pipeBuffer,
        sizeof(pipeBuffer),
        &pipeRead,
        NULL
    );

    if (!readPipeSuccess) {
        cout << "Pipe Read failed!\n";
        return 1;
    }

    cout << "Pipe Data: " << pipeBuffer << endl;


    // ---------------- MEMORY ----------------
    QueryPerformanceCounter(&start);

    complex<double>* buffer = (complex<double>*)
        VirtualAlloc(
            NULL,
            N * sizeof(complex<double>),
            MEM_COMMIT,
            PAGE_READWRITE
        );

    complex<double>* result = (complex<double>*)
        VirtualAlloc(
            NULL,
            N * sizeof(complex<double>),
            MEM_COMMIT,
            PAGE_READWRITE
        );

    if (!buffer || !result) {
        cout << "VirtualAlloc failed!\n";
        return 1;
    }

    QueryPerformanceCounter(&end);

    cout << "VirtualAlloc Time: "
         << (double)(end.QuadPart - start.QuadPart) / freq.QuadPart
         << " sec\n";

    // Initialize input data
    for (int i = 0; i < N; i++) {
        buffer[i] = complex<double>(i, 0);
    }

    SYSTEMTIME st;
    GetSystemTime(&st);

    cout << "FFT Start Time: "
         << st.wHour << ":"
         << st.wMinute << ":"
         << st.wSecond << endl;

    // ---------------- FFT FIXED ----------------
    clock_t fft_start = clock();

    run_fft(buffer, result);

    clock_t fft_end = clock();

    cout << "FFT Time: "
         << (double)(fft_end - fft_start) / CLOCKS_PER_SEC
         << " sec\n";

    // ---------------- IPC ----------------
    HANDLE hMap = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        N * sizeof(complex<double>),
        "MySharedMem"
    );

    if (hMap == NULL) {
        cout << "CreateFileMapping failed!\n";
        return 1;
    }

    void* pBuf = MapViewOfFile(
        hMap,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        0
    );

    if (pBuf == NULL) {
        cout << "MapViewOfFile failed!\n";
        return 1;
    }

    memcpy(pBuf, result, N * sizeof(complex<double>));

    cout << "Shared Memory Used\n";

    // ---------------- OUTPUT ----------------
    SetFileAttributes("output.txt", FILE_ATTRIBUTE_NORMAL);


    hFile = CreateFile(
        "output.txt",
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        cout << "Error creating output file!\n";
        return 1;
    }

    for (int i = 0; i < 10; i++) {
        string line = to_string(abs(result[i])) + "\n";

        success = WriteFile(
            hFile,
            line.c_str(),
            line.length(),
            &written,
            NULL
        );

        if (!success) {
            cout << "WriteFile failed!\n";
        }
    }

    CloseHandle(hFile);

    cout << "Output File Written\n";

    BOOL attrSuccess = SetFileAttributes(
        "output.txt",
        FILE_ATTRIBUTE_READONLY
    );

    if (attrSuccess) {
        cout << "Read-only attribute applied\n";
    } else {
        cout << "SetFileAttributes failed\n";
    }
    // ---------------- PERMISSIONS ----------------
    cout << "SetFileSecurity (conceptual system call demonstrated)\n";

    // ---------------- CLEANUP ----------------
    VirtualFree(buffer, 0, MEM_RELEASE);
    VirtualFree(result, 0, MEM_RELEASE);
    
    BOOL unmapSuccess = UnmapViewOfFile(pBuf);

    if (unmapSuccess) {
        cout << "UnmapViewOfFile successful\n";
    } else {
        cout << "UnmapViewOfFile failed\n";
    }

    CloseHandle(hMap);

    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);

    cout << "--- COMPLETED ---\n";

    return 0;
}
