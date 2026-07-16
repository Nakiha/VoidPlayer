import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/automation/automation_probe.dart';

void main() {
  test('native seek probe counts one shared renderer request per seek', () {
    const log = '''
[info] [Renderer] seek_internal: target=1.800s, type=Exact
[info] [Renderer] seek_internal: track[0] cleared, target=1.800s
[info] [DemuxThread] Executing seek: target=1.800s, type=Exact
[info] [Renderer] seek_internal: target=4.200s, type=Exact
[info] [Renderer] seek_internal: track[0] cleared, target=4.200s
''';

    expect(countNativeSeekRequests(log), 2);
  });

  test('obsolete plugin marker is not mistaken for a current seek', () {
    const log = '[VideoRendererPlugin] seek: target=1.800s';

    expect(countNativeSeekRequests(log), 0);
  });
}
