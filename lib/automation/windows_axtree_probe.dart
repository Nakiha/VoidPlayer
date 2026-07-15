import 'dart:convert';
import 'dart:io';

import '../app_log.dart';

/// Uses a real out-of-process Windows accessibility client to activate and
/// inspect Flutter's platform AXTree through the locked engine's MSAA provider.
///
/// Widget semantics tests validate the intended Dart tree. This probe covers
/// the separate Flutter-engine/Windows bridge and keeps semantics enabled while
/// subsequent native hover actions exercise tooltip updates.
Future<void> assertWindowsAxTreeNames(List<String> requiredNames) async {
  if (!Platform.isWindows) return;
  if (requiredNames.isEmpty) {
    throw ArgumentError.value(
      requiredNames,
      'requiredNames',
      'must not be empty',
    );
  }

  final script = _findProbeScript();
  final encodedNameGroups = requiredNames
      .map((name) => (_windowsAxTreeNameAliases[name] ?? [name]).join(';'))
      .join('|');
  final result = await Process.run(
    'powershell.exe',
    [
      '-NoLogo',
      '-NoProfile',
      '-NonInteractive',
      '-ExecutionPolicy',
      'Bypass',
      '-File',
      script.path,
      '-TargetProcessId',
      '$pid',
      '-RequiredNames',
      encodedNameGroups,
      '-TimeoutMs',
      '5000',
    ],
    stdoutEncoding: utf8,
    stderrEncoding: utf8,
  );

  final stdoutText = (result.stdout as String).trim();
  final stderrText = (result.stderr as String).trim();
  if (result.exitCode != 0) {
    throw StateError(
      'Windows AXTree probe failed (${result.exitCode}): '
      '${stderrText.isNotEmpty ? stderrText : stdoutText}',
    );
  }
  log.info('Windows AXTree probe passed: $stdoutText');
}

const _windowsAxTreeNameAliases = <String, List<String>>{
  'playbackControls': ['Playback controls', '播放控制'],
  'zoom': ['Zoom', '缩放'],
  'enterFullScreen': ['Enter Full Screen', '进入全屏'],
  'previousFrame': ['Previous Frame', '上一帧'],
  'play': ['Play', '播放'],
  'nextFrame': ['Next Frame', '下一帧'],
  'timelineSeek': ['Timeline seek', '播放进度'],
  'showOverlayControls': ['Show bitstream overlay controls', '显示码流遮罩控制条'],
  'hideOverlayControls': ['Hide bitstream overlay controls', '隐藏码流遮罩控制条'],
  'overlayControls': ['Bitstream overlay controls', '码流遮罩控制'],
  'cuPartitions': ['CU partitions', 'CU 划分'],
  'qpHeatmap': ['QP heatmap', 'QP 热力图'],
  'bitCostHeatmap': ['Bit-cost heatmap', '编码开销热力图'],
  'overlayOpacity': ['Overlay opacity', '遮罩透明度'],
};

File _findProbeScript() {
  final starts = <Directory>[
    Directory.current,
    File(Platform.resolvedExecutable).parent,
  ];
  final visited = <String>{};
  for (final start in starts) {
    var current = start.absolute;
    while (visited.add(current.path)) {
      final candidate = File(
        '${current.path}${Platform.pathSeparator}scripts'
        '${Platform.pathSeparator}windows${Platform.pathSeparator}'
        'axtree_probe.ps1',
      );
      if (candidate.existsSync()) return candidate;
      final parent = current.parent;
      if (parent.path == current.path) break;
      current = parent;
    }
  }
  throw StateError(
    'Unable to locate scripts/windows/axtree_probe.ps1 from '
    '${Directory.current.path} or ${Platform.resolvedExecutable}',
  );
}
