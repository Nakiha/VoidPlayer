import Cocoa
import CoreVideo
import FlutterMacOS

struct MacOSNativeFrameInfo {
  let width: Int
  let height: Int
  let durationUs: Int
  let ptsUs: Int
  let dtsUs: Int
}

struct MacOSNativeTrackMetadata {
  let fileId: Int
  let slot: Int
  let width: Int
  let height: Int
  let durationUs: Int
}

enum MacOSNativePlayerError: Error, CustomStringConvertible {
  case failed(String)
  case invalidPayload
  case transientFrameUnavailable(String)

  var description: String {
    switch self {
    case .failed(let message):
      return message
    case .invalidPayload:
      return "native frame had invalid dimensions or pixel data"
    case .transientFrameUnavailable(let message):
      return message
    }
  }

  var isTransientFrameUnavailable: Bool {
    switch self {
    case .failed(let message):
      return message == "no presentable frame is ready" ||
        message == "no decoded frame is ready" ||
        message == "not all present decision frames are ready" ||
        message == "timed out waiting for a decoded frame" ||
        message == "shared macOS renderer has not presented a frame yet"
    case .invalidPayload:
      return false
    case .transientFrameUnavailable:
      return true
    }
  }
}

final class MacOSNativePlayerSession {
  private let handle: OpaquePointer

  init?() {
    guard let handle = VPMacOSNativePlayerCreate() else {
      return nil
    }
    self.handle = handle
  }

