import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

class AnalysisOverlayChunkRequest {
  final String hash;
  final String videoPath;
  final int startFrame;
  final int endFrame;
  final int targetFrame;

  const AnalysisOverlayChunkRequest({
    required this.hash,
    required this.videoPath,
    required this.startFrame,
    required this.endFrame,
    required this.targetFrame,
  });

  String get key => '$hash:$startFrame:$endFrame';
}

class AnalysisOverlayChunkJobResult {
  final AnalysisOverlayChunkRequest request;
  final bool ok;
  final int? overlaySerial;

  const AnalysisOverlayChunkJobResult({
    required this.request,
    required this.ok,
    required this.overlaySerial,
  });
}

class _AnalysisOverlayChunkJob {
  final AnalysisOverlayChunkRequest request;
  final Future<bool> Function() run;
  final Completer<bool> completer = Completer<bool>();
  int priority;
  int? overlaySerial;
  final int sequence;

  _AnalysisOverlayChunkJob({
    required this.request,
    required this.run,
    required this.priority,
    required this.overlaySerial,
    required this.sequence,
  });
}

class AnalysisOverlayChunkScheduler {
  final int maxWorkers;
  final int maxQueuedJobs;
  final void Function(AnalysisOverlayChunkJobResult result)? onComplete;
  final void Function(String message, [Object? error, StackTrace? stackTrace])?
  onLog;

  final Map<String, _AnalysisOverlayChunkJob> _jobsByKey = {};
  final List<_AnalysisOverlayChunkJob> _pendingJobs = [];
  int _jobSequence = 0;
  int _activeWorkers = 0;
  int _backpressureDropCount = 0;

  AnalysisOverlayChunkScheduler({
    this.maxWorkers = 1,
    this.maxQueuedJobs = 48,
    this.onComplete,
    this.onLog,
  }) : assert(maxWorkers > 0),
       assert(maxQueuedJobs > 0);

  int get activeWorkers => _activeWorkers;
  int get pendingJobs => _pendingJobs.length;
  int get trackedJobs => _jobsByKey.length;
  int get backpressureDropCount => _backpressureDropCount;

  static int defaultNativeSubmissionWorkers() {
    final processors = math.max(1, Platform.numberOfProcessors);
    final fallback = math.min(math.max(8, processors), 32);
    final value = int.tryParse(
      Platform.environment['VOIDPLAYER_ANALYSIS_SUBMISSIONS'] ?? '',
    );
    return (value ?? fallback).clamp(1, math.max(1, processors)).toInt();
  }

  Future<bool> schedule({
    required AnalysisOverlayChunkRequest request,
    required int priority,
    required Future<bool> Function() run,
    int? overlaySerial,
  }) {
    final existing = _jobsByKey[request.key];
    if (existing != null) {
      if (priority < existing.priority) existing.priority = priority;
      existing.overlaySerial = overlaySerial ?? existing.overlaySerial;
      _sortPending();
      return existing.completer.future;
    }

    final job = _AnalysisOverlayChunkJob(
      request: request,
      run: run,
      priority: priority,
      overlaySerial: overlaySerial,
      sequence: ++_jobSequence,
    );
    _jobsByKey[request.key] = job;
    _pendingJobs.add(job);
    _sortPending();
    _trimPending();
    _pump();
    return job.completer.future;
  }

  void clear() {
    for (final job in _jobsByKey.values.toList(growable: false)) {
      if (!job.completer.isCompleted) {
        job.completer.complete(false);
      }
    }
    _jobsByKey.clear();
    _pendingJobs.clear();
  }

  void _sortPending() {
    _pendingJobs.sort((a, b) {
      final priority = a.priority.compareTo(b.priority);
      if (priority != 0) return priority;
      return a.sequence.compareTo(b.sequence);
    });
  }

  void _trimPending() {
    _sortPending();
    while (_pendingJobs.length > maxQueuedJobs) {
      final dropped = _pendingJobs.removeLast();
      _jobsByKey.remove(dropped.request.key);
      _backpressureDropCount++;
      if (!dropped.completer.isCompleted) {
        dropped.completer.complete(false);
      }
      onLog?.call(
        '[Analysis] dropped queued overlay chunk due to backpressure: '
        '${dropped.request.hash} '
        'frames=${dropped.request.startFrame}..${dropped.request.endFrame}',
      );
    }
  }

  void _pump() {
    while (_activeWorkers < maxWorkers && _pendingJobs.isNotEmpty) {
      final job = _pendingJobs.removeAt(0);
      _activeWorkers++;
      unawaited(_runJob(job));
    }
  }

  Future<void> _runJob(_AnalysisOverlayChunkJob job) async {
    var ok = false;
    try {
      ok = await job.run();
    } catch (e, stack) {
      onLog?.call(
        '[Analysis] overlay chunk job failed: '
        '${job.request.hash} '
        'frames=${job.request.startFrame}..${job.request.endFrame}: $e',
        e,
        stack,
      );
    } finally {
      if (identical(_jobsByKey[job.request.key], job)) {
        _jobsByKey.remove(job.request.key);
      }
      if (!job.completer.isCompleted) {
        job.completer.complete(ok);
      }
      if (_activeWorkers > 0) {
        _activeWorkers--;
      }
      onComplete?.call(
        AnalysisOverlayChunkJobResult(
          request: job.request,
          ok: ok,
          overlaySerial: job.overlaySerial,
        ),
      );
      _pump();
    }
  }
}
