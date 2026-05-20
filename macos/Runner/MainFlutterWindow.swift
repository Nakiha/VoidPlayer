import Cocoa
import CoreVideo
import FlutterMacOS

private struct MacOSDecodedFirstFrame {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let bgra: Data
}

private enum MacOSFirstFrameDecodeError: Error, CustomStringConvertible {
  case failed(String)
  case invalidPayload

  var description: String {
    switch self {
    case .failed(let message):
      return message
    case .invalidPayload:
      return "decoded first frame had invalid dimensions or pixel data"
    }
  }
}

private enum MacOSFirstFrameDecode {
  static func decode(path: String, targetPtsUs: Int = 0) throws -> MacOSDecodedFirstFrame {
    var frame = VPMacOSDecodedFrame()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = path.withCString { pathPointer in
      VPMacOSDecodeVideoFrameBGRA(
        pathPointer,
        Int64(targetPtsUs),
        &frame,
        &error,
        error.count
      )
    }
    defer {
      VPMacOSDecodedFrameFree(&frame)
    }

    if ret != 0 {
      let message = String(cString: error)
      throw MacOSFirstFrameDecodeError.failed(
        message.isEmpty ? "FFmpeg first-frame decode failed with code \(ret)" : message
      )
    }
    guard frame.width > 0,
          frame.height > 0,
          let bgra = frame.bgra,
          frame.bgra_size > 0 else {
      throw MacOSFirstFrameDecodeError.invalidPayload
    }

    return MacOSDecodedFirstFrame(
      width: Int(frame.width),
      height: Int(frame.height),
      durationUs: Int(frame.duration_us),
      ptsUs: Int(frame.pts_us),
      bgra: Data(bytes: bgra, count: Int(frame.bgra_size))
    )
  }
}

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
  private let playbackQueue = DispatchQueue(label: "dev.nakiha.voidplayer.macos.preview-playback")
  private var texture: MacOSSyntheticTexture?
  private var textureId: Int64?
  private var tracks: [[String: Any]] = []
  private var layout: [String: Any] = MacOSVideoRendererStub.defaultLayout()
  private var currentPtsUs = 0
  private var currentDurationUs = 0
  private var isPlaying = false
  private var backendName = "synthetic-texture"
  private var currentMediaPath: String?
  private var playbackSpeed = 1.0
  private var playbackTimer: DispatchSourceTimer?
  private var playbackGeneration = 0

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
    case "initLogging", "setLoopRange", "setAudibleTrack",
         "setViewportBackgroundColor", "setTrackOffset":
      result(nil)
    case "setSpeed":
      playbackSpeed = max(0.01, doubleArg(call.arguments, "speed") ?? 1.0)
      if isPlaying {
        startPreviewPlaybackTimer()
      }
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
      startPreviewPlaybackTimer()
      result(nil)
    case "pause":
      isPlaying = false
      stopPreviewPlaybackTimer()
      result(nil)
    case "seek":
      let targetPtsUs = intArg(call.arguments, "ptsUs") ?? 0
      stopPreviewPlaybackTimer()
      currentPtsUs = max(0, min(activeDurationUs(), targetPtsUs))
      if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
        result(error)
        return
      }
      markFrameAvailable()
      result(nil)
    case "stepForward":
      stopPreviewPlaybackTimer()
      currentPtsUs = min(activeDurationUs(), currentPtsUs + 33_333)
      if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
        result(error)
        return
      }
      markFrameAvailable()
      result(nil)
    case "stepBackward":
      stopPreviewPlaybackTimer()
      currentPtsUs = max(0, currentPtsUs - 33_333)
      if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
        result(error)
        return
      }
      markFrameAvailable()
      result(nil)
    case "currentPts":
      result(currentPtsUs)
    case "duration":
      result(tracks.isEmpty ? 0 : currentDurationUs)
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
        "backend": backendName,
        "available": false,
        "reason": "macOS native playback is not implemented yet; " +
          "first-frame bridge is active for port validation",
        "textureId": textureId ?? -1,
        "trackCount": tracks.count,
      ])
    case "captureViewport":
      result(captureViewport())
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func createPlayer(arguments: Any?) -> Any {
    destroyPlayer()

    let paths = stringListArg(arguments, "videoPaths")
    let requestedWidth = max(16, intArg(arguments, "width") ?? 1920)
    let requestedHeight = max(16, intArg(arguments, "height") ?? 1080)
    let firstPath = paths.first ?? "macos-synthetic://color-bars"

    let nextTexture: MacOSSyntheticTexture
    let trackWidth: Int
    let trackHeight: Int
    let trackDurationUs: Int
    let trackFormatName: String
    let trackCodecName: String
    let trackCodecLongName: String
    let trackDecoderName: String
    if firstPath.hasPrefix("macos-synthetic://") {
      nextTexture = MacOSSyntheticTexture(width: requestedWidth, height: requestedHeight)
      trackWidth = requestedWidth
      trackHeight = requestedHeight
      trackDurationUs = Self.syntheticDurationUs
      trackFormatName = "synthetic"
      trackCodecName = "macos_synthetic"
      trackCodecLongName = "macOS Synthetic FlutterTexture"
      trackDecoderName = "synthetic"
      backendName = "synthetic-texture"
      currentMediaPath = nil
    } else {
      do {
        let decoded = try MacOSFirstFrameDecode.decode(path: firstPath)
        nextTexture = MacOSSyntheticTexture(decoded: decoded)
        trackWidth = decoded.width
        trackHeight = decoded.height
        trackDurationUs = decoded.durationUs > 0 ? decoded.durationUs : Self.syntheticDurationUs
        trackFormatName = "ffmpeg-first-frame"
        trackCodecName = "ffmpeg"
        trackCodecLongName = "macOS FFmpeg first-frame bridge"
        trackDecoderName = "ffmpeg_software_first_frame"
        backendName = "ffmpeg-first-frame"
        currentMediaPath = firstPath
      } catch {
        return FlutterError(
          code: "DECODE_FAILED",
          message: "Failed to decode first macOS video frame",
          details: "\(error)"
        )
      }
    }

    let registeredTextureId = textureRegistry.register(nextTexture)

    texture = nextTexture
    textureId = registeredTextureId
    currentDurationUs = trackDurationUs
    tracks = paths.enumerated().map { index, path in
      trackMap(
        fileId: index,
        slot: index,
        path: path,
        width: trackWidth,
        height: trackHeight,
        durationUs: trackDurationUs,
        formatName: trackFormatName,
        codecName: trackCodecName,
        codecLongName: trackCodecLongName,
        decoderName: trackDecoderName
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
    stopPreviewPlaybackTimer()
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    tracks.removeAll()
    currentPtsUs = 0
    currentDurationUs = 0
    isPlaying = false
    backendName = "synthetic-texture"
    currentMediaPath = nil
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
      height: size.height,
      durationUs: currentDurationUs,
      formatName: backendName,
      codecName: backendName == "ffmpeg-first-frame" ? "ffmpeg" : "macos_synthetic",
      codecLongName: backendName == "ffmpeg-first-frame"
        ? "macOS FFmpeg first-frame bridge"
        : "macOS Synthetic FlutterTexture",
      decoderName: backendName == "ffmpeg-first-frame"
        ? "ffmpeg_software_first_frame"
        : "synthetic"
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

  private func activeDurationUs() -> Int {
    currentDurationUs > 0 ? currentDurationUs : Self.syntheticDurationUs
  }

  private func refreshDecodedFrameIfNeeded(targetPtsUs: Int) -> FlutterError? {
    guard backendName == "ffmpeg-first-frame",
          let currentMediaPath,
          let texture else {
      return nil
    }

    do {
      let decoded = try MacOSFirstFrameDecode.decode(
        path: currentMediaPath,
        targetPtsUs: targetPtsUs
      )
      texture.update(decoded: decoded)
      return nil
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      )
    }
  }

  private func startPreviewPlaybackTimer() {
    stopPreviewPlaybackTimer()
    guard backendName == "ffmpeg-first-frame",
          let path = currentMediaPath,
          textureId != nil else {
      return
    }

    let generation = playbackGeneration + 1
    playbackGeneration = generation
    let startPtsUs = currentPtsUs
    let durationUs = activeDurationUs()
    let speed = playbackSpeed
    let startNanos = DispatchTime.now().uptimeNanoseconds
    let timer = DispatchSource.makeTimerSource(queue: playbackQueue)
    timer.schedule(deadline: .now() + .milliseconds(120), repeating: .milliseconds(120))
    timer.setEventHandler { [weak self] in
      self?.decodePreviewPlaybackTick(
        path: path,
        startPtsUs: startPtsUs,
        durationUs: durationUs,
        speed: speed,
        generation: generation,
        startNanos: startNanos
      )
    }
    playbackTimer = timer
    timer.resume()
  }

  private func stopPreviewPlaybackTimer() {
    playbackGeneration += 1
    playbackTimer?.setEventHandler {}
    playbackTimer?.cancel()
    playbackTimer = nil
  }

  private func decodePreviewPlaybackTick(
    path: String,
    startPtsUs: Int,
    durationUs: Int,
    speed: Double,
    generation: Int,
    startNanos: UInt64
  ) {
    let elapsedNanos = DispatchTime.now().uptimeNanoseconds - startNanos
    let elapsedUs = Int((Double(elapsedNanos) / 1000.0) * speed)
    let targetPtsUs = min(durationUs, startPtsUs + max(0, elapsedUs))

    do {
      let decoded = try MacOSFirstFrameDecode.decode(path: path, targetPtsUs: targetPtsUs)
      DispatchQueue.main.async { [weak self] in
        guard let self,
              self.playbackGeneration == generation,
              self.isPlaying,
              self.backendName == "ffmpeg-first-frame" else {
          return
        }
        self.currentPtsUs = targetPtsUs
        self.texture?.update(decoded: decoded)
        self.markFrameAvailable()
        if targetPtsUs >= durationUs {
          self.isPlaying = false
          self.stopPreviewPlaybackTimer()
        }
      }
    } catch {
      DispatchQueue.main.async { [weak self] in
        guard let self, self.playbackGeneration == generation else { return }
        NSLog("VoidPlayer macOS preview playback decode failed: \(error)")
        self.isPlaying = false
        self.stopPreviewPlaybackTimer()
      }
    }
  }

  private func trackMap(
    fileId: Int,
    slot: Int,
    path: String,
    width: Int,
    height: Int,
    durationUs: Int,
    formatName: String,
    codecName: String,
    codecLongName: String,
    decoderName: String
  ) -> [String: Any] {
    return [
      "fileId": fileId,
      "slot": slot,
      "path": path,
      "width": width,
      "height": height,
      "durationUs": durationUs,
      "startTimeUs": 0,
      "bitRate": 0,
      "formatName": formatName,
      "codecName": codecName,
      "codecLongName": codecLongName,
      "decoderName": decoderName,
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

  private func doubleArg(_ arguments: Any?, _ key: String) -> Double? {
    guard let map = arguments as? [String: Any] else { return nil }
    if let value = map[key] as? Double {
      return value
    }
    if let value = map[key] as? NSNumber {
      return value.doubleValue
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
  private let syntheticPattern: Bool
  private var decodedBGRA: Data?
  private let hashPrefix: String
  private var pixelBuffer: CVPixelBuffer?

  init(width: Int, height: Int) {
    self.width = width
    self.height = height
    self.syntheticPattern = true
    self.decodedBGRA = nil
    self.hashPrefix = "macos-synthetic"
    super.init()
    rebuildPixelBuffer()
  }

  init(decoded: MacOSDecodedFirstFrame) {
    self.width = decoded.width
    self.height = decoded.height
    self.syntheticPattern = false
    self.decodedBGRA = decoded.bgra
    self.hashPrefix = "macos-first-frame"
    super.init()
    rebuildPixelBuffer()
  }

  func resize(width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    guard syntheticPattern else { return }
    guard width != self.width || height != self.height else { return }
    self.width = width
    self.height = height
    rebuildPixelBufferLocked()
  }

  func update(decoded: MacOSDecodedFirstFrame) {
    lock.lock()
    defer { lock.unlock() }

    guard !syntheticPattern else { return }
    width = decoded.width
    height = decoded.height
    decodedBGRA = decoded.bgra
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
        hash: "\(hashPrefix)-empty"
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

    if let decodedBGRA {
      copyBGRA(decodedBGRA, to: nextBuffer)
    } else {
      fill(buffer: nextBuffer)
    }
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
        hash: "\(hashPrefix)-empty"
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
      hash: String(format: "%@-%dx%d-%016llx", hashPrefix, width, height, hash)
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

  private func copyBGRA(_ data: Data, to buffer: CVPixelBuffer) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let rowBytes = width * 4

    data.withUnsafeBytes { rawSource in
      guard let source = rawSource.baseAddress else { return }
      for y in 0..<height {
        let sourceOffset = y * rowBytes
        guard sourceOffset < rawSource.count else { break }
        let sourceRowBytes = min(rowBytes, rawSource.count - sourceOffset)
        memcpy(
          baseAddress.advanced(by: y * bytesPerRow),
          source.advanced(by: sourceOffset),
          sourceRowBytes
        )
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
