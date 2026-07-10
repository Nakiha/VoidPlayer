import CoreVideo
import Foundation

struct MacOSCompositorSourceTrackDescriptor {
  let slot: Int
  let fileId: Int
  let width: Int
  let height: Int
  let colorTransfer: Int
  let displayOffsetX: Float
  let displayOffsetY: Float
  let invDisplaySizeX: Float
  let invDisplaySizeY: Float
  let viewOffsetUvX: Float
  let viewOffsetUvY: Float
}

struct MacOSNativeCompositorSourceRefreshResult {
  let published: Bool
  let publishCount: Int
  let drawnMask: UInt64
  let reusedMask: UInt64
  let missingMask: UInt64
  let ptsUs: Int64
  let error: String
}

/// Serializes source-package refresh requests around a native-owned lease.
///
/// CVPixelBuffer/IOSurface allocation, ring generations, package completeness,
/// and lifecycle validation live in native. Swift retains only the published
/// package handed to the runner compositor and the projection supplied by Dart.
final class MacOSNativeCompositorSourceRing {
  private weak var compositor: MacOSNativeCompositorView?
  private let lease: OpaquePointer
  private let ringQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.source-ring",
    qos: .userInteractive
  )

  // Confined to ringQueue.
  private var projection: MacOSNativeCompositorSourceProjection?
  private var live = false
  private var baking = false
  private var pending = false
  private var topologyRevision: UInt64 = 0
  private let refreshRequestRate = MacOSRateWindow()
  private let bakeRate = MacOSRateWindow()
  private let publishRate = MacOSRateWindow()
  private let bakeDuration = MacOSDurationWindow()
  private let refreshQueueWaitDuration = MacOSDurationWindow()
  private let requestToPublishDuration = MacOSDurationWindow()
  private let publishedPtsStepDuration = MacOSDurationWindow()
  private var refreshRequestCount = 0
  private var refreshCoalescedCount = 0
  private var bakeCount = 0
  private var publishMissCount = 0
  private var lastBakeDrawnCount = 0
  private var lastBakeError = ""
  private var lastRequiredMask: UInt64 = 0
  private var lastDrawnMask: UInt64 = 0
  private var lastMissingMask: UInt64 = 0
  private var lastPublishedSlotSignature = ""
  private var lastPublishedFileIdSignature = ""
  private var lastPublishedBufferSignature = ""
  private var lastPublishedDuplicateSlotCount = 0
  private var lastPublishedDuplicateFileIdCount = 0
  private var lastPublishedDuplicateBufferCount = 0
  private var lastPublishedPtsUs: Int64 = -1
  private var lastPublishedDurationUs: Int64 = 0
  private var publishedPtsDuplicateCount = 0
  private var publishedPtsLargeStepCount = 0
  private var publishedPtsRegressionCount = 0

  init(compositor: MacOSNativeCompositorView) {
    self.compositor = compositor
    guard let lease = VPMacOSNativeSourceCompositorLeaseCreate() else {
      preconditionFailure("failed to create native source compositor lease")
    }
    self.lease = lease
  }

  deinit {
    VPMacOSNativeSourceCompositorLeaseDestroy(lease)
  }

  func subscribe(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    order: [Int],
    edrOutputEnabled: Bool,
    topologyRevision: UInt64,
    projection: MacOSNativeCompositorSourceProjection
  ) -> MacOSNativeCompositorSourceRefreshResult {
    let semaphore = DispatchSemaphore(value: 0)
    var result = Self.emptyFailure("source ring subscribe did not run")
    ringQueue.async { [weak self] in
      if let self {
        result = self.subscribeOnQueue(
          player: player,
          descriptors: descriptors,
          edrOutputEnabled: edrOutputEnabled,
          topologyRevision: topologyRevision,
          projection: projection
        )
      }
      semaphore.signal()
    }
    semaphore.wait()
    _ = order // Ordering is carried by the projection package.
    return result
  }

  func hasCompletePackage(topologyRevision expectedTopologyRevision: UInt64) -> Bool {
    ringQueue.sync {
      live && VPMacOSNativeSourceCompositorLeaseHasCompletePackage(
        lease,
        expectedTopologyRevision
      ) != 0
    }
  }

  func updateProjection(_ projection: MacOSNativeCompositorSourceProjection) {
    ringQueue.async { [weak self] in
      self?.projection = projection
    }
  }

  func requestRefresh(player: MacOSNativePlayerSession) {
    let requestedNs = DispatchTime.now().uptimeNanoseconds
    ringQueue.async { [weak self] in
      guard let self, live else { return }
      recordRefreshRequest(requestedNs: requestedNs)
      if nativeDiagnostics().frozen_snapshot != 0 { return }
      if baking {
        refreshCoalescedCount += 1
        pending = true
        return
      }
      baking = true
      repeat {
        pending = false
        _ = bakeAndPublishOnQueue(player: player, requestNs: requestedNs)
      } while pending && live
      baking = false
    }
  }

  func refreshAndWait(
    player: MacOSNativePlayerSession,
    timeoutMs: Int
  ) -> MacOSNativeCompositorSourceRefreshResult {
    let requestedNs = DispatchTime.now().uptimeNanoseconds
    let semaphore = DispatchSemaphore(value: 0)
    var result = Self.emptyFailure("source ring refresh timed out")
    ringQueue.async { [weak self] in
      guard let self else {
        semaphore.signal()
        return
      }
      guard live else {
        result = failedResult("source ring is not live")
        semaphore.signal()
        return
      }
      recordRefreshRequest(requestedNs: requestedNs)
      if nativeDiagnostics().frozen_snapshot != 0 {
        result = failedResult("source ring is frozen")
        semaphore.signal()
        return
      }
      if baking {
        refreshCoalescedCount += 1
        pending = true
        result = failedResult("source ring is already baking")
        semaphore.signal()
        return
      }
      baking = true
      pending = false
      result = bakeAndPublishOnQueue(player: player, requestNs: requestedNs)
      baking = false
      semaphore.signal()
    }
    if semaphore.wait(timeout: .now() + .milliseconds(max(0, timeoutMs))) == .timedOut {
      return result
    }
    return result
  }

  func unsubscribe(reason: String) {
    ringQueue.async { [weak self] in
      guard let self else { return }
      live = false
      baking = false
      pending = false
      projection = nil
      VPMacOSNativeSourceCompositorLeaseReset(lease)
      compositor?.clearSource(reason: reason)
    }
  }

  func diagnostics() -> [String: Any] {
    ringQueue.sync {
      let native = nativeDiagnostics()
      let complete = live && native.published_slot_mask == native.required_slot_mask &&
        native.required_slot_mask != 0
      return [
        "sourceRingLive": live,
        "sourceRingHasPublished": native.publish_count > 0,
        "sourceRingComplete": complete,
        "sourceRingNativeOwned": true,
        "sourceRingLifecycleState": Int(native.lifecycle_state),
        "sourceRingDepth": Int(native.ring_depth),
        "sourceRingTrackCount": Int(native.track_count),
        "sourceRingFrozenSnapshot": native.frozen_snapshot != 0,
        "sourceRingBytesPerFrame": Int64(min(native.bytes_per_frame, UInt64(Int64.max))),
        "sourceRingTotalBytes": Int64(min(native.total_bytes, UInt64(Int64.max))),
        "sourceRingRefreshRequestCount": refreshRequestCount,
        "sourceRingRefreshRequestHz": refreshRequestRate.rateHz(),
        "sourceRingRefreshQueueWaitP95Ms": refreshQueueWaitDuration.p95Ms(),
        "sourceRingRefreshQueueWaitLastMs": refreshQueueWaitDuration.lastMs(),
        "sourceRingRefreshCoalescedCount": refreshCoalescedCount,
        "sourceRingBaking": baking,
        "sourceRingPending": pending,
        "sourceRingBakeCount": bakeCount,
        "sourceRingBakeHz": bakeRate.rateHz(),
        "sourceRingBakeP95Ms": bakeDuration.p95Ms(),
        "sourceRingBakeLastMs": bakeDuration.lastMs(),
        "sourceRingLastBakeDrawnCount": lastBakeDrawnCount,
        "sourceRingLastBakeError": lastBakeError,
        "sourceRingTopologyRevision": Int64(min(topologyRevision, UInt64(Int64.max))),
        "sourceRingReadyTopologyRevision": Int64(min(native.topology_generation, UInt64(Int64.max))),
        "sourceRingRingGeneration": Int64(min(native.ring_generation, UInt64(Int64.max))),
        "sourceRingFrameGeneration": Int64(min(native.frame_generation, UInt64(Int64.max))),
        "sourceRingRequiredMask": Int64(min(native.required_slot_mask, UInt64(Int64.max))),
        "sourceRingDrawnMask": Int64(min(lastDrawnMask, UInt64(Int64.max))),
        "sourceRingMissingMask": Int64(min(lastMissingMask, UInt64(Int64.max))),
        "sourceRingReusedMask": 0,
        "sourceRingReusedPublishedSlotCount": 0,
        "sourceRingIncompletePublishSuppressedCount": Int64(
          min(native.incomplete_publish_count, UInt64(Int64.max))
        ),
        "sourceRingLastIncompleteReason": lastMissingMask == 0 ? "" : lastBakeError,
        "sourceRingPublishedSlotSignature": lastPublishedSlotSignature,
        "sourceRingPublishedFileIdSignature": lastPublishedFileIdSignature,
        "sourceRingPublishedActualFileIdSignature": lastPublishedFileIdSignature,
        "sourceRingPublishedBufferSignature": lastPublishedBufferSignature,
        "sourceRingPublishedDuplicateSlotCount": lastPublishedDuplicateSlotCount,
        "sourceRingPublishedDuplicateFileIdCount": lastPublishedDuplicateFileIdCount,
        "sourceRingPublishedDuplicateActualFileIdCount": lastPublishedDuplicateFileIdCount,
        "sourceRingPublishedDuplicateBufferCount": lastPublishedDuplicateBufferCount,
        "sourceRingPublishCount": Int64(min(native.publish_count, UInt64(Int64.max))),
        "sourceRingPublishHz": publishRate.rateHz(),
        "sourceRingRequestToPublishP95Ms": requestToPublishDuration.p95Ms(),
        "sourceRingRequestToPublishLastMs": requestToPublishDuration.lastMs(),
        "sourceRingPublishMissCount": publishMissCount,
        "sourceRingLastPublishedPtsUs": lastPublishedPtsUs,
        "sourceRingLastPublishedDurationUs": lastPublishedDurationUs,
        "sourceRingPublishedPtsStepP95Ms": publishedPtsStepDuration.p95Ms(),
        "sourceRingPublishedPtsStepLastMs": publishedPtsStepDuration.lastMs(),
        "sourceRingPublishedPtsDuplicateCount": publishedPtsDuplicateCount,
        "sourceRingPublishedPtsLargeStepCount": publishedPtsLargeStepCount,
        "sourceRingPublishedPtsRegressionCount": publishedPtsRegressionCount,
      ]
    }
  }

  private func subscribeOnQueue(
    player: MacOSNativePlayerSession,
    descriptors: [MacOSCompositorSourceTrackDescriptor],
    edrOutputEnabled: Bool,
    topologyRevision: UInt64,
    projection: MacOSNativeCompositorSourceProjection
  ) -> MacOSNativeCompositorSourceRefreshResult {
    guard !descriptors.isEmpty else {
      return failedResult("no source tracks")
    }
    let nativeDescriptors = descriptors.map { descriptor in
      VPMacOSNativeSourceCompositorDescriptor(
        source_slot: Int32(descriptor.slot),
        source_file_id: Int32(descriptor.fileId),
        width: Int32(descriptor.width),
        height: Int32(descriptor.height),
        color_transfer: Int32(descriptor.colorTransfer)
      )
    }
    var package = VPMacOSNativeSourceCompositorPackage()
    VPMacOSNativeSourceCompositorPackageInit(&package)
    var error = [CChar](repeating: 0, count: 512)
    let startNs = DispatchTime.now().uptimeNanoseconds
    let status = nativeDescriptors.withUnsafeBufferPointer { buffer in
      VPMacOSNativeSourceCompositorLeaseSubscribeAndBake(
        lease,
        player.handle,
        buffer.baseAddress,
        buffer.count,
        edrOutputEnabled ? 1 : 0,
        topologyRevision,
        &package,
        &error,
        error.count
      )
    }
    recordBake(startNs: startNs, status: status, error: String(cString: error))
    guard status == VPMacOSNativeStatusOk.rawValue else {
      publishMissCount += 1
      return failedResult(lastBakeError)
    }
    self.projection = projection
    self.topologyRevision = topologyRevision
    live = true
    return publish(package: &package, player: player, requestNs: nil)
  }

  private func bakeAndPublishOnQueue(
    player: MacOSNativePlayerSession,
    requestNs: UInt64?
  ) -> MacOSNativeCompositorSourceRefreshResult {
    guard live else { return failedResult("source ring is not ready") }
    var package = VPMacOSNativeSourceCompositorPackage()
    VPMacOSNativeSourceCompositorPackageInit(&package)
    var error = [CChar](repeating: 0, count: 512)
    let startNs = DispatchTime.now().uptimeNanoseconds
    let status = VPMacOSNativeSourceCompositorLeaseRefreshAndBake(
      lease,
      player.handle,
      &package,
      &error,
      error.count
    )
    recordBake(startNs: startNs, status: status, error: String(cString: error))
    guard status == VPMacOSNativeStatusOk.rawValue else {
      publishMissCount += 1
      return failedResult(lastBakeError)
    }
    return publish(package: &package, player: player, requestNs: requestNs)
  }

  private func publish(
    package: inout VPMacOSNativeSourceCompositorPackage,
    player: MacOSNativePlayerSession,
    requestNs: UInt64?
  ) -> MacOSNativeCompositorSourceRefreshResult {
    guard let projection else {
      return failedResult("source package projection is not ready")
    }
    var textures: [MacOSNativeCompositorSourceTexture] = []
    var ptsUs: Int64 = -1
    var durationUs: Int64 = 0
    withUnsafeBytes(of: &package.entries) { raw in
      let entries = raw.bindMemory(to: VPMacOSNativeSourceCompositorPackageEntry.self)
      for index in 0..<min(Int(package.track_count), entries.count) {
        let entry = entries[index]
        guard let pointer = entry.pixel_buffer else { continue }
        let pixelBuffer = Unmanaged<CVPixelBuffer>
          .fromOpaque(pointer)
          .takeRetainedValue()
        textures.append(MacOSNativeCompositorSourceTexture(
          pixelBuffer: pixelBuffer,
          sourceSlot: Int(entry.source_slot),
          fileId: Int(entry.source_file_id),
          width: Int(entry.width),
          height: Int(entry.height)
        ))
        if ptsUs < 0 {
          ptsUs = entry.frame_info.pts_us
          durationUs = entry.frame_info.duration_us
        }
      }
    }
    let requiredMask = package.required_slot_mask
    let drawnMask = package.published_slot_mask
    let missingMask = requiredMask & ~drawnMask
    lastRequiredMask = requiredMask
    lastDrawnMask = drawnMask
    lastMissingMask = missingMask
    guard !textures.isEmpty, missingMask == 0 else {
      publishMissCount += 1
      return failedResult("native source package was incomplete")
    }

    updatePublishedIdentity(textures)
    updatePublishedPts(ptsUs: ptsUs, durationUs: durationUs)
    let publishNs = DispatchTime.now().uptimeNanoseconds
    publishRate.record(nowNs: publishNs)
    if let requestNs {
      requestToPublishDuration.record(publishNs >= requestNs ? publishNs - requestNs : 0)
    }
    compositor?.setSourcePackage(
      textures: textures,
      projection: projection,
      overlay: player.currentOverlayPrimitives()
    )
    return MacOSNativeCompositorSourceRefreshResult(
      published: true,
      publishCount: Int(min(package.publish_count, UInt64(Int.max))),
      drawnMask: drawnMask,
      reusedMask: 0,
      missingMask: 0,
      ptsUs: ptsUs,
      error: ""
    )
  }

  private func recordRefreshRequest(requestedNs: UInt64) {
    let queueStartNs = DispatchTime.now().uptimeNanoseconds
    refreshQueueWaitDuration.record(queueStartNs >= requestedNs ? queueStartNs - requestedNs : 0)
    refreshRequestCount += 1
    refreshRequestRate.record(nowNs: queueStartNs)
  }

  private func recordBake(startNs: UInt64, status: Int32, error: String) {
    let finishNs = DispatchTime.now().uptimeNanoseconds
    bakeCount += 1
    bakeRate.record(nowNs: finishNs)
    bakeDuration.record(finishNs >= startNs ? finishNs - startNs : 0)
    lastBakeError = status == VPMacOSNativeStatusOk.rawValue ? "" : error
    let native = nativeDiagnostics()
    lastBakeDrawnCount = native.published_slot_mask.nonzeroBitCount
  }

  private func nativeDiagnostics() -> VPMacOSNativeSourceCompositorDiagnostics {
    var result = VPMacOSNativeSourceCompositorDiagnostics()
    VPMacOSNativeSourceCompositorDiagnosticsInit(&result)
    _ = VPMacOSNativeSourceCompositorLeaseCopyDiagnostics(lease, &result)
    return result
  }

  private func updatePublishedPts(ptsUs: Int64, durationUs: Int64) {
    guard ptsUs >= 0 else { return }
    if lastPublishedPtsUs >= 0 {
      let stepUs = ptsUs - lastPublishedPtsUs
      if stepUs < 0 {
        publishedPtsRegressionCount += 1
      } else if stepUs == 0 {
        publishedPtsDuplicateCount += 1
      } else {
        if stepUs > 50_000 { publishedPtsLargeStepCount += 1 }
        publishedPtsStepDuration.record(UInt64(stepUs) * 1_000)
      }
    }
    lastPublishedPtsUs = ptsUs
    lastPublishedDurationUs = durationUs
  }

  private func updatePublishedIdentity(_ textures: [MacOSNativeCompositorSourceTexture]) {
    let sorted = textures.sorted { $0.sourceSlot < $1.sourceSlot }
    let slots = sorted.map(\.sourceSlot)
    let fileIds = sorted.map(\.fileId)
    let buffers = sorted.map {
      UInt(bitPattern: Unmanaged.passUnretained($0.pixelBuffer).toOpaque())
    }
    lastPublishedSlotSignature = zip(slots, fileIds)
      .map { "s\($0.0):f\($0.1)" }
      .joined(separator: "|")
    lastPublishedFileIdSignature = fileIds.map(String.init).joined(separator: ",")
    lastPublishedBufferSignature = buffers.map { String($0, radix: 16) }.joined(separator: ",")
    lastPublishedDuplicateSlotCount = max(0, slots.count - Set(slots).count)
    lastPublishedDuplicateFileIdCount = max(0, fileIds.count - Set(fileIds).count)
    lastPublishedDuplicateBufferCount = max(0, buffers.count - Set(buffers).count)
  }

  private func failedResult(_ error: String) -> MacOSNativeCompositorSourceRefreshResult {
    let native = nativeDiagnostics()
    return MacOSNativeCompositorSourceRefreshResult(
      published: false,
      publishCount: Int(min(native.publish_count, UInt64(Int.max))),
      drawnMask: lastDrawnMask,
      reusedMask: 0,
      missingMask: lastMissingMask,
      ptsUs: lastPublishedPtsUs,
      error: error
    )
  }

  private static func emptyFailure(_ error: String) -> MacOSNativeCompositorSourceRefreshResult {
    MacOSNativeCompositorSourceRefreshResult(
      published: false,
      publishCount: 0,
      drawnMask: 0,
      reusedMask: 0,
      missingMask: 0,
      ptsUs: -1,
      error: error
    )
  }
}
