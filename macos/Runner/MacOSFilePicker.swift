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

  func activateBookmarks(_ bookmarksByPath: [String: String]) -> [[String: Any]] {
    var results: [[String: Any]] = []
    for (requestedPath, bookmarkBase64) in bookmarksByPath {
      var entry: [String: Any] = [
        "path": requestedPath,
        "requestedPath": requestedPath,
        "activated": false,
        "stale": false,
      ]
      guard let bookmarkData = Data(base64Encoded: bookmarkBase64) else {
        entry["error"] = "invalid bookmark data"
        results.append(entry)
        continue
      }

      var stale = false
      do {
        let url = try URL(
          resolvingBookmarkData: bookmarkData,
          options: [.withSecurityScope],
          relativeTo: nil,
          bookmarkDataIsStale: &stale
        )
        lock.lock()
        let alreadyActive = activeUrlsByPath[url.path] != nil
        lock.unlock()
        let activated = alreadyActive || url.startAccessingSecurityScopedResource()
        lock.lock()
        if activated && !alreadyActive {
          activeUrlsByPath[url.path] = url
        }
        if let refreshedBookmark = try? url.bookmarkData(
          options: [.withSecurityScope],
          includingResourceValuesForKeys: nil,
          relativeTo: nil
        ) {
          bookmarkDataByPath[url.path] = refreshedBookmark
          entry["securityScopedBookmarkBase64"] = refreshedBookmark.base64EncodedString()
        }
        lock.unlock()
        entry["path"] = url.path
        entry["activated"] = activated
        entry["stale"] = stale
      } catch {
        entry["error"] = error.localizedDescription
      }
      results.append(entry)
    }
    return results
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
  static func activateSecurityScopedBookmarks(arguments: Any?, result: @escaping FlutterResult) {
    guard let args = arguments as? [String: Any],
          let bookmarks = args["bookmarks"] as? [String: String] else {
      result([])
      return
    }
    result(MacOSSecurityScopedFileAccess.shared.activateBookmarks(bookmarks))
  }

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
