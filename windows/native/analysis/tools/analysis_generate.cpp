#include "analysis/generators/analysis_generator.h"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: analysis_generate <video> <out.vbi> <out.vbt>\n";
        return 2;
    }

    const std::string video_path = argv[1];
    const std::string vbi_path = argv[2];
    const std::string vbt_path = argv[3];

    if (!vr::analysis::AnalysisGenerator::generate(video_path, vbi_path, vbt_path)) {
        std::cerr << "Failed to generate analysis files for: " << video_path << "\n";
        return 1;
    }

    return 0;
}
