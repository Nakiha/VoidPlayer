import Cocoa
import FlutterMacOS

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
