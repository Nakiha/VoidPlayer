import 'dart:io';
import 'package:flutter/material.dart';
import 'package:flutter_acrylic/flutter_acrylic.dart';
import 'package:file_picker/file_picker.dart';
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

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Window.initialize();
  await Window.setEffect(
    effect: WindowEffect.mica,
    color: const Color(0xCC222222),
  );
  final accentColor = await getWindowsAccentColor();
  runApp(MyApp(accentColor: accentColor));
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
      home: const VideoPlayerPage(),
    );
  }
}

class VideoPlayerPage extends StatefulWidget {
  const VideoPlayerPage({super.key});

  @override
  State<VideoPlayerPage> createState() => _VideoPlayerPageState();
}

class _VideoPlayerPageState extends State<VideoPlayerPage> {
  final VideoRendererController _controller = VideoRendererController();
  int? _textureId;
  bool _loading = false;
  String? _error;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
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
      });
    } catch (e) {
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
            onPressed: _openFile,
          ),
        ],
      ),
      body: Center(
        child: _buildBody(),
      ),
      floatingActionButton: _textureId != null
          ? Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                FloatingActionButton(
                  heroTag: 'play',
                  onPressed: () => _controller.play(),
                  child: const Icon(Icons.play_arrow),
                ),
                const SizedBox(width: 8),
                FloatingActionButton(
                  heroTag: 'pause',
                  onPressed: () => _controller.pause(),
                  child: const Icon(Icons.pause),
                ),
              ],
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
      return Texture(textureId: _textureId!);
    }
    return const Text('Open a video file to begin');
  }
}
