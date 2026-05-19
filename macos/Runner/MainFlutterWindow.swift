import Cocoa
import CoreVideo
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    MacOSVideoRendererStub.register(with: flutterViewController.engine)
    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

private final class MacOSVideoRendererStub: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static let syntheticDurationUs = 10_000_000

  private let textureRegistry: FlutterTextureRegistry
  private var texture: MacOSSyntheticTexture?
  private var textureId: Int64?
  private var tracks: [[String: Any]] = []
  private var layout: [String: Any] = MacOSVideoRendererStub.defaultLayout()
  private var currentPtsUs = 0
  private var isPlaying = false

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
    super.init()
  }

  static func register(with engine: FlutterEngine) {
    let stub = MacOSVideoRendererStub(textureRegistry: engine)
    let messenger = engine.binaryMessenger
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    channel.setMethodCallHandler(stub.handle)

    let events = FlutterEventChannel(name: eventsChannelName, binaryMessenger: messenger)
    events.setStreamHandler(stub)
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging", "setSpeed", "setLoopRange", "setAudibleTrack",
         "setViewportBackgroundColor", "setTrackOffset":
      result(nil)
    case "createPlayer":
      result(createPlayer(arguments: call.arguments))
    case "destroyPlayer":
      destroyPlayer()
      result(nil)
    case "addTrack":
      result(addTrack(arguments: call.arguments))
    case "removeTrack":
      removeTrack(arguments: call.arguments)
      result(nil)
    case "resize":
      resize(arguments: call.arguments)
      result(nil)
    case "play":
      isPlaying = textureId != nil
      result(nil)
    case "pause":
      isPlaying = false
      result(nil)
    case "seek":
      let targetPtsUs = intArg(call.arguments, "ptsUs") ?? 0
      currentPtsUs = max(0, min(Self.syntheticDurationUs, targetPtsUs))
      markFrameAvailable()
      result(nil)
    case "stepForward":
      currentPtsUs = min(Self.syntheticDurationUs, currentPtsUs + 33_333)
      markFrameAvailable()
      result(nil)
    case "stepBackward":
      currentPtsUs = max(0, currentPtsUs - 33_333)
      markFrameAvailable()
      result(nil)
    case "currentPts":
      result(currentPtsUs)
    case "duration":
      result(tracks.isEmpty ? 0 : Self.syntheticDurationUs)
    case "currentPresentedFrame":
      result(
        textureId == nil
          ? nil
          : ["ptsUs": currentPtsUs, "dtsUs": currentPtsUs]
      )
    case "isPlaying":
      result(isPlaying)
    case "getLayout":
      result(layout)
    case "applyLayout":
      if let nextLayout = call.arguments as? [String: Any] {
        layout = nextLayout
      }
      result(nil)
    case "getTracks":
      result(tracks)
    case "pickFiles":
      pickFiles(arguments: call.arguments, result: result)
    case "getDiagnostics":
      result([
        "platform": "macos",
        "backend": "synthetic-texture",
        "available": false,
        "reason": "macOS native playback is not implemented yet; " +
          "synthetic FlutterTexture bridge is active for port validation",
        "textureId": textureId ?? -1,
        "trackCount": tracks.count,
      ])
    case "captureViewport":
      result(captureViewport())
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func createPlayer(arguments: Any?) -> [String: Any] {
    destroyPlayer()

    let width = max(16, intArg(arguments, "width") ?? 1920)
    let height = max(16, intArg(arguments, "height") ?? 1080)
    let paths = stringListArg(arguments, "videoPaths")
    let syntheticTexture = MacOSSyntheticTexture(width: width, height: height)
    let registeredTextureId = textureRegistry.register(syntheticTexture)

    texture = syntheticTexture
    textureId = registeredTextureId
    tracks = paths.enumerated().map { index, path in
      trackMap(
        fileId: index,
        slot: index,
        path: path,
        width: width,
        height: height
      )
    }
    currentPtsUs = 0
    isPlaying = false
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": tracks,
    ]
  }

  private func destroyPlayer() {
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    tracks.removeAll()
    currentPtsUs = 0
    isPlaying = false
  }

  private func addTrack(arguments: Any?) -> Any {
    guard textureId != nil else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "createPlayer must be called before addTrack",
        details: nil
      )
    }

    let fileId = (tracks.map { intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
    let slot = tracks.count
    let path = stringArg(arguments, "path") ?? "macos-synthetic-\(fileId)"
    let size = texture?.dimensions() ?? (width: 1920, height: 1080)
    let track = trackMap(
      fileId: fileId,
      slot: slot,
      path: path,
      width: size.width,
      height: size.height
    )
    tracks.append(track)
    markFrameAvailable()
    return track
  }

  private func removeTrack(arguments: Any?) {
    guard let fileId = intArg(arguments, "fileId") else { return }
    tracks.removeAll { intValue($0["fileId"]) == fileId }
    tracks = tracks.enumerated().map { index, track in
      var next = track
      next["slot"] = index
      return next
    }
    if tracks.isEmpty {
      destroyPlayer()
    } else {
      markFrameAvailable()
    }
  }

  private func resize(arguments: Any?) {
    let width = intArg(arguments, "width")
    let height = intArg(arguments, "height")
    if let width, let height {
      texture?.resize(width: max(16, width), height: max(16, height))
    }
    markFrameAvailable()
  }

  private func captureViewport() -> Any {
    guard let texture else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "No macOS synthetic texture is registered",
        details: nil
      )
    }
    let metrics = texture.captureMetrics()

    return [
      "hash": metrics.hash,
      "width": metrics.width,
      "height": metrics.height,
      "avgLuma": metrics.avgLuma,
      "nonBlackRatio": metrics.nonBlackRatio,
    ]
  }

  private func markFrameAvailable() {
    if let id = textureId {
      textureRegistry.textureFrameAvailable(id)
    }
  }

  private func trackMap(
    fileId: Int,
    slot: Int,
    path: String,
    width: Int,
    height: Int
  ) -> [String: Any] {
    return [
      "fileId": fileId,
      "slot": slot,
      "path": path,
      "width": width,
      "height": height,
      "durationUs": Self.syntheticDurationUs,
      "startTimeUs": 0,
      "bitRate": 0,
      "formatName": "synthetic",
      "codecName": "macos_synthetic",
      "codecLongName": "macOS Synthetic FlutterTexture",
      "decoderName": "synthetic",
    ]
  }

  private static func defaultLayout() -> [String: Any] {
    return [
      "mode": 0,
      "splitPos": 0.5,
      "zoomRatio": 1.0,
      "viewOffsetX": 0.0,
      "viewOffsetY": 0.0,
      "pixelSizeMode": 0,
      "order": [0, 1, 2, 3],
    ]
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    return nil
  }

  private func pickFiles(arguments: Any?, result: @escaping FlutterResult) {
    let allowsMultipleSelection = boolArg(arguments, "allowMultiple") ?? true
    DispatchQueue.main.async {
      let panel = NSOpenPanel()
      panel.canChooseFiles = true
      panel.canChooseDirectories = false
      panel.allowsMultipleSelection = allowsMultipleSelection
      panel.resolvesAliases = true

      panel.begin { response in
        if response == .OK {
          result(panel.urls.map(\.path))
        } else {
          result(nil)
        }
      }
    }
  }

  private func intArg(_ arguments: Any?, _ key: String) -> Int? {
    guard let map = arguments as? [String: Any] else { return nil }
    return intValue(map[key])
  }

  private func boolArg(_ arguments: Any?, _ key: String) -> Bool? {
    guard let map = arguments as? [String: Any] else { return nil }
    if let value = map[key] as? Bool {
      return value
    }
    if let value = map[key] as? NSNumber {
      return value.boolValue
    }
    return nil
  }

  private func stringArg(_ arguments: Any?, _ key: String) -> String? {
    guard let map = arguments as? [String: Any] else { return nil }
    return map[key] as? String
  }

  private func stringListArg(_ arguments: Any?, _ key: String) -> [String] {
    guard let map = arguments as? [String: Any] else { return [] }
    if let values = map[key] as? [String] {
      return values
    }
    if let values = map[key] as? [Any] {
      return values.compactMap { $0 as? String }
    }
    return []
  }

  private func intValue(_ value: Any?) -> Int? {
    if let value = value as? Int {
      return value
    }
    if let value = value as? Int64 {
      return Int(value)
    }
    if let value = value as? NSNumber {
      return value.intValue
    }
    return nil
  }
}

