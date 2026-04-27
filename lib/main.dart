import 'package:flutter/widgets.dart';

import 'app_log.dart';
import 'windows/app_bootstrap.dart';

void main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  await initLogging(args);
  await runVoidPlayer(args);
}
