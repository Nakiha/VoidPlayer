#pragma once

#include <string>

// Singleton that generates VBI, VBT, VBS4, and VAC2 test data on demand.
// Call ensure() at the start of each TEST_CASE — it runs once via std::call_once.
// If generation fails, ensure() returns false and the test should REQUIRE(false).
class AnalysisTestData {
public:
    static AnalysisTestData& instance();

    // Generate all test data. Returns true on success.
    // Safe to call multiple times — only generates once.
    bool ensure();

    bool is_ok() const { return ok_; }

    const std::string& vbt_path()   const { return vbt_path_; }
    const std::string& vbi_path()   const { return vbi_path_; }
    const std::string& vbs4_path()  const { return vbs4_path_; }
    const std::string& vac2_base_path() const { return vac2_base_path_; }

    // Remove generated temp directory. Called via atexit.
    void cleanup();

private:
    AnalysisTestData() = default;

    bool generate_vbi_vbt();
    bool generate_vbs4();
    bool generate_vac2_base();

    std::string temp_dir_;
    std::string vbt_path_;
    std::string vbi_path_;
    std::string vbs4_path_;
    std::string vac2_base_path_;
    bool ok_ = false;
    bool cleaned_up_ = false;
};
