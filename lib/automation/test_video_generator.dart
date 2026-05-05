import 'dart:io';

import '../app_log.dart';

Future<void> generateTestVideo({
  required String path,
  required int frames,
  required int fps,
  required int width,
  required int height,
}) async {
  if (frames <= 0 || fps <= 0 || width <= 0 || height <= 0) {
    throw ArgumentError(
      'Invalid video parameters frames=$frames fps=$fps size=${width}x$height',
    );
  }
  final output = File(path).absolute;
  await output.parent.create(recursive: true);
  final ffmpeg = _resolveFfmpegExecutable();
  final args = [
    '-hide_banner',
    '-loglevel',
    'error',
    '-y',
    '-f',
    'lavfi',
    '-i',
    'testsrc2=size=${width}x$height:rate=$fps',
    '-frames:v',
    '$frames',
    '-metadata',
    'comment=voidplayer-test-${DateTime.now().microsecondsSinceEpoch}',
    '-c:v',
    'libx264',
    '-preset',
    'ultrafast',
    '-g',
    '$fps',
    '-pix_fmt',
    'yuv420p',
    '-an',
    output.path,
  ];
  final result = await Process.run(ffmpeg, args);
  if (result.exitCode != 0) {
    throw StateError(
      'ffmpeg failed (${result.exitCode}) generating ${output.path}: '
      '${result.stderr}',
    );
  }
  final size = await output.length();
  if (size <= 0) {
    throw StateError('Generated video is empty: ${output.path}');
  }
  log.info(
    'TestRunner: generated ${output.path} '
    'frames=$frames fps=$fps bytes=$size',
  );
}

String _resolveFfmpegExecutable() {
  final bundled = File('windows/libs/ffmpeg/bin/ffmpeg.exe').absolute;
  if (bundled.existsSync()) return bundled.path;
  return 'ffmpeg';
}
