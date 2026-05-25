import Foundation

extension MacOSNativePlayerSession {
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
}
