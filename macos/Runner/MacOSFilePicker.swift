import Cocoa
import FlutterMacOS

final class MacOSSecurityScopedFileAccess {
  static let shared = MacOSSecurityScopedFileAccess()

  private let lock = NSLock()
  private var activeUrlsByPath: [String: URL] = [:]
  private var bookmarkDataByPath: [String: Data] = [:]

  private init() {}

  func retainUserSelectedFiles(_ urls: [URL]) {
    lock.lock()
    defer { lock.unlock() }
    for url in urls {
      let path = url.path
      if activeUrlsByPath[path] == nil {
        _ = url.startAccessingSecurityScopedResource()
        activeUrlsByPath[path] = url
      }
      if let bookmark = try? url.bookmarkData(
        options: [.withSecurityScope],
        includingResourceValuesForKeys: nil,
        relativeTo: nil
      ) {
        bookmarkDataByPath[path] = bookmark
      }
    }
  }

  func bookmarkData(forPath path: String) -> Data? {
    lock.lock()
    defer { lock.unlock() }
    return bookmarkDataByPath[path]
  }

  func bookmarkBase64(forPath path: String) -> String? {
    bookmarkData(forPath: path)?.base64EncodedString()
  }

  func releaseAll() {
    lock.lock()
    let urls = Array(activeUrlsByPath.values)
    activeUrlsByPath.removeAll()
    bookmarkDataByPath.removeAll()
    lock.unlock()
    for url in urls {
      url.stopAccessingSecurityScopedResource()
    }
  }
}

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
          MacOSSecurityScopedFileAccess.shared.retainUserSelectedFiles(panel.urls)
          let entries = panel.urls.map { url -> [String: Any] in
            var entry: [String: Any] = ["path": url.path]
            if let bookmark = MacOSSecurityScopedFileAccess.shared.bookmarkBase64(forPath: url.path) {
              entry["securityScopedBookmarkBase64"] = bookmark
            }
            return entry
          }
          result(entries)
        } else {
          result(nil)
        }
      }
    }
  }
}
