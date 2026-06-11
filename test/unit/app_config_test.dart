import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/config/app_config.dart';

void main() {
  test('migrateData adds current schema version to legacy config', () {
    final migrated = AppConfig.migrateData({
      'window': {'width': 1280, 'height': 720},
    });

    expect(migrated['schemaVersion'], AppConfig.currentSchemaVersion);
    expect(migrated['window'], {'width': 1280, 'height': 720});
  });

  test('migrateData replaces invalid schema version', () {
    final migrated = AppConfig.migrateData({'schemaVersion': 'old'});

    expect(migrated['schemaVersion'], AppConfig.currentSchemaVersion);
  });

  test('migrateData does not downgrade future schema versions', () {
    final migrated = AppConfig.migrateData({'schemaVersion': 99});

    expect(migrated['schemaVersion'], 99);
  });

  test('migrateData normalizes known map sections', () {
    final migrated = AppConfig.migrateData({
      'preferences': {'themeMode': 'dark', 42: 'ignored'},
      'macos': {
        'securityScopedBookmarks': {'/tmp/video.mp4': 'bookmark'},
      },
      'shortcuts': {'playPause': 'Space'},
    });

    expect(migrated['preferences'], {'themeMode': 'dark'});
    expect(migrated['macos'], {
      'securityScopedBookmarks': {'/tmp/video.mp4': 'bookmark'},
    });
    expect(migrated['shortcuts'], {'playPause': 'Space'});
  });

  test('migrateData removes malformed known sections', () {
    final migrated = AppConfig.migrateData({
      'window': 'old-window-shape',
      'preferences': ['dark'],
      'customRoot': {'left': 'alone'},
    });

    expect(migrated.containsKey('window'), isFalse);
    expect(migrated.containsKey('preferences'), isFalse);
    expect(migrated['customRoot'], {'left': 'alone'});
  });
}
