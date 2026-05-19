import 'dart:io';

import 'package:flutter/widgets.dart';

import 'app_log.dart';
import 'macos/app_bootstrap.dart' deferred as macos_bootstrap;
import 'windows/app_bootstrap.dart' deferred as windows_bootstrap;

void main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  await initLogging(args);
  if (Platform.isWindows) {
    await windows_bootstrap.loadLibrary();
    await windows_bootstrap.runVoidPlayer(args);
  } else if (Platform.isMacOS) {
    await macos_bootstrap.loadLibrary();
    await macos_bootstrap.runMacOSVoidPlayer(args);
  } else {
    log.severe('Unsupported platform: ${Platform.operatingSystem}');
    exit(2);
  }
}
