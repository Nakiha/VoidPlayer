#include "native_player_bridge.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <typeinfo>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

namespace {

constexpr int kFatalSignals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE};

std::atomic<bool> g_installed{false};
std::terminate_handler g_previous_terminate = nullptr;
struct sigaction g_previous_actions[sizeof(kFatalSignals) / sizeof(kFatalSignals[0])];
bool g_previous_actions_valid[sizeof(kFatalSignals) / sizeof(kFatalSignals[0])] = {};
char g_crash_dir[PATH_MAX] = {};

int signal_slot(int signal_number) {
  for (size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); ++i) {
    if (kFatalSignals[i] == signal_number) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string current_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
  localtime_r(&time, &local_time);

  std::ostringstream out;
  out << std::put_time(&local_time, "%Y%m%d_%H%M%S");
  return out.str();
}

std::filesystem::path crash_log_path(const char* prefix) {
  const char* dir = g_crash_dir;
  if (!dir[0]) {
    return {};
  }

  std::ostringstream name;
  name << prefix << "_" << current_timestamp() << "_" << getpid() << ".log";
  return std::filesystem::path(dir) / name.str();
}

void write_backtrace(std::ofstream& out) {
  void* frames[64] = {};
  const int frame_count = backtrace(frames, 64);
  char** symbols = backtrace_symbols(frames, frame_count);
  if (!symbols) {
    out << "backtrace: unavailable\n";
    return;
  }

  out << "backtrace:\n";
  for (int i = 0; i < frame_count; ++i) {
    out << "  " << symbols[i] << "\n";
  }
  std::free(symbols);
}

void write_terminate_log() {
  const auto path = crash_log_path("crash_macos_terminate");
  if (path.empty()) {
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    return;
  }

  out << "VoidPlayer macOS native crash\n";
  out << "kind: std::terminate\n";
  out << "pid: " << getpid() << "\n";
  out << "timestamp: " << current_timestamp() << "\n";

  const auto exception = std::current_exception();
  if (exception) {
    try {
      std::rethrow_exception(exception);
    } catch (const std::exception& exc) {
      out << "exception_type: " << typeid(exc).name() << "\n";
      out << "exception_what: " << exc.what() << "\n";
    } catch (...) {
      out << "exception_type: unknown\n";
    }
  } else {
    out << "exception_type: none\n";
  }

  write_backtrace(out);
  out.flush();
}

void append_text(int fd, const char* text) {
  if (!text) {
    return;
  }
  const size_t len = std::strlen(text);
  if (len > 0) {
    (void)write(fd, text, len);
  }
}

void write_signal_log(int signal_number, siginfo_t* info) {
  char path[PATH_MAX] = {};
  if (!g_crash_dir[0]) {
    return;
  }

  const int path_len = std::snprintf(path,
                                     sizeof(path),
                                     "%s/crash_macos_signal_%ld_%d.log",
                                     g_crash_dir,
                                     static_cast<long>(getpid()),
                                     signal_number);
  if (path_len <= 0 || path_len >= static_cast<int>(sizeof(path))) {
    return;
  }

  const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }

  char line[512] = {};
  std::snprintf(line,
                sizeof(line),
                "VoidPlayer macOS native crash\nkind: signal\npid: %ld\nsignal: %d\n",
                static_cast<long>(getpid()),
                signal_number);
  append_text(fd, line);
  if (info) {
    std::snprintf(line,
                  sizeof(line),
                  "signal_code: %d\nfault_address: %p\n",
                  info->si_code,
                  info->si_addr);
    append_text(fd, line);
  }
  append_text(fd, "backtrace:\n");
  void* frames[64] = {};
  const int frame_count = backtrace(frames, 64);
  if (frame_count > 0) {
    backtrace_symbols_fd(frames, frame_count, fd);
  } else {
    append_text(fd, "  unavailable\n");
  }
  close(fd);
}

void terminate_handler() {
  write_terminate_log();
  if (g_previous_terminate && g_previous_terminate != terminate_handler) {
    g_previous_terminate();
  }
  std::abort();
}

void fatal_signal_handler(int signal_number, siginfo_t* info, void*) {
  write_signal_log(signal_number, info);

  const int slot = signal_slot(signal_number);
  if (slot >= 0 && g_previous_actions_valid[slot]) {
    (void)sigaction(signal_number, &g_previous_actions[slot], nullptr);
  } else {
    signal(signal_number, SIG_DFL);
  }
  raise(signal_number);
  _exit(128 + signal_number);
}

} // namespace

extern "C" void VPMacOSInstallCrashHandler(const char* crash_dir) {
  if (!crash_dir || !crash_dir[0]) {
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(crash_dir, ec);

  std::snprintf(g_crash_dir, sizeof(g_crash_dir), "%s", crash_dir);

  if (g_installed.exchange(true)) {
    return;
  }

  g_previous_terminate = std::set_terminate(terminate_handler);

  struct sigaction action {};
  action.sa_sigaction = fatal_signal_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO;

  for (size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); ++i) {
    if (sigaction(kFatalSignals[i], nullptr, &g_previous_actions[i]) == 0) {
      g_previous_actions_valid[i] = true;
    }
    (void)sigaction(kFatalSignals[i], &action, nullptr);
  }
}

extern "C" void VPMacOSRemoveCrashHandler(void) {
  if (!g_installed.exchange(false)) {
    return;
  }

  if (g_previous_terminate) {
    std::set_terminate(g_previous_terminate);
    g_previous_terminate = nullptr;
  }

  for (size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); ++i) {
    if (g_previous_actions_valid[i]) {
      (void)sigaction(kFatalSignals[i], &g_previous_actions[i], nullptr);
      g_previous_actions_valid[i] = false;
    }
  }

  g_crash_dir[0] = '\0';
}
