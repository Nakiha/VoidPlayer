#pragma once

namespace vr {

struct ColorReferenceYuv {
    double y = 0.0;
    double u = 0.5;
    double v = 0.5;
};

struct ColorReferenceRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

struct ColorReferenceConfig {
    int range = 0;
    int matrix = 0;
    int transfer = 0;
    int primaries = 0;
    bool output_edr = false;
};

} // namespace vr
