#pragma once

#include <string>

struct NativeLoggingBootstrapResult {
    std::string level;
    std::string file_path;
    std::string logs_dir;
};

class NativeLoggingBootstrap {
public:
    NativeLoggingBootstrapResult InitializeDefaults();
    NativeLoggingBootstrapResult Reconfigure(
        std::string level,
        std::string logs_dir,
        std::string log_file_name);

private:
    void EnsureDefaultPaths();
    NativeLoggingBootstrapResult Configure(std::string level);

    std::string logs_dir_;
    std::string log_file_name_;
};
