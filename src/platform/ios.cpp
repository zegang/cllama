#include <cllama/platform/os.hpp>

// iOS has heavy sandbox restrictions: no fork(), no /proc, no process enumeration.
// All functions return minimal/empty results.

#include <unistd.h>
#include <cstdlib>

namespace cllama::platform {

int get_current_pid() { return (int)::getpid(); }

bool process_is_alive(int pid) {
    // On iOS we can only check our own PID
    return pid == get_current_pid();
}

int kill_process(int pid, int signum) {
    (void)signum;
    return ::kill((pid_t)pid, SIGKILL);
}

std::string temp_directory() {
    return "/tmp/";
}

int daemonize(const std::string& pidfile) {
    // fork() is not available in iOS app sandboxes
    (void)pidfile;
    return 0;
}

std::vector<ProcessInfo> list_processes() {
    // Process enumeration is not allowed on iOS
    return {};
}

std::string get_process_cmdline(int pid) {
    (void)pid;
    return "";
}

std::string get_process_comm(int pid) {
    (void)pid;
    return "";
}

int spawn_process(const std::string& binary, const std::vector<std::string>& args) {
    // fork() not available in iOS sandbox
    (void)binary; (void)args;
    return -1;
}

int wait_process(int pid) {
    (void)pid;
    return -1;
}

int find_free_port() {
    // iOS: process spawning not supported, so free port not needed
    return 0;
}

} // namespace cllama::platform
