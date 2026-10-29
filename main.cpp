// MultiYoud_Full.cpp — Roblox coklu ornek araci
// Derleme: cl /nologo /EHsc /O2 /MT MultiYoud_Full.cpp /Fe:MultiYoud_Full.exe /link psapi.lib advapi32.lib
// Yonetici olarak calistir. visual studio derleme araçları gerektirir windows 11 içindir

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

std::atomic<bool> g_running(true);
HANDLE g_holderMutex = nullptr;

// Roblox surec PID bul
DWORD FindRobloxProcess() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return pid;
}

// Handle bilgisi
struct HandleInfo {
    HANDLE handleValue;
    DWORD processId;
    std::wstring name;
};

// Surec handle'larini listele
std::vector<HandleInfo> EnumProcessHandles(DWORD pid) {
    std::vector<HandleInfo> handles;

    typedef NTSTATUS(WINAPI* NtQuerySystemInformation_t)(
        ULONG, PVOID, ULONG, PULONG);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return handles;

    auto NtQuerySystemInformation = (NtQuerySystemInformation_t)
        GetProcAddress(ntdll, "NtQuerySystemInformation");
    if (!NtQuerySystemInformation) return handles;

    ULONG bufferSize = 1024 * 1024;
    std::vector<BYTE> buffer(bufferSize);
    ULONG returnLength = 0;

    NTSTATUS status = NtQuerySystemInformation(16, buffer.data(), bufferSize, &returnLength);
    while (status == 0xC0000004L) {
        bufferSize *= 2;
        buffer.resize(bufferSize);
        status = NtQuerySystemInformation(16, buffer.data(), bufferSize, &returnLength);
    }
    if (status != 0) return handles;

    struct SYSTEM_HANDLE_ENTRY {
        ULONG ProcessId;
        BYTE ObjectTypeNumber;
        BYTE Flags;
        USHORT Handle;
        PVOID Object;
        ACCESS_MASK GrantedAccess;
    };

    struct SYSTEM_HANDLE_INFORMATION {
        ULONG HandleCount;
        SYSTEM_HANDLE_ENTRY Handles[1];
    };

    auto info = (SYSTEM_HANDLE_INFORMATION*)buffer.data();

    HANDLE hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!hProcess) return handles;

    typedef NTSTATUS(WINAPI* NtQueryObject_t)(
        HANDLE, ULONG, PVOID, ULONG, PULONG);

    auto NtQueryObject = (NtQueryObject_t)GetProcAddress(ntdll, "NtQueryObject");

    for (ULONG i = 0; i < info->HandleCount; i++) {
        auto& entry = info->Handles[i];
        if (entry.ProcessId != pid) continue;

        HANDLE dupHandle = nullptr;
        if (!DuplicateHandle(hProcess, (HANDLE)(ULONG_PTR)entry.Handle,
                             GetCurrentProcess(), &dupHandle,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            continue;
        }

        wchar_t nameBuffer[512] = { 0 };
        DWORD nameLen = 0;
        if (NtQueryObject) {
            NtQueryObject(dupHandle, 1, nameBuffer, sizeof(nameBuffer), &nameLen);
        }

        HandleInfo hi;
        hi.handleValue = (HANDLE)(ULONG_PTR)entry.Handle;
        hi.processId = entry.ProcessId;
        hi.name = nameBuffer;

        handles.push_back(hi);
        CloseHandle(dupHandle);
    }

    CloseHandle(hProcess);
    return handles;
}

