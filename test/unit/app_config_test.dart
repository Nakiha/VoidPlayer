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
}
