#include "startup_trace.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct StartupTraceEvent {
  std::string label;
  int64_t step_ms;
  int64_t total_ms;
};

std::mutex g_mutex;
Clock::time_point g_total;
Clock::time_point g_step;
bool g_initialized = false;
bool g_flushed = false;
std::vector<StartupTraceEvent> g_events;

void LogEvent(const StartupTraceEvent& event) {
  spdlog::info("[NativeStartup] {}: +{}ms, total={}ms", event.label,
               event.step_ms, event.total_ms);
}

}  // namespace

void RunnerStartupTraceReset() {
  std::lock_guard lock(g_mutex);
  g_total = Clock::now();
  g_step = g_total;
  g_initialized = true;
  g_flushed = false;
  g_events.clear();
}

void RunnerStartupTraceMark(const char* label) {
  std::lock_guard lock(g_mutex);
  if (!g_initialized) {
    g_total = Clock::now();
    g_step = g_total;
    g_initialized = true;
  }

  const auto now = Clock::now();
  StartupTraceEvent event{
      label ? label : "",
      std::chrono::duration_cast<std::chrono::milliseconds>(now - g_step)
          .count(),
      std::chrono::duration_cast<std::chrono::milliseconds>(now - g_total)
          .count(),
  };
  g_step = now;
  g_events.push_back(event);
  if (g_flushed) {
    LogEvent(event);
  }
}

void RunnerStartupTraceFlush() {
  std::lock_guard lock(g_mutex);
  for (const auto& event : g_events) {
    LogEvent(event);
  }
  g_flushed = true;
}
