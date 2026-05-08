#include <catch2/catch_test_macros.hpp>

#include "common/logging.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <mutex>

namespace {

class CountingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    size_t count() const { return count_; }

protected:
    void sink_it_(const spdlog::details::log_msg&) override {
        ++count_;
    }

    void flush_() override {}

private:
    size_t count_ = 0;
};

void restore_test_logging() {
    vr::LogConfig config;
    config.pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";
    config.level = spdlog::level::warn;
    vr::configure_logging(config);
    spdlog::set_level(spdlog::level::warn);
    spdlog::default_logger()->set_level(spdlog::level::warn);
}

} // namespace

TEST_CASE("configure_logging preserves host default logger sinks", "[logging]") {
    auto logger = spdlog::default_logger();
    auto host_sink = std::make_shared<CountingSink>();
    logger->sinks().push_back(host_sink);

    vr::LogConfig config;
    config.pattern = "%v";
    config.level = spdlog::level::info;
    vr::configure_logging(config);

    const auto& sinks = logger->sinks();
    REQUIRE(std::find(sinks.begin(), sinks.end(), host_sink) != sinks.end());

    spdlog::info("host sink survives native logging configure");
    REQUIRE(host_sink->count() >= 1);

    logger->sinks().erase(
        std::remove(logger->sinks().begin(), logger->sinks().end(), host_sink),
        logger->sinks().end());
    restore_test_logging();
}

TEST_CASE("configure_logging rotates native UTF-8 log files", "[logging]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = dir / "voidplayer_native_rotation_test.log";
    const auto rotated = dir / "voidplayer_native_rotation_test.log.1";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(rotated, ec);

    vr::LogConfig config;
    config.file_path = path.string();
    config.pattern = "%v";
    config.level = spdlog::level::info;
    config.max_file_size = 128;
    config.max_files = 2;
    vr::configure_logging(config);

    for (int i = 0; i < 20; ++i) {
        spdlog::info("rotation-test-line-{}-abcdefghijklmnopqrstuvwxyz", i);
    }
    spdlog::default_logger()->flush();

    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(rotated));

    std::filesystem::remove(path, ec);
    std::filesystem::remove(rotated, ec);
    restore_test_logging();
}
