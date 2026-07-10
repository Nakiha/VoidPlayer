import 'dart:collection';
import 'dart:io';

import '../app_log.dart';

class ViewportInteractionDiagnostics {
  static final ViewportInteractionDiagnostics instance =
      ViewportInteractionDiagnostics._();

  static const _metricNames = <String, String>{
    'pointerMovePanDispatch': 'PointerMovePanDispatch',
    'pointerPanZoomUpdate': 'PointerPanZoomUpdate',
    'pointerPanZoomPanDispatch': 'PointerPanZoomPanDispatch',
    'pointerPanZoomScaleDispatch': 'PointerPanZoomScaleDispatch',
    'viewportActionPan': 'ViewportActionPan',
    'viewportActionZoom': 'ViewportActionZoom',
    'layoutPan': 'LayoutPan',
    'layoutZoom': 'LayoutZoom',
  };

  final Map<String, _RateCounter> _counters = {
    for (final name in _metricNames.keys) name: _RateCounter(),
  };
  DateTime? _lastLogAt;

  ViewportInteractionDiagnostics._();

  void reset() {
    for (final counter in _counters.values) {
      counter.reset();
    }
    _lastLogAt = null;
  }

  void record(String name) {
    (_counters[name] ??= _RateCounter()).record();
    _maybeLog();
  }

  Map<String, Object> snapshot() {
    final result = <String, Object>{};
    for (final entry in _metricNames.entries) {
      final counter = _counters[entry.key] ?? _RateCounter();
      result['dartViewport${entry.value}Count'] = counter.total;
      result['dartViewport${entry.value}Hz'] = counter.rateHz();
      result['dartViewport${entry.value}HzX1000'] = (counter.rateHz() * 1000)
          .round();
    }
    return result;
  }

  void _maybeLog() {
    if (Platform.environment['VOIDPLAYER_MACOS_PROFILER'] != '1') return;
    final now = DateTime.now();
    final last = _lastLogAt;
    if (last != null &&
        now.difference(last) < const Duration(milliseconds: 500)) {
      return;
    }
    _lastLogAt = now;
    final values = snapshot();
    String count(String key) => '${values['dartViewport${key}Count'] ?? 0}';
    String hz(String key) {
      final value = values['dartViewport${key}Hz'];
      return value is num ? value.toStringAsFixed(1) : '0.0';
    }

    log.info(
      '[ViewportInteractionDiagnostics] '
      'rawPanZoom=${count('PointerPanZoomUpdate')}@${hz('PointerPanZoomUpdate')}Hz '
      'panZoomPan=${count('PointerPanZoomPanDispatch')}@${hz('PointerPanZoomPanDispatch')}Hz '
      'panZoomScale=${count('PointerPanZoomScaleDispatch')}@${hz('PointerPanZoomScaleDispatch')}Hz '
      'mousePan=${count('PointerMovePanDispatch')}@${hz('PointerMovePanDispatch')}Hz '
      'actionPan=${count('ViewportActionPan')}@${hz('ViewportActionPan')}Hz '
      'actionZoom=${count('ViewportActionZoom')}@${hz('ViewportActionZoom')}Hz '
      'layoutPan=${count('LayoutPan')}@${hz('LayoutPan')}Hz '
      'layoutZoom=${count('LayoutZoom')}@${hz('LayoutZoom')}Hz',
    );
  }
}

class _RateCounter {
  static const int _windowUs = 1000000;
  static const int _capacity = 512;

  final Queue<int> _samples = Queue<int>();
  int total = 0;

  void reset() {
    _samples.clear();
    total = 0;
  }

  void record() {
    final nowUs = DateTime.now().microsecondsSinceEpoch;
    total++;
    _samples.addLast(nowUs);
    _prune(nowUs);
    while (_samples.length > _capacity) {
      _samples.removeFirst();
    }
  }

  double rateHz() {
    final nowUs = DateTime.now().microsecondsSinceEpoch;
    _prune(nowUs);
    if (_samples.length <= 1) return 0.0;
    final first = _samples.first;
    final last = _samples.last;
    if (last <= first) return 0.0;
    return (_samples.length - 1) * 1000000.0 / (last - first);
  }

  void _prune(int nowUs) {
    final cutoff = nowUs - _windowUs;
    while (_samples.isNotEmpty && _samples.first < cutoff) {
      _samples.removeFirst();
    }
  }
}
