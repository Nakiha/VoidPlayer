import Foundation

enum MacOSNativeLayoutBridge {
  static func layoutMap(arguments: Any?) -> [String: Any]? {
    guard let nextLayout = arguments as? [String: Any] else {
      return nil
    }
    return nextLayout
  }

  static func apply(
    layout nextLayout: [String: Any],
    player: MacOSNativePlayerSession?
  ) {
    player?.applyLayout(
      mode: MacOSFlutterArguments.intValue(nextLayout["mode"]) ?? 0,
      splitPos: MacOSFlutterArguments.doubleValue(nextLayout["splitPos"]) ?? 0.5,
      zoomRatio: MacOSFlutterArguments.doubleValue(nextLayout["zoomRatio"]) ?? 1.0,
      viewOffsetX: MacOSFlutterArguments.doubleValue(nextLayout["viewOffsetX"]) ?? 0.0,
      viewOffsetY: MacOSFlutterArguments.doubleValue(nextLayout["viewOffsetY"]) ?? 0.0,
      pixelSizeMode: MacOSFlutterArguments.intValue(nextLayout["pixelSizeMode"]) ?? 0,
      order: MacOSFlutterArguments.intListValue(nextLayout["order"])
    )
  }
}
