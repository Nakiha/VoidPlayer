import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  override func close() {
    MacOSVideoRendererBridge.destroyActivePlayerForWindowClose()
    super.close()
  }

  override func awakeFromNib() {
    DispatchQueue.global(qos: .userInitiated).async {
      VPMacOSNativePrewarmMetalPipelines()
    }
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    MacOSVideoRendererBridge.register(
      with: flutterViewController.engine,
      contentView: flutterViewController.view
    )
    QuickMarkCursorBridge.register(with: flutterViewController.engine)
    RegisterGeneratedPlugins(registry: flutterViewController)

    super.awakeFromNib()
  }
}

private final class QuickMarkCursorBridge {
  private static let channelName = "void_player/quick_mark_cursor"
  private static var channel: FlutterMethodChannel?
  private static var upLeftDownRightCursor: NSCursor?
  private static var upRightDownLeftCursor: NSCursor?

  static func register(with engine: FlutterEngine) {
    let channel = FlutterMethodChannel(
      name: channelName,
      binaryMessenger: engine.binaryMessenger
    )
    channel.setMethodCallHandler { call, result in
      guard call.method == "activateDiagonalResizeCursor" else {
        result(FlutterMethodNotImplemented)
        return
      }
      guard let args = call.arguments as? [String: Any],
            let kind = args["kind"] as? String else {
        result(FlutterError(code: "bad_args", message: "Missing cursor kind", details: nil))
        return
      }
      cursor(for: kind).set()
      result(nil)
    }
    self.channel = channel
  }

  private static func cursor(for kind: String) -> NSCursor {
    if kind == "upRightDownLeft" {
      if let cursor = upRightDownLeftCursor {
        return cursor
      }
      let cursor = makeDiagonalCursor(kind: kind)
      upRightDownLeftCursor = cursor
      return cursor
    }
    if let cursor = upLeftDownRightCursor {
      return cursor
    }
    let cursor = makeDiagonalCursor(kind: "upLeftDownRight")
    upLeftDownRightCursor = cursor
    return cursor
  }

  private static func makeDiagonalCursor(kind: String) -> NSCursor {
    let size = NSSize(width: 24, height: 24)
    let image = NSImage(size: size)
    image.lockFocus()
    defer { image.unlockFocus() }

    let start: NSPoint
    let end: NSPoint
    if kind == "upRightDownLeft" {
      start = NSPoint(x: 19, y: 19)
      end = NSPoint(x: 5, y: 5)
    } else {
      start = NSPoint(x: 5, y: 19)
      end = NSPoint(x: 19, y: 5)
    }

    drawDoubleArrow(start: start, end: end, color: .black, lineWidth: 4.0)
    drawDoubleArrow(start: start, end: end, color: .white, lineWidth: 2.0)
    return NSCursor(image: image, hotSpot: NSPoint(x: 12, y: 12))
  }

  private static func drawDoubleArrow(
    start: NSPoint,
    end: NSPoint,
    color: NSColor,
    lineWidth: CGFloat
  ) {
    color.setStroke()
    let path = NSBezierPath()
    path.lineCapStyle = .round
    path.lineJoinStyle = .round
    path.lineWidth = lineWidth
    path.move(to: start)
    path.line(to: end)
    addArrowHead(to: path, tip: start, tail: end)
    addArrowHead(to: path, tip: end, tail: start)
    path.stroke()
  }

  private static func addArrowHead(to path: NSBezierPath, tip: NSPoint, tail: NSPoint) {
    let angle = atan2(tip.y - tail.y, tip.x - tail.x)
    let length: CGFloat = 5.5
    let spread: CGFloat = .pi / 5.0
    let left = NSPoint(
      x: tip.x - cos(angle - spread) * length,
      y: tip.y - sin(angle - spread) * length
    )
    let right = NSPoint(
      x: tip.x - cos(angle + spread) * length,
      y: tip.y - sin(angle + spread) * length
    )
    path.move(to: tip)
    path.line(to: left)
    path.move(to: tip)
    path.line(to: right)
  }
}
