import Cocoa
import FlutterMacOS

private func macOSNativeFrameAvailable(_ userData: UnsafeMutableRawPointer?) {
  guard let userData else { return }
  let renderer = Unmanaged<MacOSVideoRendererBridge>.fromOpaque(userData).takeUnretainedValue()
  renderer.scheduleNativeFrameCopyFromCallback()
}

class MainFlutterWindow: NSWindow {
  override func close() {
    MacOSVideoRendererBridge.destroyActivePlayerForWindowClose()
    super.close()
  }

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    MacOSVideoRendererBridge.register(with: flutterViewController.engine)
    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

private final class MacOSVideoRendererBridge: NSObject, FlutterStreamHandler {
  private static let channelName = "video_renderer"
  private static let eventsChannelName = "video_renderer/events"
  private static let syntheticDurationUs = 10_000_000
  private static weak var activeInstance: MacOSVideoRendererBridge?

  private let textureRegistry: FlutterTextureRegistry
  private let playbackQueue = DispatchQueue(label: "dev.nakiha.voidplayer.macos.native-playback")
  private var methodChannel: FlutterMethodChannel?
  private var eventChannel: FlutterEventChannel?
  private var texture: MacOSFlutterTextureBridge?
  private var textureId: Int64?
  private var eventSink: FlutterEventSink?
  private var nativeEventListenCount = 0
  private var nativeEventEmitCount = 0
  private var nativeEventDropNoSinkCount = 0
  private var nativeEventSequence = 0
  private var tracks: [[String: Any]] = []
  private var layout: [String: Any] = MacOSVideoTrackPayload.defaultLayout()
  private var currentDurationUs = 0
  private var isPlaying = false
  private var backendName = "synthetic-texture"
  private var nativePlayer: MacOSNativePlayerSession?
  private var playbackSpeed = 1.0
  private var nativeFrameCallbackRegistered = false
  private var nativePresentationTargetInstalled = false
  private let presentationState = MacOSFramePresentationState()

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
    super.init()
  }

  static func register(with engine: FlutterEngine) {
    configureNativeEnvironment()
    let bridge = MacOSVideoRendererBridge(textureRegistry: engine)
    activeInstance = bridge
    let messenger = engine.binaryMessenger
    let channel = FlutterMethodChannel(name: channelName, binaryMessenger: messenger)
    channel.setMethodCallHandler(bridge.handle)
    bridge.methodChannel = channel

    let events = FlutterEventChannel(name: eventsChannelName, binaryMessenger: messenger)
    events.setStreamHandler(bridge)
    bridge.eventChannel = events
  }

  private static func configureNativeEnvironment() {
  }

  static func destroyActivePlayerForWindowClose() {
    activeInstance?.destroyPlayerForWindowClose()
  }

  private func configureNativeLogging(arguments: Any?) {
    guard let args = arguments as? [String: Any],
          let logsDir = args["logsDir"] as? String,
          !logsDir.isEmpty else {
      return
    }
    logsDir.withCString { logsDirPointer in
      VPMacOSInstallCrashHandler(logsDirPointer)
    }
  }

