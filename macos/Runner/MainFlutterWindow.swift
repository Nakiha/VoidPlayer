import Cocoa
import CoreVideo
import FlutterMacOS

private struct MacOSDecodedFirstFrame {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
  let bgra: Data
}

private enum MacOSNativePlayerError: Error, CustomStringConvertible {
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

private final class MacOSNativePlayerSession {
  private let handle: OpaquePointer

  init?() {
    guard let handle = VPMacOSNativePlayerCreate() else {
      return nil
    }
    self.handle = handle
  }

  deinit {
    VPMacOSNativePlayerDestroy(handle)
  }

  func open(path: String) throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = path.withCString { pathPointer in
      VPMacOSNativePlayerOpen(handle, pathPointer, &error, error.count)
    }
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player open failed with code \(ret)" : message
      )
    }
  }

  func close() {
    VPMacOSNativePlayerClose(handle)
  }

  func play() {
    VPMacOSNativePlayerPlay(handle)
  }

  func pause() {
    VPMacOSNativePlayerPause(handle)
  }

  func setSpeed(_ speed: Double) {
    VPMacOSNativePlayerSetSpeed(handle, speed)
  }

  func setLoopRange(enabled: Bool, startUs: Int, endUs: Int) {
    VPMacOSNativePlayerSetLoopRange(
      handle,
      enabled ? 1 : 0,
      Int64(startUs),
      Int64(endUs)
    )
  }

  func setAudibleTrack(_ fileId: Int) {
    VPMacOSNativePlayerSetAudibleTrack(handle, Int32(fileId))
  }

  func seek(_ ptsUs: Int) {
    VPMacOSNativePlayerSeek(handle, Int64(ptsUs))
  }

  func currentPtsUs() -> Int {
    Int(VPMacOSNativePlayerCurrentPtsUs(handle))
  }

  func durationUs() -> Int {
    Int(VPMacOSNativePlayerDurationUs(handle))
  }

  func width() -> Int {
    Int(VPMacOSNativePlayerWidth(handle))
  }

  func height() -> Int {
    Int(VPMacOSNativePlayerHeight(handle))
  }

  func isPlaying() -> Bool {
    VPMacOSNativePlayerIsPlaying(handle) != 0
  }

  func hasAudio() -> Bool {
    VPMacOSNativePlayerHasAudio(handle) != 0
  }

  func audioSampleRate() -> Int {
    Int(VPMacOSNativePlayerAudioSampleRate(handle))
  }

  func audioChannels() -> Int {
    Int(VPMacOSNativePlayerAudioChannels(handle))
  }

  func activeAudioTrack() -> Int {
    Int(VPMacOSNativePlayerActiveAudioTrack(handle))
  }

  func copyCurrentFrame(waitTimeoutMs: Int = 0) throws -> MacOSDecodedFirstFrame {
    let deadline = Date().addingTimeInterval(Double(waitTimeoutMs) / 1000.0)
    var lastError = ""

    repeat {
      var frame = VPMacOSNativeFrame()
      var error = [CChar](repeating: 0, count: 1024)
      let ret = VPMacOSNativePlayerCopyCurrentFrameBGRA(
        handle,
        &frame,
        &error,
        error.count
      )
      if ret == 0 {
        defer {
          VPMacOSNativeFrameFree(&frame)
        }
        guard frame.width > 0,
              frame.height > 0,
              let bgra = frame.bgra,
              frame.bgra_size > 0 else {
          throw MacOSNativePlayerError.invalidPayload
        }

        return MacOSDecodedFirstFrame(
          width: Int(frame.width),
          height: Int(frame.height),
          durationUs: Int(frame.duration_us),
          ptsUs: Int(frame.pts_us),
          dtsUs: Int(frame.dts_us),
          bgra: Data(bytes: bgra, count: Int(frame.bgra_size))
        )
      }
      let message = String(cString: error)
      lastError = message.isEmpty ? "copyCurrentFrame failed with code \(ret)" : message
      Thread.sleep(forTimeInterval: 0.01)
    } while Date() < deadline

    throw MacOSNativePlayerError.failed(
      lastError.isEmpty ? "timed out waiting for a decoded frame" : lastError
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
  private let playbackQueue = DispatchQueue(label: "dev.nakiha.voidplayer.macos.native-playback")
  private var texture: MacOSSyntheticTexture?
  private var textureId: Int64?
  private var tracks: [[String: Any]] = []
  private var layout: [String: Any] = MacOSVideoRendererStub.defaultLayout()
  private var currentPtsUs = 0
  private var currentDurationUs = 0
  private var lastPresentedPtsUs: Int?
  private var lastPresentedDtsUs: Int?
  private var isPlaying = false
  private var backendName = "synthetic-texture"
  private var nativePlayer: MacOSNativePlayerSession?
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
    case "initLogging", "setViewportBackgroundColor", "setTrackOffset":
      result(nil)
    case "setLoopRange":
      nativePlayer?.setLoopRange(
        enabled: boolArg(call.arguments, "enabled") ?? false,
        startUs: intArg(call.arguments, "startUs") ?? 0,
        endUs: intArg(call.arguments, "endUs") ?? 0
      )
      result(nil)
    case "setAudibleTrack":
      let fileId = intArg(call.arguments, "fileId") ?? -1
      nativePlayer?.setAudibleTrack(fileId)
      result(nil)
    case "setSpeed":
      playbackSpeed = max(0.01, doubleArg(call.arguments, "speed") ?? 1.0)
      nativePlayer?.setSpeed(playbackSpeed)
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
      nativePlayer?.play()
      startNativePlaybackTimer()
      result(nil)
    case "pause":
      isPlaying = false
      nativePlayer?.pause()
      stopNativePlaybackTimer()
      result(nil)
    case "seek":
      let targetPtsUs = intArg(call.arguments, "ptsUs") ?? 0
      let resumeAfterSeek = nativePlayer?.isPlaying() ?? isPlaying
      if let error = seekAndRefresh(
        targetPtsUs: targetPtsUs,
        resumeAfterSeek: resumeAfterSeek
      ) {
        result(error)
        return
      }
      result(nil)
    case "stepForward":
      if let error = stepAndRefresh(deltaUs: 33_333) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      if let error = stepAndRefresh(deltaUs: -33_333) {
        result(error)
        return
      }
      result(nil)
    case "currentPts":
      currentPtsUs = nativePlayer?.currentPtsUs() ?? currentPtsUs
      result(currentPtsUs)
    case "duration":
      result(tracks.isEmpty ? 0 : currentDurationUs)
    case "currentPresentedFrame":
      result(
        textureId == nil
          ? nil
          : [
              "ptsUs": lastPresentedPtsUs ?? currentPtsUs,
              "dtsUs": lastPresentedDtsUs ?? lastPresentedPtsUs ?? currentPtsUs,
            ]
      )
    case "isPlaying":
      result(nativePlayer?.isPlaying() ?? isPlaying)
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
        "available": nativePlayer != nil,
        "reason": nativePlayer == nil
          ? "Synthetic macOS texture is active"
          : "macOS shared native DecodeThread facade is active",
        "textureId": textureId ?? -1,
        "trackCount": tracks.count,
        "audioAvailable": nativePlayer?.hasAudio() ?? false,
        "audioSampleRate": nativePlayer?.audioSampleRate() ?? 0,
        "audioChannels": nativePlayer?.audioChannels() ?? 0,
        "activeAudioTrack": nativePlayer?.activeAudioTrack() ?? -1,
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
    var initialPresentedPtsUs = 0
    var initialPresentedDtsUs = 0
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
      nativePlayer = nil
    } else {
      do {
        guard let session = MacOSNativePlayerSession() else {
          throw MacOSNativePlayerError.failed("failed to allocate macOS native player")
        }
        try session.open(path: firstPath)
        let decoded = try session.copyCurrentFrame(waitTimeoutMs: 3_000)
        nextTexture = MacOSSyntheticTexture(decoded: decoded)
        initialPresentedPtsUs = decoded.ptsUs
        initialPresentedDtsUs = normalizedDtsUs(decoded)
        trackWidth = session.width() > 0 ? session.width() : decoded.width
        trackHeight = session.height() > 0 ? session.height() : decoded.height
        trackDurationUs = session.durationUs() > 0 ? session.durationUs() : Self.syntheticDurationUs
        trackFormatName = "macos-native-player"
        trackCodecName = "ffmpeg"
        trackCodecLongName = "macOS shared native DecodeThread facade"
        trackDecoderName = "decode_thread_software"
        backendName = "macos-native-player"
        nativePlayer = session
      } catch {
        return FlutterError(
          code: "DECODE_FAILED",
          message: "Failed to open macOS native player",
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
    currentPtsUs = initialPresentedPtsUs
    lastPresentedPtsUs = initialPresentedPtsUs
    lastPresentedDtsUs = initialPresentedDtsUs
    isPlaying = false
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": tracks,
    ]
  }

  private func destroyPlayer() {
    stopNativePlaybackTimer()
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    tracks.removeAll()
    currentPtsUs = 0
    currentDurationUs = 0
    lastPresentedPtsUs = nil
    lastPresentedDtsUs = nil
    isPlaying = false
    backendName = "synthetic-texture"
    nativePlayer?.close()
    nativePlayer = nil
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
      codecName: backendName == "macos-native-player" ? "ffmpeg" : "macos_synthetic",
      codecLongName: backendName == "macos-native-player"
        ? "macOS shared native DecodeThread facade"
        : "macOS Synthetic FlutterTexture",
      decoderName: backendName == "macos-native-player"
        ? "decode_thread_software"
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
    guard backendName == "macos-native-player",
          let nativePlayer,
          texture != nil else {
      return nil
    }

    do {
      nativePlayer.seek(targetPtsUs)
      let decoded = try nativePlayer.copyCurrentFrame(waitTimeoutMs: 3_000)
      publishDecodedFrame(decoded)
      return nil
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      )
    }
  }

  private func seekAndRefresh(targetPtsUs: Int, resumeAfterSeek: Bool) -> FlutterError? {
    stopNativePlaybackTimer()
    nativePlayer?.pause()
    isPlaying = false
    currentPtsUs = max(0, min(activeDurationUs(), targetPtsUs))
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
      return error
    }
    markFrameAvailable()
    if resumeAfterSeek {
      isPlaying = textureId != nil
      nativePlayer?.play()
      startNativePlaybackTimer()
    }
    return nil
  }

  private func stepAndRefresh(deltaUs: Int) -> FlutterError? {
    stopNativePlaybackTimer()
    nativePlayer?.pause()
    isPlaying = false
    currentPtsUs = nativePlayer?.currentPtsUs() ?? currentPtsUs
    currentPtsUs = max(0, min(activeDurationUs(), currentPtsUs + deltaUs))
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: currentPtsUs) {
      return error
    }
    markFrameAvailable()
    return nil
  }

  private func publishDecodedFrame(_ decoded: MacOSDecodedFirstFrame) {
    currentPtsUs = decoded.ptsUs
    lastPresentedPtsUs = decoded.ptsUs
    lastPresentedDtsUs = normalizedDtsUs(decoded)
    texture?.update(decoded: decoded)
  }

  private func normalizedDtsUs(_ decoded: MacOSDecodedFirstFrame) -> Int {
    decoded.dtsUs == Int.min ? decoded.ptsUs : decoded.dtsUs
  }

  private func startNativePlaybackTimer() {
    stopNativePlaybackTimer()
    guard backendName == "macos-native-player",
          nativePlayer != nil,
          textureId != nil else {
      return
    }

    let generation = playbackGeneration + 1
    playbackGeneration = generation
    let timer = DispatchSource.makeTimerSource(queue: playbackQueue)
    timer.schedule(deadline: .now(), repeating: .milliseconds(33))
    timer.setEventHandler { [weak self] in
      self?.copyNativePlaybackFrame(generation: generation)
    }
    playbackTimer = timer
    timer.resume()
  }

  private func stopNativePlaybackTimer() {
    playbackGeneration += 1
    playbackTimer?.setEventHandler {}
    playbackTimer?.cancel()
    playbackTimer = nil
  }

  private func copyNativePlaybackFrame(generation: Int) {
    do {
      guard let nativePlayer else { return }
      let decoded = try nativePlayer.copyCurrentFrame(waitTimeoutMs: 100)
      DispatchQueue.main.async { [weak self] in
        guard let self,
              self.playbackGeneration == generation,
              self.isPlaying,
              self.backendName == "macos-native-player" else {
          return
        }
        self.publishDecodedFrame(decoded)
        self.markFrameAvailable()
        if decoded.ptsUs >= self.activeDurationUs() {
          self.isPlaying = false
          self.nativePlayer?.pause()
          self.stopNativePlaybackTimer()
        }
      }
    } catch {
      DispatchQueue.main.async { [weak self] in
        guard let self, self.playbackGeneration == generation else { return }
        NSLog("VoidPlayer macOS native playback frame copy failed: \(error)")
        self.isPlaying = false
        self.nativePlayer?.pause()
        self.stopNativePlaybackTimer()
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
