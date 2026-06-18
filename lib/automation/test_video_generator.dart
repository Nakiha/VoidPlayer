import 'dart:io';

import '../app_log.dart';

Future<void> generateTestVideo({
  required String path,
  required int frames,
  required int fps,
  required int width,
  required int height,
  int ptsOffsetUs = 0,
  bool withAudio = false,
}) async {
  if (frames <= 0 || fps <= 0 || width <= 0 || height <= 0 || ptsOffsetUs < 0) {
    throw ArgumentError(
      'Invalid video parameters frames=$frames fps=$fps '
      'size=${width}x$height ptsOffsetUs=$ptsOffsetUs',
    );
  }
  final output = File(path).absolute;
  await output.parent.create(recursive: true);
  final ffmpeg = _resolveFfmpegExecutable();
  final durationSeconds = frames / fps;
  final args = [
    '-hide_banner',
    '-loglevel',
    'error',
    '-y',
    '-f',
    'lavfi',
    '-i',
    'testsrc2=size=${width}x$height:rate=$fps',
    if (withAudio) ...[
      '-f',
      'lavfi',
      '-i',
      'sine=frequency=440:sample_rate=48000:duration=$durationSeconds',
    ],
    if (ptsOffsetUs > 0) ...[
      '-vf',
      'setpts=PTS+${ptsOffsetUs / 1000000.0}/TB',
      '-avoid_negative_ts',
      'disabled',
    ],
    '-frames:v',
    '$frames',
    if (withAudio) ...['-map', '0:v:0', '-map', '1:a:0'],
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
    if (withAudio) ...['-c:a', 'aac', '-b:a', '96k', '-shortest'] else '-an',
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
    'frames=$frames fps=$fps ptsOffsetUs=$ptsOffsetUs '
    'withAudio=$withAudio bytes=$size',
  );
}

String _resolveFfmpegExecutable() {
  final bundled = File(
    '.toolchains/ffmpeg/windows-x64/bin/ffmpeg.exe',
  ).absolute;
  if (bundled.existsSync()) return bundled.path;
  return 'ffmpeg';
}
