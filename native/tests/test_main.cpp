#include <catch2/catch_session.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace {

spdlog::level::level_enum native_test_log_level() {
    const char* env_level = std::getenv("VOID_NATIVE_TEST_LOG_LEVEL");
    if (env_level == nullptr || env_level[0] == '\0') {
        return spdlog::level::warn;
    }

    auto parsed = spdlog::level::from_str(env_level);
    if (parsed == spdlog::level::off && std::string(env_level) != "off") {
        return spdlog::level::warn;
    }
    return parsed;
}

} // namespace

int main(int argc, char* argv[]) {
    auto level = native_test_log_level();
    spdlog::set_level(level);
    if (auto logger = spdlog::default_logger()) {
        logger->set_level(level);
    }

    return Catch::Session().run(argc, argv);
}
