import '../platform/native_file_picker.dart';

class WindowsNativeFilePicker {
  WindowsNativeFilePicker._();

  static const NativeFilePicker instance = MethodChannelNativeFilePicker();

  /// Open the Windows native file picker dialog (IFileDialog).
  /// Returns null if the user cancels.
  static Future<List<String>?> pickFiles({bool allowMultiple = true}) async {
    return instance.pickFiles(allowMultiple: allowMultiple);
  }
}