// Singleton handle kapat
bool CloseSingletonHandle(DWORD pid) {
    std::wcout << L"[*] PID " << pid << L" handle'lari listeleniyor..." << std::endl;

    auto handles = EnumProcessHandles(pid);
    if (handles.empty()) {
        std::wcerr << L"[!] Handle listesi alinamadi. Yonetici izinleri gerekli." << std::endl;
        return false;
    }

    std::wcout << L"[*] Toplam " << handles.size() << L" handle bulundu." << std::endl;

    int closed = 0;
    for (auto& h : handles) {
        if (h.name.find(L"ROBLOX_singleton") != std::wstring::npos) {
            std::wcout << L"[+] Eslesen: " << h.name << L" (0x" << std::hex
                       << (ULONG_PTR)h.handleValue << std::dec << L")" << std::endl;

            HANDLE hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
            if (hProcess) {
                HANDLE dup = nullptr;
                if (DuplicateHandle(hProcess, h.handleValue, GetCurrentProcess(), &dup,
                                    0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
                    std::wcout << L"[+] Handle kapatildi." << std::endl;
                    closed++;
                    CloseHandle(dup);
                } else {
                    std::wcerr << L"[!] Kapatma basarisiz: " << GetLastError() << std::endl;
                }
                CloseHandle(hProcess);
            }
        }
    }

    if (closed == 0) {
        std::wcout << L"[*] ROBLOX_singleton eslesmesi yok. Ilk 20 isim:" << std::endl;
        int count = 0;
        for (auto& h : handles) {
            if (!h.name.empty() && count < 20) {
                std::wcout << L"    " << h.name << std::endl;
                count++;
            }
        }
    }

    return closed > 0;
}

// Handle Closer modu
void HandleCloserMode() {
    std::wcout << L"\n=== Handle Closer Modu ===" << std::endl;
    std::wcout << L"[*] Roblox araniyor..." << std::endl;

    DWORD pid = 0;
    int retries = 30;
    while (retries-- > 0 && g_running) {
        pid = FindRobloxProcess();
        if (pid != 0) break;
        std::wcout << L"[*] Bekleniyor... (" << retries << L")" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (pid == 0) {
        std::wcerr << L"[!] Roblox bulunamadi. Once bir Roblox baslatin." << std::endl;
        return;
    }

    std::wcout << L"[+] Roblox PID: " << pid << std::endl;

    if (CloseSingletonHandle(pid)) {
        std::wcout << L"\n[OK] Basarili! Ikinci Roblox'u baslatabilirsiniz." << std::endl;
    } else {
        std::wcerr << L"\n[!] Basarisiz. Yonetici olarak calistirin." << std::endl;
    }

    std::wcout << L"\n[*] Cikmak icin Ctrl+C..." << std::endl;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Mutex Holder modu
void MutexHolderMode() {
    std::wcout << L"\n=== Mutex Holder Modu ===" << std::endl;

    const wchar_t* MUTEX_NAME = L"ROBLOX_singletonEvent";

    HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME);
    if (existing) {
        std::wcout << L"[*] Mevcut mutex bulundu, kapatiliyor..." << std::endl;
        CloseHandle(existing);
        DWORD pid = FindRobloxProcess();
        if (pid) CloseSingletonHandle(pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::wcout << L"[*] Mutex olusturuluyor: " << MUTEX_NAME << std::endl;
    g_holderMutex = CreateMutexW(nullptr, FALSE, MUTEX_NAME);
    DWORD createErr = GetLastError();

    if (!g_holderMutex) {
        std::wcerr << L"[!] Olusturulamadi, hata: " << createErr << std::endl;
        if (createErr == 6) {
            std::wcout << L"[*] Hata 6: isim cakismasi. Handle Closer modunu deneyin." << std::endl;
        }
        return;
    }

    if (createErr == ERROR_ALREADY_EXISTS) {
        std::wcout << L"[*] Mutex zaten mevcut." << std::endl;
    } else {
        std::wcout << L"[+] Mutex olusturuldu ve tutuluyor." << std::endl;
    }

    std::wcout << L"\n[OK] Hazir! Roblox baslatabilirsiniz." << std::endl;
    std::wcout << L"[*] Bu pencereyi kapatmayin. Cikmak icin Ctrl+C..." << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (g_holderMutex) {
        ReleaseMutex(g_holderMutex);
        CloseHandle(g_holderMutex);
        g_holderMutex = nullptr;
    }
}

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"MultiYoud — Roblox Coklu Ornek");
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::wcout << L"============================================================" << std::endl;
    std::wcout << L"  MultiYoud — Roblox Coklu Ornek Araci" << std::endl;
    std::wcout << L"============================================================" << std::endl;
    std::wcout << L"  /mutex    — Mutex Holder (Roblox'tan once)" << std::endl;
    std::wcout << L"  /closer   — Handle Closer (Roblox acildiktan sonra)" << std::endl;
    std::wcout << L"  (bos)     — Otomatik" << std::endl;
    std::wcout << L"============================================================" << std::endl;

    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    if (!isAdmin) {
        std::wcout << L"\n[!] Yonetici degilsiniz. Handle Closer basarisiz olabilir." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    std::wstring mode;
    if (argc > 1) {
        mode = argv[1];
        if (mode == L"/mutex" || mode == L"-m") {
            MutexHolderMode();
        } else if (mode == L"/closer" || mode == L"-c") {
            HandleCloserMode();
        } else {
            std::wcerr << L"[!] Bilinmeyen parametre: " << mode << std::endl;
            return 1;
        }
    } else {
        DWORD pid = FindRobloxProcess();
        if (pid != 0) {
            std::wcout << L"\n[*] Roblox calisiyor (PID: " << pid << L"), Closer modu..." << std::endl;
            HandleCloserMode();
        } else {
            std::wcout << L"\n[*] Roblox calismiyor, Mutex modu..." << std::endl;
            MutexHolderMode();
        }
    }

    std::wcout << L"\n[MultiYoud] Cikildi." << std::endl;
    return 0;
}
