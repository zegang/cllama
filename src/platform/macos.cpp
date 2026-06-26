#include <cllama/platform/os.hpp>

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <libproc.h>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>

namespace cllama::platform {

int get_current_pid() { return (int)::getpid(); }

bool process_is_alive(int pid) {
    return ::kill((pid_t)pid, 0) == 0;
}

int kill_process(int pid, int signum) {
    return ::kill((pid_t)pid, signum);
}

std::string temp_directory() {
    return "/tmp/";
}

int daemonize(const std::string& pidfile) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;
    umask(0);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    if (!pidfile.empty()) {
        std::ofstream pf(pidfile);
        if (pf) {
            pf << get_current_pid() << "\n";
            pf.close();
        }
    }

    return 0;
}

std::vector<ProcessInfo> list_processes() {
    std::vector<ProcessInfo> result;
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t bufsize = 0;
    if (sysctl(mib, 3, nullptr, &bufsize, nullptr, 0) != 0) return result;
    std::vector<char> buf(bufsize);
    if (sysctl(mib, 3, buf.data(), &bufsize, nullptr, 0) != 0) return result;

    struct kinfo_proc* proc_list = (struct kinfo_proc*)buf.data();
    int count = bufsize / sizeof(struct kinfo_proc);
    for (int i = 0; i < count; ++i) {
        pid_t pid = proc_list[i].kp_proc.p_pid;
        if (pid <= 0) continue;
        if (!process_is_alive((int)pid)) continue;

        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
        std::string full(path);
        auto sp = full.rfind('/');
        std::string comm = (sp != std::string::npos) ? full.substr(sp + 1) : full;
        result.push_back({(int)pid, comm});
    }
    return result;
}

std::string get_process_cmdline(int pid) {
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, (int)pid };
    size_t bufsize = 0;
    if (sysctl(mib, 3, nullptr, &bufsize, nullptr, 0) != 0) return "";
    std::vector<char> buf(bufsize);
    if (sysctl(mib, 3, buf.data(), &bufsize, nullptr, 0) != 0) return "";

    int argc = *(int*)buf.data();
    char* p = buf.data() + sizeof(int);
    p += strlen(p) + 1;  // skip exec path
    p += strlen(p) + 1;  // skip arg0

    std::string cmdline;
    for (int i = 1; i < argc; ++i) {
        if (!cmdline.empty()) cmdline += " ";
        cmdline += p;
        p += strlen(p) + 1;
    }
    return cmdline;
}

std::string get_process_comm(int pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath((pid_t)pid, path, sizeof(path)) <= 0) return "";
    std::string full(path);
    auto sp = full.rfind('/');
    return (sp != std::string::npos) ? full.substr(sp + 1) : full;
}

int spawn_process(const std::string& binary, const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<const char*> cargs;
        cargs.reserve(args.size() + 2);
        cargs.push_back(binary.c_str());
        for (const auto& a : args) cargs.push_back(a.c_str());
        cargs.push_back(nullptr);
        ::execvp(binary.c_str(), const_cast<char**>(cargs.data()));
        _exit(1);
    }
    return (int)pid;
}

int wait_process(int pid) {
    int status;
    if (::waitpid((pid_t)pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int find_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd); return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, (struct sockaddr*)&addr, &len) < 0) {
        ::close(fd); return 0;
    }
    int port = ::ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

} // namespace cllama::platform
