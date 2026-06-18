import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/analysis_overlay_chunk_scheduler.dart';

AnalysisOverlayChunkRequest _request(
  int startFrame,
  int endFrame, {
  String hash = 'hash',
}) {
  return AnalysisOverlayChunkRequest(
    hash: hash,
    videoPath: 'video.mp4',
    startFrame: startFrame,
    endFrame: endFrame,
    targetFrame: startFrame,
  );
}

void main() {
  test('deduplicates overlay chunk jobs by hash and frame range', () async {
    var runCount = 0;
    final completer = Completer<bool>();
    final scheduler = AnalysisOverlayChunkScheduler();
    final request = _request(0, 63);

    final first = scheduler.schedule(
      request: request,
      priority: 10,
      run: () {
        runCount++;
        return completer.future;
      },
      overlaySerial: 1,
    );
    final second = scheduler.schedule(
      request: request,
      priority: 0,
      run: () {
        runCount++;
        return Future.value(false);
      },
      overlaySerial: 2,
    );

    completer.complete(true);

    expect(await first, isTrue);
    expect(await second, isTrue);
    expect(runCount, 1);
  });

  test('drops the lowest priority pending job under backpressure', () async {
    final running = Completer<bool>();
    final runOrder = <String>[];
    final scheduler = AnalysisOverlayChunkScheduler(
      maxInFlightSubmissions: 1,
      maxQueuedJobs: 1,
    );

    final first = scheduler.schedule(
      request: _request(0, 63),
      priority: 0,
      run: () {
        runOrder.add('first');
        return running.future;
      },
    );
    final dropped = scheduler.schedule(
      request: _request(64, 127),
      priority: 10,
      run: () {
        runOrder.add('dropped');
        return Future.value(true);
      },
    );
    final kept = scheduler.schedule(
      request: _request(128, 191),
      priority: 0,
      run: () {
        runOrder.add('kept');
        return Future.value(true);
      },
    );

    expect(await dropped, isFalse);
    running.complete(true);

    expect(await first, isTrue);
    expect(await kept, isTrue);
    expect(runOrder, ['first', 'kept']);
  });

  test('reports completion with overlay serial', () async {
    AnalysisOverlayChunkJobResult? result;
    final scheduler = AnalysisOverlayChunkScheduler(
      onComplete: (value) => result = value,
    );

    final request = _request(0, 63, hash: 'h1');
    final ok = await scheduler.schedule(
      request: request,
      priority: 0,
      overlaySerial: 42,
      run: () => Future.value(true),
    );

    expect(ok, isTrue);
    expect(result?.request, same(request));
    expect(result?.ok, isTrue);
    expect(result?.overlaySerial, 42);
  });

  test('runs jobs concurrently up to the worker limit', () async {
    final first = Completer<bool>();
    final second = Completer<bool>();
    var active = 0;
    var maxActive = 0;
    Future<bool> tracked(Completer<bool> completer) async {
      active++;
      maxActive = active > maxActive ? active : maxActive;
      try {
        return await completer.future;
      } finally {
        active--;
      }
    }

    final scheduler = AnalysisOverlayChunkScheduler(maxInFlightSubmissions: 2);
    final a = scheduler.schedule(
      request: _request(0, 63, hash: 'a'),
      priority: 0,
      run: () => tracked(first),
    );
    final b = scheduler.schedule(
      request: _request(64, 127, hash: 'b'),
      priority: 0,
      run: () => tracked(second),
    );

    await Future<void>.delayed(Duration.zero);

    expect(maxActive, 2);
    first.complete(true);
    second.complete(true);
    expect(await Future.wait([a, b]), [true, true]);
  });
}
