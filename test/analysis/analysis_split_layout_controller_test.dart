import 'package:flutter_test/flutter_test.dart';
import 'package:void_player/analysis/ui/widgets/analysis_split_layout_controller.dart';

void main() {
  test('split layout publishes shared chart and view state', () {
    final controller = AnalysisSplitLayoutController();
    addTearDown(controller.dispose);
    var notifications = 0;
    controller.addListener(() => notifications++);

    controller.setChartWindow(
      sourceFileId: 1,
      offset: 24,
      visibleFrameCount: 48,
    );
    expect(controller.chartWindowRevision, 1);
    expect(controller.chartWindowSourceFileId, 1);
    expect(controller.chartOffset, 24);
    expect(controller.visibleFrameCount, 48);

    controller.setViewState(sourceFileId: 1, selectedTab: 1, ptsOrder: false);
    expect(controller.viewStateRevision, 1);
    expect(controller.viewStateSourceFileId, 1);
    expect(controller.selectedTab, 1);
    expect(controller.ptsOrder, isFalse);
    expect(notifications, 2);

    controller.setChartWindow(
      sourceFileId: 1,
      offset: 24,
      visibleFrameCount: 48,
    );
    controller.setViewState(sourceFileId: 1, selectedTab: 1, ptsOrder: false);
    expect(notifications, 2);
  });
}
