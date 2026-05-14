class AppDependencyMetadata {
  final String name;
  final String license;

  const AppDependencyMetadata(this.name, this.license);
}

class AppMetadata {
  static const version = '1.0.3';
  static const license = 'GPLv3';

  static const dependencies = [
    AppDependencyMetadata('Flutter / Dart SDK', 'BSD-3-Clause'),
    AppDependencyMetadata('FFmpeg 8.1 full build (gyan.dev)', 'GPL-3.0'),
    AppDependencyMetadata(
      'Windows SDK: Direct3D 11 / DXGI / DWM',
      'Microsoft SDK',
    ),
    AppDependencyMetadata('spdlog', 'MIT'),
    AppDependencyMetadata('flutter_acrylic', 'MIT'),
    AppDependencyMetadata('window_manager', 'MIT'),
    AppDependencyMetadata('screen_retriever', 'MIT'),
    AppDependencyMetadata('desktop_drop', 'Apache-2.0'),
    AppDependencyMetadata(
      'Dart packages: crypto, ffi, intl, logging, path',
      'BSD-3-Clause',
    ),
  ];
}
