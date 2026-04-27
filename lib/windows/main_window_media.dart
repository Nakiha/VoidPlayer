part of 'main_window.dart';

extension _MainWindowMediaLoading on _MainWindowState {
  /// Load media files by paths (shared by file picker, drag-drop, and test scripts).
  void _loadMediaPaths(List<String> paths) async {
    if (paths.isEmpty) return;

    if (_textureId == null) {
      _setViewportState(0);
      try {
        final initialWidth = _viewportWidth > 0 ? _viewportWidth : 1920;
        final initialHeight = _viewportHeight > 0 ? _viewportHeight : 1080;
        final res = await _controller.createRenderer(
          paths,
          width: initialWidth,
          height: initialHeight,
        );
        _setTextureId(res.textureId);
        _trackManager.setTracks(res.tracks);
        _layout = await _controller.getLayout();
        _applyStartupLoopRangeIfReady();
        await WidgetsBinding.instance.endOfFrame;
        if (!mounted) return;
        if (_viewportWidth > 0 && _viewportHeight > 0) {
          await _controller.resize(_viewportWidth, _viewportHeight);
        }
        if (!mounted) return;
        _setViewportState(2);
      } catch (e) {
        log.severe("createRenderer failed: $e");
        _setViewportState(1);
      }
    } else {
      for (final path in paths) {
        try {
          final track = await _controller.addTrack(path);
          _trackManager.addTrack(track);
          _applyStartupLoopRangeIfReady();
        } catch (e) {
          log.severe("addTrack failed: $e");
        }
      }
    }
  }

  /// Add media by path (used by test scripts, bypasses file picker).
  void _addMediaByPath(String path) {
    if (path.isEmpty) return;
    _loadMediaPaths([path]);
  }

  void _openFile() async {
    final paths = await WindowsNativeFilePicker.pickFiles(allowMultiple: true);
    if (paths == null || paths.isEmpty) return;
    _loadMediaPaths(paths);
  }
}
