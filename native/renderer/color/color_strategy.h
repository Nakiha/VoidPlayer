#pragma once

namespace vr {

enum class ColorOutputTarget {
    kMacOSExtendedLinearDisplayP3,
    kWindowsLinearScRGB,
    kSDRToneMappedBT709,
};

constexpr ColorOutputTarget kMacOSHDROutputTarget =
    ColorOutputTarget::kMacOSExtendedLinearDisplayP3;
constexpr double kHDRReferenceWhiteNits = 203.0;
constexpr double kHLGEDRHeadroomScale = 4.0;

constexpr double kBT2020ToDisplayP3RFromR = 1.3435782526;
constexpr double kBT2020ToDisplayP3RFromG = -0.2821796705;
constexpr double kBT2020ToDisplayP3RFromB = -0.0613985821;
constexpr double kBT2020ToDisplayP3GFromR = -0.0652974528;
constexpr double kBT2020ToDisplayP3GFromG = 1.0757879158;
constexpr double kBT2020ToDisplayP3GFromB = -0.0104904631;
constexpr double kBT2020ToDisplayP3BFromR = 0.0028217873;
constexpr double kBT2020ToDisplayP3BFromG = -0.0195984945;
constexpr double kBT2020ToDisplayP3BFromB = 1.0167767073;

constexpr double kBT709ToDisplayP3RFromR = 0.8224619687;
constexpr double kBT709ToDisplayP3RFromG = 0.1775380313;
constexpr double kBT709ToDisplayP3RFromB = 0.0;
constexpr double kBT709ToDisplayP3GFromR = 0.0331941989;
constexpr double kBT709ToDisplayP3GFromG = 0.9668058011;
constexpr double kBT709ToDisplayP3GFromB = 0.0;
constexpr double kBT709ToDisplayP3BFromR = 0.0170826307;
constexpr double kBT709ToDisplayP3BFromG = 0.0723974407;
constexpr double kBT709ToDisplayP3BFromB = 0.9105199286;

constexpr double kBT601ToDisplayP3RFromR = 0.7758928495;
constexpr double kBT601ToDisplayP3RFromG = 0.2127372197;
constexpr double kBT601ToDisplayP3RFromB = 0.0113699286;
constexpr double kBT601ToDisplayP3GFromR = 0.0483696384;
constexpr double kBT601ToDisplayP3GFromG = 0.9353998726;
constexpr double kBT601ToDisplayP3GFromB = 0.0162304897;
constexpr double kBT601ToDisplayP3BFromR = 0.0158600140;
constexpr double kBT601ToDisplayP3BFromG = 0.0667994164;
constexpr double kBT601ToDisplayP3BFromB = 0.9173405701;

constexpr double kBT2020ToBT709RFromR = 1.6605;
constexpr double kBT2020ToBT709RFromG = -0.5876;
constexpr double kBT2020ToBT709RFromB = -0.0728;
constexpr double kBT2020ToBT709GFromR = -0.1246;
constexpr double kBT2020ToBT709GFromG = 1.1329;
constexpr double kBT2020ToBT709GFromB = -0.0083;
constexpr double kBT2020ToBT709BFromR = -0.0182;
constexpr double kBT2020ToBT709BFromG = -0.1006;
constexpr double kBT2020ToBT709BFromB = 1.1187;

} // namespace vr
