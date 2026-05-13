#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>
#include <filesystem>

/// Simple LoadLibrary-based DLL injector for BS1SDK.
/// 
/// Usage: BS1Injector.exe [path_to_dll]
///   If no path specified, looks for BS1SDK.dll in current directory.
///
/// The injector:
/// 1. Finds the BioShock Remastered process
/// 2. Allocates memory in the target process for the DLL path
/// 3. Creates a remote thread calling LoadLibraryA with the path
/// 4. Waits for injection to complete
///
/// For development, prefer using a proxy DLL (d3d9.dll, dinput8.dll)
/// for automatic loading without an external injector.

static const char* TARGET_PROCESS_NAMES[] = {
    "BioshockHD.exe",
    "Bioshock.exe",
    "BioshockRemastered.exe",
};

DWORD FindTargetProcess()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    if (Process32First(snapshot, &entry)) {
        do {
            for (const char* name : TARGET_PROCESS_NAMES) {
                if (_stricmp(entry.szExeFile, name) == 0) {
                    CloseHandle(snapshot);
                    return entry.th32ProcessID;
                }
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return 0;
}

bool InjectDLL(DWORD processId, const std::string& dllPath)
{
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | 
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, processId
    );

    if (!hProcess) {
        std::cerr << "[ERROR] Failed to open process " << processId 
                  << " (error: " << GetLastError() << ")\n";
        std::cerr << "        Try running as Administrator.\n";
        return false;
    }

    // Allocate memory in target for DLL path string
    size_t pathSize = dllPath.size() + 1;
    void* remoteMem = VirtualAllocEx(hProcess, nullptr, pathSize, 
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        std::cerr << "[ERROR] VirtualAllocEx failed\n";
        CloseHandle(hProcess);
        return false;
    }

    // Write DLL path to target process memory
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), pathSize, nullptr)) {
        std::cerr << "[ERROR] WriteProcessMemory failed\n";
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Get address of LoadLibraryA in kernel32 (same in all processes)
    FARPROC loadLibAddr = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    if (!loadLibAddr) {
        std::cerr << "[ERROR] Could not find LoadLibraryA\n";
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Create remote thread calling LoadLibraryA(dllPath)
    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibAddr),
        remoteMem, 0, nullptr
    );

    if (!hThread) {
        std::cerr << "[ERROR] CreateRemoteThread failed (error: " << GetLastError() << ")\n";
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Wait for injection to complete
    std::cout << "[*] Waiting for DLL to load...\n";
    WaitForSingleObject(hThread, 10000);

    // Check if LoadLibrary succeeded (return value = module handle)
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    if (exitCode == 0) {
        std::cerr << "[ERROR] LoadLibraryA returned NULL - DLL failed to load\n";
        std::cerr << "        Check that the DLL path is correct and dependencies are met.\n";
        return false;
    }

    return true;
}

int main(int argc, char* argv[])
{
    std::cout << "╔══════════════════════════════════════╗\n";
    std::cout << "║     BS1SDK Injector v0.1.0          ║\n";
    std::cout << "║     BioShock Remastered SDK         ║\n";
    std::cout << "╚══════════════════════════════════════╝\n\n";

    // Determine DLL path
    std::string dllPath;
    if (argc > 1) {
        dllPath = argv[1];
    } else {
        // Look for BS1SDK.dll in same directory as injector
        std::filesystem::path exePath(argv[0]);
        dllPath = (exePath.parent_path() / "BS1SDK.dll").string();
    }

    // Resolve to absolute path
    dllPath = std::filesystem::absolute(dllPath).string();

    if (!std::filesystem::exists(dllPath)) {
        std::cerr << "[ERROR] DLL not found: " << dllPath << "\n";
        return 1;
    }

    std::cout << "[*] DLL: " << dllPath << "\n";

    // Find target process
    std::cout << "[*] Searching for BioShock process...\n";
    DWORD pid = FindTargetProcess();

    if (pid == 0) {
        std::cerr << "[ERROR] BioShock process not found. Is the game running?\n";
        std::cout << "        Looking for: ";
        for (const char* name : TARGET_PROCESS_NAMES) {
            std::cout << name << " ";
        }
        std::cout << "\n";
        return 1;
    }

    std::cout << "[+] Found process (PID: " << pid << ")\n";

    // Inject
    std::cout << "[*] Injecting...\n";
    if (InjectDLL(pid, dllPath)) {
        std::cout << "[+] Injection successful!\n";
        std::cout << "    Press INSERT in-game to toggle overlay.\n";
        std::cout << "    Press F12 to unload SDK.\n";
        return 0;
    }

    std::cerr << "[-] Injection failed.\n";
    return 1;
}