private final class MacOSSyntheticTexture: NSObject, FlutterTexture {
  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private var pixelBuffer: CVPixelBuffer?

  init(width: Int, height: Int) {
    self.width = width
    self.height = height
    super.init()
    rebuildPixelBuffer()
  }

  func resize(width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    guard width != self.width || height != self.height else { return }
    self.width = width
    self.height = height
    rebuildPixelBufferLocked()
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else { return nil }
    return Unmanaged.passRetained(pixelBuffer)
  }

  func dimensions() -> (width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    return (width: width, height: height)
  }

  func captureMetrics() -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String
  ) {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "macos-synthetic-empty"
      )
    }
    return measure(buffer: pixelBuffer)
  }

  private func rebuildPixelBuffer() {
    lock.lock()
    defer { lock.unlock() }

    rebuildPixelBufferLocked()
  }

  private func rebuildPixelBufferLocked() {
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    var nextBuffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      kCVPixelFormatType_32BGRA,
      attributes,
      &nextBuffer
    )
    guard status == kCVReturnSuccess, let nextBuffer else {
      pixelBuffer = nil
      return
    }

    fill(buffer: nextBuffer)
    pixelBuffer = nextBuffer
  }

  private func measure(buffer: CVPixelBuffer) -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String
  ) {
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "macos-synthetic-empty"
      )
    }

    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    var lumaSum = 0.0
    var nonBlack = 0
    var hash: UInt64 = 14_695_981_039_346_656_037

    for y in 0..<height {
      for x in 0..<width {
        let offset = y * bytesPerRow + x * 4
        let b = pixels[offset + 0]
        let g = pixels[offset + 1]
        let r = pixels[offset + 2]
        let luma = 0.2126 * Double(r) + 0.7152 * Double(g) + 0.0722 * Double(b)
        lumaSum += luma
        if r > 4 || g > 4 || b > 4 {
          nonBlack += 1
        }

        hash ^= UInt64(r)
        hash = hash &* 1_099_511_628_211
        hash ^= UInt64(g)
        hash = hash &* 1_099_511_628_211
        hash ^= UInt64(b)
        hash = hash &* 1_099_511_628_211
      }
    }

    let pixelCount = max(1, width * height)
    return (
      width: width,
      height: height,
      avgLuma: lumaSum / Double(pixelCount),
      nonBlackRatio: Double(nonBlack) / Double(pixelCount),
      hash: String(format: "macos-synthetic-%dx%d-%016llx", width, height, hash)
    )
  }

  private func fill(buffer: CVPixelBuffer) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    let barWidth = max(1, width / 6)

    for y in 0..<height {
      for x in 0..<width {
        let bar = min(5, x / barWidth)
        let gradient = UInt8((x * 63) / max(1, width - 1))
        let scanline = UInt8((y * 31) / max(1, height - 1))
        let color = colorForBar(bar, gradient: gradient, scanline: scanline)
        let offset = y * bytesPerRow + x * 4
        pixels[offset + 0] = color.b
        pixels[offset + 1] = color.g
        pixels[offset + 2] = color.r
        pixels[offset + 3] = 255
      }
    }
  }

  private func colorForBar(
    _ bar: Int,
    gradient: UInt8,
    scanline: UInt8
  ) -> (b: UInt8, g: UInt8, r: UInt8) {
    switch bar {
    case 0:
      return (32, 48 &+ scanline, 220)
    case 1:
      return (32 &+ gradient, 180, 64)
    case 2:
      return (220, 180 &- min(scanline, 120), 48)
    case 3:
      return (180, 64 &+ gradient, 200)
    case 4:
      return (64, 210, 210 &- min(gradient, 160))
    default:
      return (200 &- min(scanline, 120), 88, 96 &+ gradient)
    }
  }
}