  private func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "initLogging":
      configureNativeLogging(arguments: call.arguments)
      result(nil)
    case "setViewportBackgroundColor":
      result(nil)
    case "setTrackOffset":
      let fileId = MacOSFlutterArguments.intArg(call.arguments, "fileId") ?? -1
      let offsetUs = MacOSFlutterArguments.intArg(call.arguments, "offsetUs") ?? 0
      nativePlayer?.setTrackOffset(fileId: fileId, offsetUs: offsetUs)
      result(nil)
    case "setLoopRange":
      nativePlayer?.setLoopRange(
        enabled: MacOSFlutterArguments.boolArg(call.arguments, "enabled") ?? false,
        startUs: MacOSFlutterArguments.intArg(call.arguments, "startUs") ?? 0,
        endUs: MacOSFlutterArguments.intArg(call.arguments, "endUs") ?? 0
      )
      result(nil)
    case "setAudibleTrack":
      let fileId = MacOSFlutterArguments.intArg(call.arguments, "fileId") ?? -1
      nativePlayer?.setAudibleTrack(fileId)
      result(nil)
    case "setSpeed":
      playbackSpeed = max(0.01, MacOSFlutterArguments.doubleArg(call.arguments, "speed") ?? 1.0)
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
      startNativeFramePump()
      result(nil)
    case "pause":
      isPlaying = false
      nativePlayer?.pause()
      stopNativeFramePump()
      result(nil)
    case "seek":
      let targetPtsUs = MacOSFlutterArguments.intArg(call.arguments, "ptsUs") ?? 0
      let requestId = MacOSFlutterArguments.intArg(call.arguments, "requestId")
      let resumeAfterSeek = nativePlayer?.isPlaying() ?? isPlaying
      if let error = seekAndRefresh(
        targetPtsUs: targetPtsUs,
        requestId: requestId,
        resumeAfterSeek: resumeAfterSeek
      ) {
        result(error)
        return
      }
      result(nil)
    case "stepForward":
      if let error = stepAndRefresh(forward: true) {
        result(error)
        return
      }
      result(nil)
    case "stepBackward":
      if let error = stepAndRefresh(forward: false) {
        result(error)
        return
      }
      result(nil)
    case "currentPts":
      presentationState.setCurrentPts(nativePlayer?.currentPtsUs() ?? presentationState.currentPtsUs)
      result(presentationState.currentPtsUs)
    case "duration":
      result(tracks.isEmpty ? 0 : currentDurationUs)
    case "currentPresentedFrame":
      result(
        textureId == nil
          ? nil
          : presentationState.currentPresentedFrameMap()
      )
    case "isPlaying":
      result(nativePlayer?.isPlaying() ?? isPlaying)
    case "getLayout":
      result(layout)
    case "applyLayout":
      if let nextLayout = call.arguments as? [String: Any] {
        nativePlayer?.applyLayout(
          mode: MacOSFlutterArguments.intValue(nextLayout["mode"]) ?? 0,
          splitPos: MacOSFlutterArguments.doubleValue(nextLayout["splitPos"]) ?? 0.5,
          zoomRatio: MacOSFlutterArguments.doubleValue(nextLayout["zoomRatio"]) ?? 1.0,
          viewOffsetX: MacOSFlutterArguments.doubleValue(nextLayout["viewOffsetX"]) ?? 0.0,
          viewOffsetY: MacOSFlutterArguments.doubleValue(nextLayout["viewOffsetY"]) ?? 0.0,
          pixelSizeMode: MacOSFlutterArguments.intValue(nextLayout["pixelSizeMode"]) ?? 0,
          order: MacOSFlutterArguments.intListValue(nextLayout["order"])
        )
        layout = nativePlayer?.layoutSnapshotMap() ?? nextLayout
        refreshCurrentFrameAfterLayoutChange()
      }
      result(nil)
    case "getTracks":
      result(tracks)
    case "pickFiles":
      pickFiles(arguments: call.arguments, result: result)
    case "getDiagnostics":
      let textureStats = texture?.diagnostics()
      let textureDimensions = texture?.dimensions()
      let nativeLayoutSnapshot = nativePlayer?.layoutSnapshotMap()
      let schedulerStats = nativePlayer?.presentationSchedulerStats()
      let perfStats = nativePlayer?.performanceStats()
      var diagnostics: [String: Any] = [
        "platform": "macos",
        "backend": backendName,
        "presentationAdapter": String(cString: VPMacOSNativePresentationAdapterName()),
        "presentationAdapterKind": presentationAdapterKind(),
        "presentationScheduler": String(cString: VPMacOSNativePresentationSchedulerName()),
        "presentationBackend": presentationBackendName(),
        "rendererOwnedPresentationActive": nativePlayer?.rendererOwnedPresentationActive() ?? false,
        "hardwareDecodeProvider": String(cString: VPMacOSNativeHardwareDecodeProviderName()),
        "hardwareDecodeAvailable": VPMacOSNativeHardwareDecodeAvailable() != 0,
        "hardwareDecodeActive": nativePlayer?.hardwareDecodeActive() ?? false,
        "hardwareDecodeDownloadsToCpu": nativePlayer?.hardwareDecodeDownloadsToCpu() ?? false,
        "decodeMode": nativePlayer?.decodeModeName() ?? "none",
        "softwareFallbackActive": nativePlayer?.hardwareDecodeActive() != true,
        "available": nativePlayer != nil,
        "reason": nativePlayer == nil
          ? "Synthetic macOS texture bridge is active"
          : presentationReason(),
        "textureId": textureId ?? -1,
        "textureWidth": textureDimensions?.width ?? 0,
        "textureHeight": textureDimensions?.height ?? 0,
        "trackCount": tracks.count,
        "audioAvailable": nativePlayer?.hasAudio() ?? false,
        "audioSampleRate": nativePlayer?.audioSampleRate() ?? 0,
        "audioChannels": nativePlayer?.audioChannels() ?? 0,
        "activeAudioTrack": nativePlayer?.activeAudioTrack() ?? -1,
        "primaryTrackOffsetUs": nativePlayer?.trackOffsetUs(fileId: 0) ?? 0,
        "secondaryTrackOffsetUs": nativePlayer?.trackOffsetUs(fileId: 1) ?? 0,
        "presentationSchedulerTickCount": schedulerStats?["tickCount"] ?? 0,
        "presentationSchedulerPresentableTickCount": schedulerStats?["presentableTickCount"] ?? 0,
        "presentationSchedulerFrameNotificationCount": schedulerStats?["frameNotificationCount"] ?? 0,
        "presentationSchedulerLastSelectedPtsUs": schedulerStats?["lastSelectedPtsUs"] ?? -1,
        "presentationSchedulerLastPresentFrameCount": schedulerStats?["lastPresentFrameCount"] ?? 0,
        "presentationSchedulerCachedDecisionAvailable": schedulerStats?["cachedPresentDecisionAvailable"] ?? false,
        "presentationSchedulerDeadlineSleepCount": schedulerStats?["deadlineSleepCount"] ?? 0,
        "presentationSchedulerLastDeadlineSleepUs": schedulerStats?["lastDeadlineSleepUs"] ?? 0,
        "nativeLayoutMode": nativeLayoutSnapshot?["mode"] ?? -1,
        "nativeLayoutZoomRatio": nativeLayoutSnapshot?["zoomRatio"] ?? 0.0,
        "nativeLayoutPixelSizeMode": nativeLayoutSnapshot?["pixelSizeMode"] ?? -1,
        "pixelBufferRebuildCount": textureStats?.rebuildCount ?? 0,
        "pixelBufferReuseCount": textureStats?.reuseCount ?? 0,
        "pixelBufferMetalUploadCount": textureStats?.metalUploadCount ?? 0,
        "pixelBufferMetalYuvUploadCount":
          perfStats?["rendererOwnedDirectYuvUploadCount"] ?? 0,
        "pixelBufferMetalCVPixelBufferUploadCount":
          perfStats?["rendererOwnedCVPixelBufferUploadCount"] ?? 0,
        "pixelBufferMetalUploadFailureCount": textureStats?.metalUploadFailureCount ?? 0,
        "presentationUploadMode": MacOSPresentationDiagnostics.uploadMode(
          perfStats: perfStats,
          targetReady: textureStats?.metalTextureValid ?? false,
          targetInstalled: nativePresentationTargetInstalled,
          textureRegistered: textureId != nil
        ),
        "presentationPackageUploadCount":
          perfStats?["rendererOwnedPresentPackageUploadCount"] ?? 0,
        "presentationPackageCopyUs": perfStats?["rendererOwnedPresentPackageCopyUs"] ?? 0,
        "presentationPackageGpuWaitUs":
          perfStats?["rendererOwnedPresentPackageGpuWaitUs"] ?? 0,
        "presentationPackageTotalUs":
          perfStats?["rendererOwnedPresentPackageTotalUs"] ?? 0,
        "presentationPackageStorage":
          perfStats?["rendererOwnedPresentPackageStorage"] ?? "unavailable",
        "metalAvailable": textureStats?.metalAvailable ?? false,
        "metalTextureCacheAvailable": textureStats?.metalTextureCacheAvailable ?? false,
        "metalTextureValid": textureStats?.metalTextureValid ?? false,
        "metalTextureCreationCount": textureStats?.metalTextureCreationCount ?? 0,
        "metalTextureFailureCount": textureStats?.metalTextureFailureCount ?? 0,
        "metalTextureLastError": textureStats?.metalTextureLastError ?? "",
        "nativeEventListenCount": nativeEventListenCount,
        "nativeEventEmitCount": nativeEventEmitCount,
        "nativeEventDropNoSinkCount": nativeEventDropNoSinkCount,
        "nativePresentationTargetInstalled": nativePresentationTargetInstalled,
        "nativeRendererOwnedUploadCount": nativePlayer?.rendererOwnedPresentationUploadCount() ?? 0,
        "nativeRendererOwnedUploadFailureCount": nativePlayer?.rendererOwnedPresentationFailureCount() ?? 0,
        "nativeRendererOwnedUploadFps": perfStats?["rendererOwnedUploadFps"] ?? 0.0,
        "nativeRendererOwnedUploadFpsX1000": perfStats?["rendererOwnedUploadFpsX1000"] ?? 0,
        "nativeRendererOwnedUploadElapsedMs": perfStats?["rendererOwnedUploadElapsedMs"] ?? 0,
        "nativeDecodeFrameCount": perfStats?["decodeFrameCount"] ?? 0,
        "nativeDecodeDroppedCount": perfStats?["decodeDroppedCount"] ?? 0,
        "nativeDecodeElapsedMs": perfStats?["decodeElapsedMs"] ?? 0,
        "nativeDecodeFps": perfStats?["decodeFps"] ?? 0.0,
        "nativeDecodeFpsX1000": perfStats?["decodeFpsX1000"] ?? 0,
        "nativeDecodeAvgMs": perfStats?["decodeAvgMs"] ?? 0.0,
        "nativeDecodeMaxMs": perfStats?["decodeMaxMs"] ?? 0.0,
        "presentationFallbackReason": MacOSPresentationDiagnostics.fallbackReason(
          player: nativePlayer,
          targetInstalled: nativePresentationTargetInstalled,
          perfStats: perfStats
        ),
      ]
      presentationState.diagnosticMap().forEach { diagnostics[$0.key] = $0.value }
      result(diagnostics)
    case "captureViewport":
      result(captureViewport())
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func createPlayer(arguments: Any?) -> Any {
    destroyPlayer()

    let paths = MacOSFlutterArguments.stringListArg(arguments, "videoPaths")
    let requestedWidth = max(16, MacOSFlutterArguments.intArg(arguments, "width") ?? 1920)
    let requestedHeight = max(16, MacOSFlutterArguments.intArg(arguments, "height") ?? 1080)
    let firstPath = paths.first ?? "macos-synthetic://color-bars"

    let nextTexture: MacOSFlutterTextureBridge
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
      nextTexture = MacOSFlutterTextureBridge(
        width: requestedWidth,
        height: requestedHeight
      )
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
        nextTexture = MacOSFlutterTextureBridge(
          nativeWidth: requestedWidth,
          nativeHeight: requestedHeight
        )
        let firstFrame = try nextTexture.updateFromNativePlayer(
          session,
          maxTrackSlots: 1,
          waitTimeoutMs: 3_000
        )
        nativePresentationTargetInstalled = session.rendererOwnedPresentationActive()
        initialPresentedPtsUs = firstFrame.ptsUs
        initialPresentedDtsUs = MacOSFramePresentationState.normalizedDtsUs(firstFrame)
        trackWidth = session.width() > 0 ? session.width() : firstFrame.width
        trackHeight = session.height() > 0 ? session.height() : firstFrame.height
        trackDurationUs = session.durationUs() > 0 ? session.durationUs() : Self.syntheticDurationUs
        trackFormatName = "macos-native-player"
        trackCodecName = "ffmpeg"
        trackCodecLongName = "macOS shared native DecodeThread facade"
        trackDecoderName = session.decoderName()
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
    if backendName == "macos-native-player" {
      tracks = [
        MacOSVideoTrackPayload.track(
          fileId: 0,
          slot: 0,
          path: firstPath,
          width: trackWidth,
          height: trackHeight,
          durationUs: trackDurationUs,
          formatName: trackFormatName,
          codecName: trackCodecName,
          codecLongName: trackCodecLongName,
          decoderName: trackDecoderName
        )
      ]
      if paths.count > 1 {
        for path in paths.dropFirst() {
          let fileId = (tracks.map { MacOSFlutterArguments.intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
          do {
            guard let session = nativePlayer else {
              throw MacOSNativePlayerError.failed("macOS native player is unavailable")
            }
            let metadata = try session.addTrack(path: path, fileId: fileId)
            tracks.append(nativeTrackMap(path: path, metadata: metadata))
          } catch {
            destroyPlayer()
            return FlutterError(
              code: "DECODE_FAILED",
              message: "Failed to add macOS native track",
              details: "\(error)"
            )
          }
        }
      }
    } else {
      tracks = paths.enumerated().map { index, path in
        MacOSVideoTrackPayload.track(
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
    }
    currentDurationUs = tracks
      .map { MacOSFlutterArguments.intValue($0["durationUs"]) ?? trackDurationUs }
      .max() ?? trackDurationUs
    presentationState.seedPresentedFrame(
      ptsUs: initialPresentedPtsUs,
      dtsUs: initialPresentedDtsUs,
      durationUs: trackDurationUs
    )
    isPlaying = false
    markFrameAvailable()

    return [
      "textureId": registeredTextureId,
      "tracks": tracks,
    ]
  }

  private func nativeTrackMap(path: String, metadata: MacOSNativeTrackMetadata) -> [String: Any] {
    MacOSVideoTrackPayload.track(
      fileId: metadata.fileId,
      slot: metadata.slot,
      path: path,
      width: metadata.width,
      height: metadata.height,
      durationUs: metadata.durationUs,
      formatName: "macos-native-player",
      codecName: "ffmpeg",
      codecLongName: "macOS shared native DecodeThread facade",
      decoderName: nativePlayer?.decoderName() ?? "decode_thread_software"
    )
  }

  private func destroyPlayer() {
    stopNativeFramePump(clearPresentationTarget: true)
    if let id = textureId {
      textureRegistry.unregisterTexture(id)
    }
    texture = nil
    textureId = nil
    tracks.removeAll()
    presentationState.resetAll()
    currentDurationUs = 0
    isPlaying = false
    backendName = "synthetic-texture"
    nativePresentationTargetInstalled = false
    nativePlayer?.close()
    nativePlayer = nil
  }

  private func destroyPlayerForWindowClose() {
    if textureId == nil && nativePlayer == nil {
      return
    }
    NSLog("VoidPlayer macOS native player teardown before window close")
    destroyPlayer()
  }

  private func addTrack(arguments: Any?) -> Any {
    guard textureId != nil else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "createPlayer must be called before addTrack",
        details: nil
      )
    }

    let fileId = (tracks.map { MacOSFlutterArguments.intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
    let slot = tracks.count
    let path = MacOSFlutterArguments.stringArg(arguments, "path") ?? "macos-synthetic-\(fileId)"
    if backendName == "macos-native-player" {
      do {
        guard let session = nativePlayer else {
          throw MacOSNativePlayerError.failed("macOS native player is unavailable")
        }
        let metadata = try session.addTrack(path: path, fileId: fileId)
        let track = nativeTrackMap(path: path, metadata: metadata)
        tracks.append(track)
        currentDurationUs = max(currentDurationUs, metadata.durationUs)
        refreshCurrentFrameAfterLayoutChange()
        return track
      } catch {
        return FlutterError(
          code: "DECODE_FAILED",
          message: "Failed to add macOS native track",
          details: "\(error)"
        )
      }
    }

    let size = texture?.dimensions() ?? (width: 1920, height: 1080)
    let track = MacOSVideoTrackPayload.track(
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
        ? nativePlayer?.decoderName() ?? "decode_thread_software"
        : "synthetic"
    )
    tracks.append(track)
    markFrameAvailable()
    return track
  }

  private func removeTrack(arguments: Any?) {
    guard let fileId = MacOSFlutterArguments.intArg(arguments, "fileId") else { return }
    if backendName == "macos-native-player" {
      if fileId == 0 {
        destroyPlayer()
        return
      }
      nativePlayer?.removeTrack(fileId: fileId)
    }
    tracks.removeAll { MacOSFlutterArguments.intValue($0["fileId"]) == fileId }
    if backendName != "macos-native-player" {
      tracks = tracks.enumerated().map { index, track in
        var next = track
        next["slot"] = index
        return next
      }
    }
    currentDurationUs = tracks
      .map { MacOSFlutterArguments.intValue($0["durationUs"]) ?? 0 }
      .max() ?? 0
    if tracks.isEmpty {
      destroyPlayer()
    } else {
      refreshCurrentFrameAfterLayoutChange()
    }
  }

  private func resize(arguments: Any?) {
    let width = MacOSFlutterArguments.intArg(arguments, "width")
    let height = MacOSFlutterArguments.intArg(arguments, "height")
    if let width, let height {
      let nextWidth = max(16, width)
      let nextHeight = max(16, height)
      let currentDimensions = texture?.dimensions()
      let willChange = currentDimensions?.width != nextWidth ||
        currentDimensions?.height != nextHeight
      if backendName == "macos-native-player", willChange {
        nativePlayer?.clearMetalPresentationTarget()
      }
      _ = texture?.resize(width: nextWidth, height: nextHeight) ?? false
      if backendName == "macos-native-player" {
        refreshCurrentFrameAfterLayoutChange()
        if isPlaying,
           let nativePlayer,
           let texture,
           texture.installNativePresentationTarget(
             nativePlayer,
             maxTrackSlots: activeTrackSlotCapacity()
           ) {
          // The next scheduler callback will publish the next renderer-owned frame.
        }
      }
    }
    markFrameAvailable()
  }

  private func captureViewport() -> Any {
    guard let texture else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "No macOS Flutter texture bridge is registered",
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

  private func activeTrackSlotCapacity() -> Int {
    let maxSlot = tracks
      .compactMap { MacOSFlutterArguments.intValue($0["slot"]) }
      .max() ?? 0
    return max(1, min(4, maxSlot + 1))
  }

  private func refreshDecodedFrameIfNeeded(targetPtsUs: Int) -> FlutterError? {
    guard backendName == "macos-native-player",
          let nativePlayer,
          texture != nil else {
      return nil
    }

    do {
      nativePlayer.seek(targetPtsUs)
      guard let texture else {
        throw MacOSNativePlayerError.invalidPayload
      }
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity(),
        waitTimeoutMs: 3_000
      )
      nativePresentationTargetInstalled = nativePlayer.rendererOwnedPresentationActive()
      publishFrameInfo(frameInfo)
      return nil
    } catch {
      return FlutterError(
        code: "DECODE_FAILED",
        message: "Failed to decode macOS video frame",
        details: "\(error)"
      )
    }
  }

  private func refreshCurrentFrameAfterLayoutChange() {
    guard backendName == "macos-native-player",
          let nativePlayer,
          let texture else {
      markFrameAvailable()
      return
    }
    do {
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity(),
        waitTimeoutMs: 100
      )
      nativePresentationTargetInstalled = nativePlayer.rendererOwnedPresentationActive()
      publishFrameInfo(frameInfo)
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true {
        presentationState.recordMiss()
      } else {
        NSLog("VoidPlayer macOS native layout refresh failed: \(error)")
      }
    }
    markFrameAvailable()
  }

  private func seekAndRefresh(
    targetPtsUs: Int,
    requestId: Int?,
    resumeAfterSeek: Bool
  ) -> FlutterError? {
    stopNativeFramePump()
    nativePlayer?.pause()
    isPlaying = false
    let settledPtsUs = max(0, min(activeDurationUs(), targetPtsUs))
    presentationState.setCurrentPts(settledPtsUs)
    if let error = refreshDecodedFrameIfNeeded(targetPtsUs: settledPtsUs) {
      return error
    }
    markFrameAvailable()
    emitSeekPreviewPresented(requestId: requestId, targetPtsUs: settledPtsUs)
    if resumeAfterSeek {
      isPlaying = textureId != nil
      nativePlayer?.play()
      startNativeFramePump()
    }
    return nil
  }

  private func stepAndRefresh(forward: Bool) -> FlutterError? {
    stopNativeFramePump()
    isPlaying = false
    guard let nativePlayer,
          let texture else {
      return nil
    }
    do {
      if forward {
        try nativePlayer.stepForward()
      } else {
        try nativePlayer.stepBackward()
      }
      let frameInfo = try texture.updateFromNativePlayer(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity(),
        waitTimeoutMs: 3_000
      )
      nativePresentationTargetInstalled = nativePlayer.rendererOwnedPresentationActive()
      publishFrameInfo(frameInfo)
    } catch {
      return FlutterError(
        code: "STEP_FAILED",
        message: "Failed to step macOS native playback",
        details: "\(error)"
      )
    }
    markFrameAvailable()
    return nil
  }

  private func publishFrameInfo(_ info: MacOSNativeFrameInfo) {
    presentationState.recordFrame(info)
  }

  private func emitSeekPreviewPresented(requestId: Int?, targetPtsUs: Int) {
    guard let requestId,
          let ptsUs = presentationState.lastPresentedPtsUs else {
      return
    }
    guard let eventSink else {
      nativeEventDropNoSinkCount += 1
      return
    }
    nativeEventSequence += 1
    nativeEventEmitCount += 1
    let payload: [String: Any] = [
      "schemaVersion": 1,
      "sequence": nativeEventSequence,
      "type": "seekPreviewPresented",
      "timestampUs": Int(Date().timeIntervalSince1970 * 1_000_000),
      "requestId": requestId,
      "trackFileId": 0,
      "ptsUs": ptsUs,
      "dtsUs": presentationState.lastPresentedDtsUs ?? ptsUs,
      "targetPtsUs": targetPtsUs,
    ]
    DispatchQueue.main.async {
      eventSink(payload)
    }
  }

  private func presentationBackendName() -> String {
    guard nativePlayer != nil else {
      return "synthetic-texture"
    }
    if nativePlayer?.rendererOwnedPresentationActive() == true {
      return "native-metal-cvpixelbuffer-target"
    }
    return "native-metal-target-unavailable"
  }

  private func presentationAdapterKind() -> String {
    guard nativePlayer != nil else {
      return "synthetic"
    }
    if nativePlayer?.rendererOwnedPresentationActive() == true {
      return "renderer-owned-metal"
    }
    return "unavailable"
  }

  private func presentationReason() -> String {
    if nativePlayer?.rendererOwnedPresentationActive() == true {
      return "macOS shared native facade is active with renderer-owned Metal presentation"
    }
    return "macOS shared native facade has no active renderer-owned presentation target"
  }

  private func startNativeFramePump() {
    stopNativeFramePump()
    guard backendName == "macos-native-player",
          let nativePlayer,
          textureId != nil else {
      return
    }

    presentationState.resetFrameCounters()
    nativePlayer.resetRendererOwnedPresentationStats()
    nativeFrameCallbackRegistered = true
    nativePresentationTargetInstalled = false
    if let texture {
      nativePresentationTargetInstalled = texture.installNativePresentationTarget(
        nativePlayer,
        maxTrackSlots: activeTrackSlotCapacity()
      )
    }
    nativePlayer.setFrameAvailableCallback(
      macOSNativeFrameAvailable,
      userData: Unmanaged.passUnretained(self).toOpaque()
    )
    guard !nativePresentationTargetInstalled else {
      return
    }
    presentationState.recordError()
    NSLog("VoidPlayer macOS renderer-owned Metal presentation target unavailable")
    isPlaying = false
    nativePlayer.pause()
    stopNativeFramePump()
  }

  private func stopNativeFramePump(clearPresentationTarget: Bool = false) {
    if clearPresentationTarget {
      nativePlayer?.clearMetalPresentationTarget()
      nativePresentationTargetInstalled = false
    }
    if nativeFrameCallbackRegistered {
      nativePlayer?.setFrameAvailableCallback(nil, userData: nil)
      nativeFrameCallbackRegistered = false
    }
  }

  fileprivate func scheduleNativeFrameCopyFromCallback() {
    DispatchQueue.main.async { [weak self] in
      guard let self else { return }
      self.presentationState.recordCallback()
      guard self.isPlaying,
            self.backendName == "macos-native-player" else {
        return
      }
      if self.nativePlayer?.lastRendererOwnedPresentationSucceeded() == true {
        self.presentationState.recordPresentation(rendererOwned: true)
        if let frameInfo = self.nativePlayer?.lastRendererOwnedFrameInfo() {
          self.publishFrameInfo(frameInfo)
        }
        self.markFrameAvailable()
        return
      }
      if self.nativePresentationTargetInstalled {
        return
      }
      self.presentationState.recordError()
      NSLog("VoidPlayer macOS renderer-owned Metal presentation failed")
      self.isPlaying = false
      self.nativePlayer?.pause()
      self.stopNativeFramePump()
    }
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    eventSink = events
    nativeEventListenCount += 1
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    eventSink = nil
    return nil
  }

  private func pickFiles(arguments: Any?, result: @escaping FlutterResult) {
    let allowsMultipleSelection = MacOSFlutterArguments.boolArg(arguments, "allowMultiple") ?? true
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

}
