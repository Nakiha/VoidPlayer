import Cocoa
import FlutterMacOS

enum MacOSFilePicker {
  static func pickFiles(arguments: Any?, result: @escaping FlutterResult) {
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
