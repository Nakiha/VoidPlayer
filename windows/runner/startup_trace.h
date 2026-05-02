#ifndef RUNNER_STARTUP_TRACE_H_
#define RUNNER_STARTUP_TRACE_H_

void RunnerStartupTraceReset();
void RunnerStartupTraceMark(const char* label);
void RunnerStartupTraceFlush();

#endif  // RUNNER_STARTUP_TRACE_H_
