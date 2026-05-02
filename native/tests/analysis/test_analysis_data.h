#pragma once

#include <string>

// Singleton that generates VBI, VBT, and VBS3 test data on demand.
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
    const std::string& vbs3_path()  const { return vbs3_path_; }
    const std::string& vac_path()   const { return vac_path_; }

    // Remove generated temp directory. Called via atexit.
    void cleanup();

private:
    AnalysisTestData() = default;

    bool generate_vbi_vbt();
    bool extract_raw_vvc();
    bool generate_vbs3();
    bool generate_container();

    std::string temp_dir_;
    std::string vbt_path_;
    std::string vbi_path_;
    std::string vbs3_path_;
    std::string vac_path_;
    std::string raw_vvc_path_;
    bool ok_ = false;
    bool cleaned_up_ = false;
};
