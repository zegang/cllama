#include <cllama/platform/os.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace cllama::platform {

namespace {
    std::map<int, HANDLE>& process_handles() {
        static std::map<int, HANDLE> m;
        return m;
    }
    std::mutex& process_mutex() {
        static std::mutex m;
        return m;
    }
}

int get_current_pid() { return (int)::GetCurrentProcessId(); }

bool process_is_alive(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD exit_code;
    bool alive = GetExitCodeProcess(h, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

int kill_process(int pid, int signum) {
    (void)signum; // Windows ignores signum, always terminates
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return -1;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok ? 0 : -1;
}

std::string temp_directory() {
    char buf[MAX_PATH + 1] = {};
    DWORD r = GetTempPathA(MAX_PATH, buf);
    std::string d = r ? std::string(buf) : ".\\";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '\\';
    return d;
}

int daemonize(const std::string& pidfile) {
    // daemon mode not supported on Windows — running as a service would require
    // CreateService which is far more involved. Just write pidfile and return.
    (void)pidfile;
    return 0;
}

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (!process_is_alive((int)pe.th32ProcessID)) continue;
            result.push_back({(int)pe.th32ProcessID, std::string(pe.szExeFile)});
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

std::string get_process_cmdline(int pid) {
    // No portable way to read another process's full command line on Windows.
    // Would need WMI or NtQueryInformationProcess (undocumented).
    (void)pid;
    return "";
}

std::string get_process_comm(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    if (!h) return "";
    char name[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    if (!QueryFullProcessImageNameA(h, 0, name, &sz)) {
        CloseHandle(h);
        return "";
    }
    CloseHandle(h);
    std::string full(name);
    auto sp = full.find_last_of("/\\");
    return (sp != std::string::npos) ? full.substr(sp + 1) : full;
}

int spawn_process(const std::string& binary, const std::vector<std::string>& args) {
    std::string cmdline = "\"" + binary + "\"";
    for (const auto& a : args) {
        if (a.find(' ') != std::string::npos)
            cmdline += " \"" + a + "\"";
        else
            cmdline += " " + a;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessA(binary.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }

    int pid = (int)pi.dwProcessId;
    {
        std::lock_guard<std::mutex> lk(process_mutex());
        process_handles()[pid] = pi.hProcess;
    }
    CloseHandle(pi.hThread);
    return pid;
}

int wait_process(int pid) {
    HANDLE h = nullptr;
    {
        std::lock_guard<std::mutex> lk(process_mutex());
        auto it = process_handles().find(pid);
        if (it == process_handles().end()) return -1;
        h = it->second;
        process_handles().erase(it);
    }
    WaitForSingleObject(h, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);
    return (int)exit_code;
}

int find_free_port() {
    // httplib already initializes Winsock, but ensure it's ready
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) { WSACleanup(); return 0; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(fd); WSACleanup(); return 0;
    }

    int len = sizeof(addr);
    if (::getsockname(fd, (struct sockaddr*)&addr, &len) == SOCKET_ERROR) {
        ::closesocket(fd); WSACleanup(); return 0;
    }

    int port = ::ntohs(addr.sin_port);
    ::closesocket(fd);
    // WSACleanup intentionally not called — httplib manages WSA lifetime
    return port;
}

} // namespace cllama::platform
