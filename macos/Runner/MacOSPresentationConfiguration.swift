import AppKit
import CoreVideo
import Metal

enum MacOSPresentationMode: String {
  case nativeCompositorSDR = "native-compositor-sdr"
  case nativeCompositorEDR = "native-compositor-edr"
}

struct MacOSPresentationConfiguration {
  let mode: MacOSPresentationMode
  let request: String
  let reason: String
  let displayEDRHeadroomX1000: Int

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
      "macOSPresentationRequest": request,
      "macOSPresentationReason": reason,
      "macOSPresentationNativeCompositorEnabled": nativeCompositorEnabled,
      "macOSPresentationEDROutputEnabled": edrOutputEnabled,
      "macOSDisplayEDRHeadroomX1000": displayEDRHeadroomX1000,
    ]
  }

  static let environment = MacOSPresentationEnvironment(
    environment: ProcessInfo.processInfo.environment
  )

  private static let lock = NSLock()
  private static var resolvedCurrent = MacOSPresentationConfiguration(
    mode: .nativeCompositorSDR,
    request: environment.request,
    reason: "auto-unresolved-sdr",
    displayEDRHeadroomX1000: 1000
  )

  static var current: MacOSPresentationConfiguration {
    lock.lock()
    defer { lock.unlock() }
    return resolvedCurrent
  }

  static func resetForNoMedia() {
    updateCurrent(resolve(hasHDRTrack: false, screen: nil))
  }

  static func resolve(hasHDRTrack: Bool, screen: NSScreen?) -> MacOSPresentationConfiguration {
    let environment = Self.environment
    let headroomX1000 = displayEDRHeadroomX1000(screen: screen)
    let supportsEDR = headroomX1000 > 1000
    switch environment.overrideMode {
    case .nativeCompositorSDR:
      return MacOSPresentationConfiguration(
        mode: .nativeCompositorSDR,
        request: environment.request,
        reason: "forced-native-compositor-sdr",
        displayEDRHeadroomX1000: headroomX1000
      )
    case .nativeCompositorEDR:
      return MacOSPresentationConfiguration(
        mode: supportsEDR ? .nativeCompositorEDR : .nativeCompositorSDR,
        request: environment.request,
        reason: supportsEDR ? "forced-native-compositor-edr" : "edr-display-unavailable",
        displayEDRHeadroomX1000: headroomX1000
      )
    case nil:
      if hasHDRTrack && supportsEDR {
        return MacOSPresentationConfiguration(
          mode: .nativeCompositorEDR,
          request: environment.request,
          reason: "auto-hdr-track",
          displayEDRHeadroomX1000: headroomX1000
        )
      }
      return MacOSPresentationConfiguration(
        mode: .nativeCompositorSDR,
        request: environment.request,
        reason: hasHDRTrack ? "auto-hdr-display-unavailable" : "auto-sdr-only",
        displayEDRHeadroomX1000: headroomX1000
      )
    }
  }

  static func updateCurrent(_ configuration: MacOSPresentationConfiguration) {
    lock.lock()
    resolvedCurrent = configuration
    lock.unlock()
  }

  static var displaySupportsEDR: Bool {
    displayEDRHeadroomX1000(screen: nil) > 1000
  }

  private static func displayEDRHeadroomX1000(screen: NSScreen?) -> Int {
    let screen = screen ?? NSScreen.main
    if #available(macOS 10.15, *) {
      var headroom = screen?.maximumExtendedDynamicRangeColorComponentValue ?? 1.0
      if #available(macOS 14.0, *) {
        headroom = max(
          headroom,
          screen?.maximumPotentialExtendedDynamicRangeColorComponentValue ?? 1.0
        )
      }
      return Int((headroom * 1000.0).rounded())
    }
    return 1000
  }
}

struct MacOSPresentationEnvironment {
  let request: String
  let overrideMode: MacOSPresentationMode?

  init(environment: [String: String]) {
    if let rawMode = environment["VOIDPLAYER_MACOS_PRESENTATION_MODE"]?.lowercased() {
      switch rawMode {
      case "native-compositor-edr", "edr", "hdr":
        request = rawMode
        overrideMode = .nativeCompositorEDR
        return
      case "native-compositor-sdr", "native", "compositor":
        request = rawMode
        overrideMode = .nativeCompositorSDR
        return
      case "auto":
        request = "auto"
        overrideMode = nil
        return
      default:
        break
      }
    }
    if environment["VOIDPLAYER_NATIVE_COMPOSITOR"] == "1" {
      if environment["VOIDPLAYER_NATIVE_COMPOSITOR_EDR"] == "1" {
        request = "native-compositor-edr-env"
        overrideMode = .nativeCompositorEDR
        return
      }
      request = "native-compositor-sdr-env"
      overrideMode = .nativeCompositorSDR
      return
    }
    request = "auto"
    overrideMode = nil
  }
}
