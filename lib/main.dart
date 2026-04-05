import 'dart:io';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_acrylic/flutter_acrylic.dart';
import 'package:file_picker/file_picker.dart';
import 'actions/action_registry.dart';
import 'actions/player_action.dart';
import 'actions/test_runner.dart';
import 'app_log.dart';
import 'video_renderer_controller.dart';

Future<Color> getWindowsAccentColor() async {
  try {
    final result = await Process.run('powershell', [
      '-Command',
      "(Get-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows\\DWM' -Name 'AccentColor').AccentColor"
    ]);
    final value = int.parse(result.stdout.trim());
    final r = value & 0xFF;
    final g = (value >> 8) & 0xFF;
    final b = (value >> 16) & 0xFF;
    return Color.fromARGB(255, r, g, b);
  } catch (_) {
    return const Color(0xFF0078D4);
  }
}

void main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();

  // Initialize logging (parses --log-level from args).
  await initLogging(args);

  await Window.initialize();
  await Window.setEffect(
    effect: WindowEffect.mica,
    color: const Color(0xCC222222),
  );
  final accentColor = await getWindowsAccentColor();
  log.info('Application starting');

  // --test-script mode: run scripted tests without UI interaction.
  final scriptIdx = args.indexOf('--test-script');
  if (scriptIdx >= 0 && scriptIdx + 1 < args.length) {
    final scriptPath = args[scriptIdx + 1];
    final controller = VideoRendererController();
    runApp(TestRunnerApp(scriptPath: scriptPath, controller: controller));
  } else {
    runApp(MyApp(accentColor: accentColor));
  }
}

/// Minimal app wrapper for test script mode.
class TestRunnerApp extends StatefulWidget {
  final String scriptPath;
  final VideoRendererController controller;

  const TestRunnerApp({
    super.key,
    required this.scriptPath,
    required this.controller,
  });

  @override
  State<TestRunnerApp> createState() => _TestRunnerAppState();
}

class _TestRunnerAppState extends State<TestRunnerApp> {
  @override
  void initState() {
    super.initState();
    // Run test script after first frame is rendered.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      TestRunner(
        scriptPath: widget.scriptPath,
        controller: widget.controller,
      ).run();
    });
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(body: Container()), // Minimal shell
    );
  }
}

class MyApp extends StatelessWidget {
  final Color accentColor;
  const MyApp({super.key, required this.accentColor});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Void Player',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.light,
        ),
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: accentColor,
          brightness: Brightness.dark,
        ),
      ),
      themeMode: ThemeMode.system,
      home: const ActionFocus(child: VideoPlayerPage()),
    );
  }
}

class VideoPlayerPage extends StatefulWidget {
  const VideoPlayerPage({super.key});

  @override
  State<VideoPlayerPage> createState() => _VideoPlayerPageState();
}