  deinit {
    setFrameAvailableCallback(nil, userData: nil)
    clearMetalPresentationTarget()
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

  func addTrack(path: String, fileId: Int) throws -> MacOSNativeTrackMetadata {
    var info = VPMacOSNativeTrackInfo()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = path.withCString { pathPointer in
      VPMacOSNativePlayerAddTrack(handle, pathPointer, Int32(fileId), &info, &error, error.count)
    }
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player addTrack failed with code \(ret)" : message
      )
    }
    return MacOSNativeTrackMetadata(
      fileId: Int(info.file_id),
      slot: Int(info.slot),
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us)
    )
  }

  func removeTrack(fileId: Int) {
    VPMacOSNativePlayerRemoveTrack(handle, Int32(fileId))
  }

  func close() {
    VPMacOSNativePlayerClose(handle)
  }

  func setFrameAvailableCallback(
    _ callback: VPMacOSFrameAvailableCallback?,
    userData: UnsafeMutableRawPointer?
  ) {
    VPMacOSNativePlayerSetFrameAvailableCallback(handle, callback, userData)
  }

  func setMetalPresentationTarget(
    backend: OpaquePointer,
    pixelBuffer: CVPixelBuffer,
    width: Int,
    height: Int,
    maxTrackSlots: Int
  ) -> Bool {
    VPMacOSNativePlayerSetMetalPresentationTarget(
      handle,
      backend,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque()),
      Int32(width),
      Int32(height),
      Int32(max(1, min(4, maxTrackSlots)))
    ) == 0
  }

  func clearMetalPresentationTarget() {
    VPMacOSNativePlayerClearMetalPresentationTarget(handle)
  }

  func rendererOwnedPresentationActive() -> Bool {
    VPMacOSNativePlayerRendererOwnedPresentationActive(handle) != 0
  }

  func lastRendererOwnedPresentationSucceeded() -> Bool {
    VPMacOSNativePlayerLastRendererOwnedPresentationSucceeded(handle) != 0
  }

  func lastRendererOwnedFrameInfo() -> MacOSNativeFrameInfo? {
    var info = VPMacOSNativeFrameInfo()
    guard VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(handle, &info) == 0 else {
      return nil
    }
    return MacOSNativeFrameInfo(
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us),
      ptsUs: Int(info.pts_us),
      dtsUs: Int(info.dts_us)
    )
  }

  func presentCurrentFrameToMetalTarget() throws -> MacOSNativeFrameInfo {
    var info = VPMacOSNativeFrameInfo()
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(
      handle,
      &info,
      &error,
      error.count
    )
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty
          ? "macOS renderer-owned presentation failed with code \(ret)"
          : message
      )
    }
    guard info.width > 0, info.height > 0 else {
      throw MacOSNativePlayerError.invalidPayload
    }
    return MacOSNativeFrameInfo(
      width: Int(info.width),
      height: Int(info.height),
      durationUs: Int(info.duration_us),
      ptsUs: Int(info.pts_us),
      dtsUs: Int(info.dts_us)
    )
  }

  func resetRendererOwnedPresentationStats() {
    VPMacOSNativePlayerResetRendererOwnedPresentationStats(handle)
  }

  func rendererOwnedPresentationUploadCount() -> Int {
    Int(VPMacOSNativePlayerRendererOwnedPresentationUploadCount(handle))
  }

  func rendererOwnedPresentationFailureCount() -> Int {
    Int(VPMacOSNativePlayerRendererOwnedPresentationFailureCount(handle))
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

  func setTrackOffset(fileId: Int, offsetUs: Int) {
    VPMacOSNativePlayerSetTrackOffset(handle, Int32(fileId), Int64(offsetUs))
  }

  func trackOffsetUs(fileId: Int) -> Int {
    Int(VPMacOSNativePlayerTrackOffsetUs(handle, Int32(fileId)))
  }

  func applyLayout(
    mode: Int,
    splitPos: Double,
    zoomRatio: Double,
    viewOffsetX: Double,
    viewOffsetY: Double,
    pixelSizeMode: Int,
    order: [Int]
  ) {
    let paddedOrder = (order + [0, 0, 0, 0]).prefix(4).map { Int32($0) }
    var state = VPMacOSNativeLayoutState()
    state.mode = Int32(mode)
    state.split_pos = Float(splitPos)
    state.zoom_ratio = Float(zoomRatio)
    state.view_offset_x = Float(viewOffsetX)
    state.view_offset_y = Float(viewOffsetY)
    state.pixel_size_mode = Int32(pixelSizeMode)
    state.order = (paddedOrder[0], paddedOrder[1], paddedOrder[2], paddedOrder[3])
    VPMacOSNativePlayerApplyLayout(handle, &state)
  }

  func layoutSnapshotMap() -> [String: Any]? {
    var state = VPMacOSNativeLayoutState()
    guard VPMacOSNativePlayerCopyLayout(handle, &state) == 0 else {
      return nil
    }
    return [
      "mode": Int(state.mode),
      "splitPos": Double(state.split_pos),
      "zoomRatio": Double(state.zoom_ratio),
      "viewOffsetX": Double(state.view_offset_x),
      "viewOffsetY": Double(state.view_offset_y),
      "pixelSizeMode": Int(state.pixel_size_mode),
      "order": [
        Int(state.order.0),
        Int(state.order.1),
        Int(state.order.2),
        Int(state.order.3),
      ],
    ]
  }

  func seek(_ ptsUs: Int) {
    VPMacOSNativePlayerSeek(handle, Int64(ptsUs))
  }

  func stepForward() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerStepForward(handle, &error, error.count)
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player stepForward failed with code \(ret)" : message
      )
    }
  }

  func stepBackward() throws {
    var error = [CChar](repeating: 0, count: 1024)
    let ret = VPMacOSNativePlayerStepBackward(handle, &error, error.count)
    if ret != 0 {
      let message = String(cString: error)
      throw MacOSNativePlayerError.failed(
        message.isEmpty ? "macOS native player stepBackward failed with code \(ret)" : message
      )
    }
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

  func hardwareDecodeActive() -> Bool {
    VPMacOSNativePlayerHardwareDecodeActive(handle) != 0
  }

  func hardwareDecodeDownloadsToCpu() -> Bool {
    VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(handle) != 0
  }

  func decodeModeName() -> String {
    String(cString: VPMacOSNativePlayerDecodeModeName(handle))
  }

  func decoderName() -> String {
    String(cString: VPMacOSNativePlayerDecoderName(handle))
  }

  func presentationSchedulerStats() -> [String: Any] {
    var stats = VPMacOSNativePresentationSchedulerStats()
    guard VPMacOSNativePlayerCopyPresentationSchedulerStats(handle, &stats) == 0 else {
      return [
        "tickCount": 0,
        "presentableTickCount": 0,
        "frameNotificationCount": 0,
        "lastSelectedPtsUs": -1,
        "lastPresentFrameCount": 0,
        "cachedPresentDecisionAvailable": false,
        "deadlineSleepCount": 0,
        "lastDeadlineSleepUs": 0,
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "tickCount": Int64(min(UInt64(stats.tick_count), maxInt64)),
      "presentableTickCount": Int64(min(UInt64(stats.presentable_tick_count), maxInt64)),
      "frameNotificationCount": Int64(min(UInt64(stats.frame_notification_count), maxInt64)),
      "lastSelectedPtsUs": Int64(stats.last_selected_pts_us),
      "lastPresentFrameCount": Int(stats.last_present_frame_count),
      "cachedPresentDecisionAvailable": stats.cached_present_decision_available != 0,
      "deadlineSleepCount": Int64(min(UInt64(stats.deadline_sleep_count), maxInt64)),
      "lastDeadlineSleepUs": Int64(stats.last_deadline_sleep_us),
    ]
  }

  func performanceStats() -> [String: Any] {
    var stats = VPMacOSNativePlayerPerfStats()
    guard VPMacOSNativePlayerCopyPerfStats(handle, &stats) == 0 else {
      return [
        "decodeFrameCount": 0,
        "decodeDroppedCount": 0,
        "decodeElapsedMs": 0,
        "decodeFps": 0.0,
        "decodeFpsX1000": 0,
        "decodeAvgMs": 0.0,
        "decodeMaxMs": 0.0,
        "rendererOwnedUploadCount": 0,
        "rendererOwnedUploadFailureCount": 0,
        "rendererOwnedUploadElapsedMs": 0,
        "rendererOwnedUploadFps": 0.0,
        "rendererOwnedUploadFpsX1000": 0,
        "rendererOwnedDirectYuvUploadCount": 0,
        "rendererOwnedCVPixelBufferUploadCount": 0,
        "rendererOwnedPresentPackageUploadCount": 0,
        "rendererOwnedPresentPackageCopyUs": 0,
        "rendererOwnedPresentPackageGpuWaitUs": 0,
        "rendererOwnedPresentPackageTotalUs": 0,
        "rendererOwnedPresentPackageStorage": "unavailable",
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "decodeFrameCount": Int64(min(UInt64(stats.decode_frame_count), maxInt64)),
      "decodeDroppedCount": Int64(min(UInt64(stats.decode_dropped_count), maxInt64)),
      "decodeElapsedMs": Int64(stats.decode_elapsed_ms),
      "decodeFps": stats.decode_fps,
      "decodeFpsX1000": Int64(max(0.0, stats.decode_fps * 1000.0)),
      "decodeAvgMs": stats.decode_avg_ms,
      "decodeMaxMs": stats.decode_max_ms,
      "rendererOwnedUploadCount": Int64(
        min(UInt64(stats.renderer_owned_upload_count), maxInt64)
      ),
      "rendererOwnedUploadFailureCount": Int64(
        min(UInt64(stats.renderer_owned_upload_failure_count), maxInt64)
      ),
      "rendererOwnedUploadElapsedMs": Int64(stats.renderer_owned_upload_elapsed_ms),
      "rendererOwnedUploadFps": stats.renderer_owned_upload_fps,
      "rendererOwnedUploadFpsX1000": Int64(
        max(0.0, stats.renderer_owned_upload_fps * 1000.0)
      ),
      "rendererOwnedDirectYuvUploadCount": Int64(
        stats.renderer_owned_direct_yuv_upload_count
      ),
      "rendererOwnedCVPixelBufferUploadCount": Int64(
        stats.renderer_owned_cvpixelbuffer_upload_count
      ),
      "rendererOwnedPresentPackageUploadCount": Int64(
        stats.renderer_owned_present_package_upload_count
      ),
      "rendererOwnedPresentPackageCopyUs": Int64(
        stats.renderer_owned_present_package_copy_us
      ),
      "rendererOwnedPresentPackageGpuWaitUs": Int64(
        stats.renderer_owned_present_package_gpu_wait_us
      ),
      "rendererOwnedPresentPackageTotalUs": Int64(
        stats.renderer_owned_present_package_total_us
      ),
      "rendererOwnedPresentPackageStorage": Self.presentPackageStorageName(
        stats.renderer_owned_present_package_storage
      ),
    ]
  }

  private static func presentPackageStorageName(_ storage: Int32) -> String {
    switch storage {
    case Int32(VPMacOSNativePresentPackageStorageYUV):
      return "yuv"
    case Int32(VPMacOSNativePresentPackageStorageBGRA):
      return "bgra"
    case Int32(VPMacOSNativePresentPackageStorageCVPixelBuffer):
      return "cvpixelbuffer"
    default:
      return "unavailable"
    }
  }

}
