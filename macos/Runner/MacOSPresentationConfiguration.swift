import AppKit
import CoreVideo
import Metal

enum MacOSPresentationMode: String {
  case rendererOwnedWgpuSDR = "renderer-owned-wgpu-sdr"
  case rendererOwnedWgpuEDR = "renderer-owned-wgpu-edr"
}

struct MacOSPresentationConfiguration {
  let mode: MacOSPresentationMode
  let request: String
  let reason: String
  let displayEDRHeadroomX1000: Int

  var rendererOwnedPresentationEnabled: Bool { true }

  var edrOutputEnabled: Bool {
    mode == .rendererOwnedWgpuEDR
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
      "macOSPresentationRendererOwnedEnabled": rendererOwnedPresentationEnabled,
      "macOSPresentationEDROutputEnabled": edrOutputEnabled,
      "macOSDisplayEDRHeadroomX1000": displayEDRHeadroomX1000,
    ]
  }

  static let environment = MacOSPresentationEnvironment(
    environment: ProcessInfo.processInfo.environment
  )

  private static let lock = NSLock()
  private static var resolvedCurrent = MacOSPresentationConfiguration(
    mode: .rendererOwnedWgpuSDR,
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
    case .rendererOwnedWgpuSDR:
      if environment.isWgpuMetalRequest {
        if hasHDRTrack && supportsEDR {
          return MacOSPresentationConfiguration(
            mode: .rendererOwnedWgpuEDR,
            request: environment.request,
            reason: "forced-wgpu-metal-edr",
            displayEDRHeadroomX1000: headroomX1000
          )
        }
        return MacOSPresentationConfiguration(
          mode: .rendererOwnedWgpuSDR,
          request: environment.request,
          reason: hasHDRTrack ? "wgpu-metal-edr-display-unavailable" : "forced-wgpu-metal-sdr",
          displayEDRHeadroomX1000: headroomX1000
        )
      }
      return MacOSPresentationConfiguration(
        mode: .rendererOwnedWgpuSDR,
        request: environment.request,
        reason: "forced-renderer-owned-wgpu-sdr",
        displayEDRHeadroomX1000: headroomX1000
      )
    case .rendererOwnedWgpuEDR:
      return MacOSPresentationConfiguration(
        mode: supportsEDR ? .rendererOwnedWgpuEDR : .rendererOwnedWgpuSDR,
        request: environment.request,
        reason: supportsEDR ? "forced-renderer-owned-wgpu-edr" : "edr-display-unavailable",
        displayEDRHeadroomX1000: headroomX1000
      )
    case nil:
      if hasHDRTrack && supportsEDR {
        return MacOSPresentationConfiguration(
          mode: .rendererOwnedWgpuEDR,
          request: environment.request,
          reason: "auto-hdr-renderer-owned-metal-layer-edr",
          displayEDRHeadroomX1000: headroomX1000
        )
      }
      return MacOSPresentationConfiguration(
        mode: .rendererOwnedWgpuSDR,
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

  var isWgpuMetalRequest: Bool {
    request == "wgpu-metal" || request == "wgpu"
  }

  init(environment: [String: String]) {
    if let rawMode = environment["VOIDPLAYER_MACOS_PRESENTATION_MODE"]?.lowercased() {
      switch rawMode {
      case "renderer-owned-wgpu-edr", "edr", "hdr":
        request = rawMode
        overrideMode = .rendererOwnedWgpuEDR
        return
      case "renderer-owned-wgpu-sdr", "native", "compositor":
        request = rawMode
        overrideMode = .rendererOwnedWgpuSDR
        return
      case "sdr":
        request = rawMode
        overrideMode = .rendererOwnedWgpuSDR
        return
      case "wgpu-metal", "wgpu":
        request = rawMode
        overrideMode = .rendererOwnedWgpuSDR
        return
      case "auto":
        request = "auto"
        overrideMode = nil
        return
      default:
        break
      }
    }
    request = "auto"
    overrideMode = nil
  }
}
