import Foundation

final class MacOSVideoTrackStore {
  private(set) var tracks: [[String: Any]] = []
  private(set) var currentDurationUs = 0

  var count: Int {
    tracks.count
  }

  var isEmpty: Bool {
    tracks.isEmpty
  }

  var hasHDRTrack: Bool {
    tracks.contains { track in
      let transfer = MacOSFlutterArguments.intValue(track["colorTransfer"]) ??
        MacOSNativeColorTransfer.unknown
      return transfer == MacOSNativeColorTransfer.pq ||
        transfer == MacOSNativeColorTransfer.hlg
    }
  }

  func reset() {
    tracks.removeAll()
    currentDurationUs = 0
  }

  func replace(with tracks: [[String: Any]], fallbackDurationUs: Int) {
    self.tracks = tracks
    refreshDuration(fallbackDurationUs: fallbackDurationUs)
  }

  func append(_ track: [String: Any]) {
    tracks.append(track)
    currentDurationUs = max(
      currentDurationUs,
      MacOSFlutterArguments.intValue(track["durationUs"]) ?? 0
    )
  }

  func remove(fileId: Int, compactSlots: Bool) {
    tracks.removeAll { MacOSFlutterArguments.intValue($0["fileId"]) == fileId }
    if compactSlots {
      tracks = tracks.enumerated().map { index, track in
        var next = track
        next["slot"] = index
        return next
      }
    }
    refreshDuration(fallbackDurationUs: 0)
  }

  func nextFileId() -> Int {
    (tracks.map { MacOSFlutterArguments.intValue($0["fileId"]) ?? 0 }.max() ?? -1) + 1
  }

  func activeSlotCapacity() -> Int {
    let maxSlot = tracks
      .compactMap { MacOSFlutterArguments.intValue($0["slot"]) }
      .max() ?? 0
    return max(1, min(4, maxSlot + 1))
  }

  private func refreshDuration(fallbackDurationUs: Int) {
    currentDurationUs = tracks
      .map { MacOSFlutterArguments.intValue($0["durationUs"]) ?? fallbackDurationUs }
      .max() ?? fallbackDurationUs
  }
}
