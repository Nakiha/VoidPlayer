import Foundation

enum MacOSFlutterArguments {
  private static func argumentMap(_ arguments: Any?) -> [String: Any]? {
    if let map = arguments as? [String: Any] {
      return map
    }
    if let map = arguments as? [AnyHashable: Any] {
      var result: [String: Any] = [:]
      for (key, value) in map {
        if let key = key as? String {
          result[key] = value
        }
      }
      return result
    }
    if let dictionary = arguments as? NSDictionary {
      var result: [String: Any] = [:]
      for (key, value) in dictionary {
        if let key = key as? String {
          result[key] = value
        }
      }
      return result
    }
    return nil
  }

  static func intArg(_ arguments: Any?, _ key: String) -> Int? {
    guard let map = argumentMap(arguments) else { return nil }
    return intValue(map[key])
  }

  static func boolArg(_ arguments: Any?, _ key: String) -> Bool? {
    guard let map = argumentMap(arguments) else { return nil }
    if let value = map[key] as? Bool {
      return value
    }
    if let value = map[key] as? NSNumber {
      return value.boolValue
    }
    return nil
  }

  static func doubleArg(_ arguments: Any?, _ key: String) -> Double? {
    guard let map = argumentMap(arguments) else { return nil }
    return doubleValue(map[key])
  }

  static func doubleValue(_ value: Any?) -> Double? {
    if let value = value as? Double {
      return value
    }
    if let value = value as? NSNumber {
      return value.doubleValue
    }
    return nil
  }

  static func stringArg(_ arguments: Any?, _ key: String) -> String? {
    guard let map = argumentMap(arguments) else { return nil }
    return map[key] as? String
  }

  static func stringListArg(_ arguments: Any?, _ key: String) -> [String] {
    guard let map = argumentMap(arguments) else { return [] }
    if let values = map[key] as? [String] {
      return values
    }
    if let values = map[key] as? [Any] {
      return values.compactMap { $0 as? String }
    }
    return []
  }

  static func intValue(_ value: Any?) -> Int? {
    if let value = value as? Int {
      return value
    }
    if let value = value as? Int64 {
      return Int(value)
    }
    if let value = value as? NSNumber {
      return value.intValue
    }
    return nil
  }

  static func intListValue(_ value: Any?) -> [Int] {
    if let values = value as? [Int] {
      return values
    }
    if let values = value as? [Any] {
      return values.compactMap { intValue($0) }
    }
    return [0, 1, 2, 3]
  }
}