class _VideoPlayerPageState extends State<VideoPlayerPage>
    with SingleTickerProviderStateMixin {
  final VideoRendererController _controller = VideoRendererController();
  int? _textureId;
  bool _loading = false;
  String? _error;
  bool _isPlaying = false;
  bool _hasStarted = false;

  // Layout state for zoom/pan/split
  LayoutState _layout = const LayoutState();
  bool _panning = false;
  bool _splitting = false;
  Offset _lastMousePos = Offset.zero;
  bool _layoutDirty = false;
  late final Ticker _layoutTicker;

  @override
  void initState() {
    super.initState();
    _bindActions();
    _layoutTicker = createTicker(_onLayoutTick);
  }

  @override
  void dispose() {
    _layoutTicker.dispose();
    _unbindActions();
    _controller.dispose();
    super.dispose();
  }

  void _bindActions() {
    actionRegistry.bind(const TogglePlayPause(), _togglePlayPause);
    actionRegistry.bind(const StepForward(), () => _controller.stepForward());
    actionRegistry.bind(const StepBackward(), () => _controller.stepBackward());
    actionRegistry.bind(const OpenFile(), _openFile);
  }

  void _unbindActions() {
    actionRegistry.unbind(const TogglePlayPause().name);
    actionRegistry.unbind(const StepForward().name);
    actionRegistry.unbind(const StepBackward().name);
    actionRegistry.unbind(const OpenFile().name);
  }

  /// vsync-aligned tick: send layout to native once per display refresh.
  void _onLayoutTick(Duration elapsed) {
    if (_layoutDirty) {
      _layoutDirty = false;
      _controller.applyLayout(_layout);
    }
    // Stop ticker when idle — no point spinning vsync for nothing.
    if (!_layoutDirty) _layoutTicker.stop();
  }

  /// Mark layout dirty and ensure ticker is running.
  void _scheduleLayoutSync() {
    _layoutDirty = true;
    if (!_layoutTicker.isActive) _layoutTicker.start();
  }

  void _togglePlayPause() {
    if (_isPlaying) {
      _controller.pause();
    } else if (_hasStarted) {
      _controller.resume();
    } else {
      _controller.play();
      _hasStarted = true;
    }
    setState(() { _isPlaying = !_isPlaying; });
  }

  Future<void> _openFile() async {
    setState(() {
      _loading = true;
      _error = null;
    });

    try {
      final result = await FilePicker.platform.pickFiles(
        type: FileType.video,
        allowMultiple: true,
      );
      if (result == null || result.files.isEmpty) {
        setState(() { _loading = false; });
        return;
      }
      final paths = result.files
          .where((f) => f.path != null)
          .map((f) => f.path!)
          .toList();
      _textureId = await _controller.createRenderer(paths);
      setState(() {
        _loading = false;
        _hasStarted = false;
        _isPlaying = false;
      });
    } catch (e) {
      log.warning('Failed to open file: $e');
      setState(() {
        _loading = false;
        _error = e.toString();
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
        title: const Text('Void Player'),
        actions: [
          IconButton(
            icon: const Icon(Icons.folder_open),
            tooltip: 'Open Video',
            onPressed: () => actionRegistry.execute(const OpenFile().name),
          ),
        ],
      ),
      body: Center(
        child: _buildBody(),
      ),
      floatingActionButton: _textureId != null
          ? FloatingActionButton(
              heroTag: 'toggle',
              onPressed: () => actionRegistry.execute(const TogglePlayPause().name),
              child: Icon(_isPlaying ? Icons.pause : Icons.play_arrow),
            )
          : null,
    );
  }

  Widget _buildBody() {
    if (_loading) {
      return const CircularProgressIndicator();
    }
    if (_error != null) {
      return Text('Error: $_error',
          style: const TextStyle(color: Colors.red));
    }
    if (_textureId != null) {
      return Builder(builder: (context) {
        return Listener(
          onPointerDown: (e) {
            if ((e.buttons & kPrimaryButton) != 0) {
              _panning = true;
              _lastMousePos = e.position;
            } else if ((e.buttons & kSecondaryButton) != 0) {
              _splitting = true;
              _lastMousePos = e.position;
            }
          },
          onPointerUp: (e) {
            if (_panning) _panning = false;
            if (_splitting) _splitting = false;
          },
          onPointerMove: (e) {
            if (!_panning && !_splitting) return;
            final delta = e.position - _lastMousePos;
            _lastMousePos = e.position;

            if (_panning) {
              final sensitivity = 1.0 / _layout.zoomRatio.clamp(1.0, 10.0);
              _layout = _layout.copyWith(
                viewOffsetX: _layout.viewOffsetX + delta.dx * sensitivity,
                viewOffsetY: _layout.viewOffsetY + delta.dy * sensitivity,
              );
            }

            if (_splitting && _layout.mode == LayoutMode.splitScreen) {
              final box = context.findRenderObject() as RenderBox;
              final localX = e.localPosition.dx;
              _layout = _layout.copyWith(
                splitPos: (localX / box.size.width).clamp(0.0, 1.0),
              );
            }

            _scheduleLayoutSync();
          },
          onPointerSignal: (e) {
            if (e is PointerScrollEvent) {
              final scrollDelta = e.scrollDelta.dy;
              if (scrollDelta < 0) {
                _layout = _layout.copyWith(
                  zoomRatio: (_layout.zoomRatio * 1.1).clamp(1.0, 10.0),
                );
              } else if (scrollDelta > 0) {
                _layout = _layout.copyWith(
                  zoomRatio: (_layout.zoomRatio / 1.1).clamp(1.0, 10.0),
                );
              }
              _scheduleLayoutSync();
            }
          },
          child: Texture(textureId: _textureId!),
        );
      });
    }
    return const Text('Open a video file to begin');
  }
}
