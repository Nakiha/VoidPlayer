import AppKit
import CoreVideo
import Metal

enum MacOSPresentationMode: String {
  case flutterTextureSDR = "flutter-texture-sdr"
  case nativeCompositorSDR = "native-compositor-sdr"
  case nativeCompositorEDR = "native-compositor-edr"
}

struct MacOSPresentationConfiguration {
  let mode: MacOSPresentationMode

  var nativeCompositorEnabled: Bool {
    mode == .nativeCompositorSDR || mode == .nativeCompositorEDR
  }

  var edrOutputEnabled: Bool {
    mode == .nativeCompositorEDR
  }

  var rendererTargetPixelFormat: OSType {
    edrOutputEnabled ? kCVPixelFormatType_64RGBAHalf : kCVPixelFormatType_32BGRA
  }

  var compositorPixelFormat: MTLPixelFormat {
    edrOutputEnabled ? .rgba16Float : .bgra8Unorm
  }

  var compositorOutputMode: String {
    edrOutputEnabled ? "edr-rgba16float" : "sdr-bgra8unorm"
  }

  var diagnostics: [String: Any] {
    [
      "macOSPresentationMode": mode.rawValue,
      "macOSPresentationNativeCompositorEnabled": nativeCompositorEnabled,
      "macOSPresentationEDROutputEnabled": edrOutputEnabled,
      "macOSDisplayEDRHeadroomX1000": Self.displayEDRHeadroomX1000(),
    ]
  }

  static let current = MacOSPresentationConfiguration(
    mode: MacOSPresentationMode.fromEnvironment(ProcessInfo.processInfo.environment)
  )

  private static func displayEDRHeadroomX1000() -> Int {
    let screen = NSScreen.main
    if #available(macOS 10.15, *) {
      let headroom = screen?.maximumExtendedDynamicRangeColorComponentValue ?? 1.0
      return Int((headroom * 1000.0).rounded())
    }
    return 1000
  }
}

private extension MacOSPresentationMode {
  static func fromEnvironment(_ environment: [String: String]) -> MacOSPresentationMode {
    if let rawMode = environment["VOIDPLAYER_MACOS_PRESENTATION_MODE"]?.lowercased() {
      switch rawMode {
      case "native-compositor-edr", "edr", "hdr":
        return .nativeCompositorEDR
      case "native-compositor-sdr", "native", "compositor":
        return .nativeCompositorSDR
      case "flutter-texture-sdr", "flutter", "sdr":
        return .flutterTextureSDR
      default:
        break
      }
    }
    if environment["VOIDPLAYER_NATIVE_COMPOSITOR_SPIKE"] == "1" {
      if environment["VOIDPLAYER_NATIVE_COMPOSITOR_EDR"] == "1" ||
        environment["VOIDPLAYER_FLUTTER_HDR_SPIKE"] == "1" {
        return .nativeCompositorEDR
      }
      return .nativeCompositorSDR
    }
    return .flutterTextureSDR
  }
}
