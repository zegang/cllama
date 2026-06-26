#include <cllama/platform/os.hpp>

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
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
    DIR* dir = opendir("/proc");
    if (!dir) return result;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        bool all_digits = true;
        for (char* p = entry->d_name; *p; ++p)
            if (!std::isdigit(*p)) { all_digits = false; break; }
        if (!all_digits) continue;

        int pid = std::stoi(entry->d_name);
        if (!process_is_alive(pid)) continue;

        // skip zombies
        {
            std::ifstream sf(std::string("/proc/") + entry->d_name + "/status");
            std::string line;
            while (std::getline(sf, line))
                // "State:\tZ (zombie)" → state char at position 7
                if (line.size() > 8 && line[0]=='S' && line[1]=='t' && line[2]=='a' && line[3]=='t' && line[4]=='e' && line[7]=='Z')
                    { goto zombie_skip; }
        }
        goto not_zombie;
        zombie_skip: continue;
        not_zombie: ;

        // read comm
        std::string comm;
        {
            std::ifstream cf(std::string("/proc/") + entry->d_name + "/comm");
            std::getline(cf, comm);
        }

        result.push_back({pid, comm});
    }
    closedir(dir);
    return result;
}

std::string get_process_cmdline(int pid) {
    std::string cmdline;
    std::ifstream ifs(std::string("/proc/") + std::to_string(pid) + "/cmdline");
    if (!ifs) return cmdline;
    std::string seg;
    while (std::getline(ifs, seg, '\0'))
        cmdline += (cmdline.empty() ? "" : " ") + seg;
    return cmdline;
}

std::string get_process_comm(int pid) {
    std::ifstream cf(std::string("/proc/") + std::to_string(pid) + "/comm");
    std::string comm;
    std::getline(cf, comm);
    return comm;
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
