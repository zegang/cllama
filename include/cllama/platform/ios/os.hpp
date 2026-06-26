#pragma once

#include <string>
#include <vector>

namespace cllama::platform {

struct ProcessInfo {
    int         pid;
    std::string comm;
};

int  get_current_pid();
bool process_is_alive(int pid);
int  kill_process(int pid, int signum);
std::string temp_directory();
int  daemonize(const std::string& pidfile);
std::vector<ProcessInfo> list_processes();
std::string get_process_cmdline(int pid);
std::string get_process_comm(int pid);

int spawn_process(const std::string& binary, const std::vector<std::string>& args);
int wait_process(int pid);
int find_free_port();

inline constexpr int SIG_TERM = 15;
inline constexpr int SIG_KILL = 9;

} // namespace cllama::platform
