import Foundation

struct MacOSNativeOverlayPrimitives {
  static let empty = MacOSNativeOverlayPrimitives(
    generation: 0,
    sourceBakedOverlayDisabled: true,
    overlayTrackCount: 0,
    matchedTrackCount: 0,
    missingTrackSlotCount: 0,
    missingPresentedFrameCount: 0,
    missingFrameIndexCount: 0,
    invalidVideoSizeCount: 0,
    overlayFrameMissingCount: 0,
    heatmapMissingFeatureTrackCount: 0,
    fillRects: [],
    lineRects: [],
    motionLines: []
  )

  let generation: UInt64
  let sourceBakedOverlayDisabled: Bool
  let overlayTrackCount: UInt64
  let matchedTrackCount: UInt64
  let missingTrackSlotCount: UInt64
  let missingPresentedFrameCount: UInt64
  let missingFrameIndexCount: UInt64
  let invalidVideoSizeCount: UInt64
  let overlayFrameMissingCount: UInt64
  let heatmapMissingFeatureTrackCount: UInt64
  let fillRects: [VPMacOSNativeOverlayGpuRect]
  let lineRects: [VPMacOSNativeOverlayGpuRect]
  let motionLines: [VPMacOSNativeOverlayGpuRect]

  var isEmpty: Bool {
    fillRects.isEmpty && lineRects.isEmpty && motionLines.isEmpty
  }
}

extension MacOSNativePlayerSession {
  func currentOverlayPrimitives() -> MacOSNativeOverlayPrimitives {
    var snapshot = VPMacOSNativeOverlayPrimitiveSnapshot()
    VPMacOSNativeOverlayPrimitiveSnapshotInit(&snapshot)
    var error = [CChar](repeating: 0, count: 512)
    let countRet = VPMacOSNativePlayerCopyCurrentOverlayPrimitives(
      handle,
      &snapshot,
      nil,
      0,
      nil,
      0,
      nil,
      0,
      &error,
      error.count
    )
    guard countRet == 0 || countRet == -2 else {
      let message = String(cString: error)
      String(
        format: "NativeOverlayPrimitiveCopy failed phase=count ret=%d error=%@",
        countRet,
        message
      ).withCString { pointer in
        VPMacOSLogProfilerSummary(pointer)
      }
      return MacOSNativeOverlayPrimitives(
        generation: 0,
        sourceBakedOverlayDisabled: true,
        overlayTrackCount: 0,
        matchedTrackCount: 0,
        missingTrackSlotCount: 0,
        missingPresentedFrameCount: 0,
        missingFrameIndexCount: 0,
        invalidVideoSizeCount: 0,
        overlayFrameMissingCount: 0,
        heatmapMissingFeatureTrackCount: 0,
        fillRects: [],
        lineRects: [],
        motionLines: []
      )
    }

    let fillCount = Int(snapshot.fill_rect_count)
    let lineCount = Int(snapshot.line_rect_count)
    let motionCount = Int(snapshot.motion_line_count)
    if fillCount == 0 && lineCount == 0 && motionCount == 0 {
      return MacOSNativeOverlayPrimitives(
        generation: snapshot.generation,
        sourceBakedOverlayDisabled: snapshot.source_baked_overlay_disabled != 0,
        overlayTrackCount: snapshot.overlay_track_count,
        matchedTrackCount: snapshot.matched_track_count,
        missingTrackSlotCount: snapshot.missing_track_slot_count,
        missingPresentedFrameCount: snapshot.missing_presented_frame_count,
        missingFrameIndexCount: snapshot.missing_frame_index_count,
        invalidVideoSizeCount: snapshot.invalid_video_size_count,
        overlayFrameMissingCount: snapshot.overlay_frame_missing_count,
        heatmapMissingFeatureTrackCount: snapshot.heatmap_missing_feature_track_count,
        fillRects: [],
        lineRects: [],
        motionLines: []
      )
    }

    var fillRects = [VPMacOSNativeOverlayGpuRect](
      repeating: VPMacOSNativeOverlayGpuRect(),
      count: fillCount
    )
    var lineRects = [VPMacOSNativeOverlayGpuRect](
      repeating: VPMacOSNativeOverlayGpuRect(),
      count: lineCount
    )
    var motionLines = [VPMacOSNativeOverlayGpuRect](
      repeating: VPMacOSNativeOverlayGpuRect(),
      count: motionCount
    )
    let copyRet = fillRects.withUnsafeMutableBufferPointer { fillBuffer in
      lineRects.withUnsafeMutableBufferPointer { lineBuffer in
        motionLines.withUnsafeMutableBufferPointer { motionBuffer in
          VPMacOSNativePlayerCopyCurrentOverlayPrimitives(
            handle,
            &snapshot,
            fillBuffer.baseAddress,
            fillBuffer.count,
            lineBuffer.baseAddress,
            lineBuffer.count,
            motionBuffer.baseAddress,
            motionBuffer.count,
            &error,
            error.count
          )
        }
      }
    }
    guard copyRet == 0 else {
      let message = String(cString: error)
      String(
        format: "NativeOverlayPrimitiveCopy failed phase=copy ret=%d error=%@",
        copyRet,
        message
      ).withCString { pointer in
        VPMacOSLogProfilerSummary(pointer)
      }
      return MacOSNativeOverlayPrimitives(
        generation: 0,
        sourceBakedOverlayDisabled: true,
        overlayTrackCount: 0,
        matchedTrackCount: 0,
        missingTrackSlotCount: 0,
        missingPresentedFrameCount: 0,
        missingFrameIndexCount: 0,
        invalidVideoSizeCount: 0,
        overlayFrameMissingCount: 0,
        heatmapMissingFeatureTrackCount: 0,
        fillRects: [],
        lineRects: [],
        motionLines: []
      )
    }

    return MacOSNativeOverlayPrimitives(
      generation: snapshot.generation,
      sourceBakedOverlayDisabled: snapshot.source_baked_overlay_disabled != 0,
      overlayTrackCount: snapshot.overlay_track_count,
      matchedTrackCount: snapshot.matched_track_count,
      missingTrackSlotCount: snapshot.missing_track_slot_count,
      missingPresentedFrameCount: snapshot.missing_presented_frame_count,
      missingFrameIndexCount: snapshot.missing_frame_index_count,
      invalidVideoSizeCount: snapshot.invalid_video_size_count,
      overlayFrameMissingCount: snapshot.overlay_frame_missing_count,
      heatmapMissingFeatureTrackCount: snapshot.heatmap_missing_feature_track_count,
      fillRects: fillRects,
      lineRects: lineRects,
      motionLines: motionLines
    )
  }
}
