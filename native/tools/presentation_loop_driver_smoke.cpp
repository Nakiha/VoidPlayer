#include "renderer/render/presentation_loop_driver.h"
#include "renderer/sync/render_sink.h"

#include <cstdio>
#include <memory>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

vr::TextureFrame make_frame(int64_t pts_us) {
  vr::TextureFrame frame;
  frame.width = 320;
  frame.height = 180;
  frame.pts_us = pts_us;
  frame.duration_us = 33'333;
  return frame;
}

vr::TextureFrame make_frame_with_duration(int64_t pts_us, int64_t duration_us) {
  auto frame = make_frame(pts_us);
  frame.duration_us = duration_us;
  return frame;
}

}  // namespace

int main() {
  if (vr::should_publish_presentation_frame_callback({false, false, false})) {
    return fail("presentation callback policy published without scheduler notification");
  }
  if (!vr::should_publish_presentation_frame_callback({true, false, false})) {
    return fail("presentation callback policy suppressed the legacy no-target callback");
  }
  if (!vr::should_publish_presentation_frame_callback({true, true, true})) {
    return fail("presentation callback policy suppressed a renderer-owned upload");
  }
  if (vr::should_publish_presentation_frame_callback({true, true, false})) {
    return fail("presentation callback policy published a failed renderer-owned upload");
  }

  int64_t now_us = 0;
  vr::Clock clock([&now_us]() { return now_us; });
  clock.seek(0);
  clock.resume();

  auto track = std::make_shared<vr::TrackBuffer>();
  track->push_frame(make_frame(0));
  track->push_frame(make_frame(33'333));
  track->set_state(vr::TrackState::Ready);

  vr::RenderSink render_sink(clock);
  render_sink.set_track(0, track, 42, 7);

  vr::PresentationLoopDriver driver;
  auto tick = driver.tick(
      render_sink,
      true,
      clock.current_pts_us(),
      clock.speed(),
      33'333,
      std::chrono::microseconds(8'000));
  if (!tick.scheduler.should_notify ||
      !tick.scheduler.has_presentable_frame ||
      tick.scheduler.selected_pts_us != 0 ||
      tick.next_sleep.count() != 8'000) {
    return fail("first presentation tick did not publish the selected frame");
  }

  tick = driver.tick(
      render_sink,
      true,
      clock.current_pts_us(),
      clock.speed(),
      33'333,
      std::chrono::microseconds(8'000));
  if (tick.scheduler.should_notify ||
      !tick.scheduler.has_presentable_frame) {
    return fail("duplicate presentation tick was not coalesced");
  }

  auto stats = driver.stats();
  if (stats.tick_count != 2 ||
      stats.presentable_tick_count != 2 ||
      stats.frame_notification_count != 1 ||
      stats.deadline_sleep_count != 2 ||
      stats.last_present_frame_count != 1 ||
      !stats.cached_present_decision_available) {
    return fail("presentation loop stats did not match tick behavior");
  }

  auto cached = driver.current_present_decision(&render_sink);
  if (!cached.should_present ||
      !cached.frames[0].has_value() ||
      cached.file_ids[0] != 42 ||
      cached.track_generations[0] != 7) {
    return fail("cached present decision did not preserve track identity");
  }

  vr::PresentDecision manual;
  manual.should_present = true;
  manual.current_pts_us = 33'333;
  manual.frames[0] = make_frame(33'333);
  manual.file_ids[0] = 84;
  manual.track_generations[0] = 8;
  driver.publish_present_decision(manual);
  cached = driver.current_present_decision(&render_sink);
  stats = driver.stats();
  if (!cached.should_present ||
      cached.file_ids[0] != 84 ||
      stats.last_selected_pts_us != 33'333 ||
      stats.last_present_frame_count != 1 ||
      !stats.cached_present_decision_available) {
    return fail("manual presentation decision publication did not update cache");
  }

  driver.reset_presentation_state();
  stats = driver.stats();
  if (stats.tick_count != 2 ||
      stats.frame_notification_count != 1 ||
      stats.cached_present_decision_available ||
      stats.last_selected_pts_us != vr::kNoTimestampUs) {
    return fail("presentation state reset changed counters or kept stale state");
  }

  now_us = 40'000;
  tick = driver.tick(
      render_sink,
      true,
      clock.current_pts_us(),
      clock.speed(),
      66'666,
      std::chrono::microseconds(8'000));
  if (!tick.scheduler.should_notify ||
      tick.scheduler.selected_pts_us != 33'333) {
    return fail("presentation loop did not advance to the next frame");
  }

  now_us = 0;
  clock.seek(0);
  auto primary = std::make_shared<vr::TrackBuffer>();
  primary->push_frame(make_frame_with_duration(0, 100'000));
  primary->set_state(vr::TrackState::Ready);
  auto secondary = std::make_shared<vr::TrackBuffer>();
  secondary->push_frame(make_frame(0));
  secondary->push_frame(make_frame(33'333));
  secondary->set_state(vr::TrackState::Ready);

  vr::RenderSink multitrack_sink(clock);
  multitrack_sink.set_track(0, primary, 1, 1);
  multitrack_sink.set_track(1, secondary, 2, 1);

  vr::PresentationLoopDriver multitrack_driver;
  tick = multitrack_driver.tick(
      multitrack_sink,
      true,
      clock.current_pts_us(),
      clock.speed(),
      33'333,
      std::chrono::microseconds(8'000));
  if (!tick.scheduler.should_notify) {
    return fail("multitrack first presentation tick did not publish");
  }
  now_us = 40'000;
  tick = multitrack_driver.tick(
      multitrack_sink,
      true,
      clock.current_pts_us(),
      clock.speed(),
      66'666,
      std::chrono::microseconds(8'000));
  if (tick.scheduler.should_notify ||
      !tick.scheduler.decision.frames[0].has_value() ||
      !tick.scheduler.decision.frames[1].has_value() ||
      tick.scheduler.decision.frames[0]->pts_us != 0 ||
      tick.scheduler.decision.frames[1]->pts_us != 33'333) {
    return fail("multitrack presentation tick published from a secondary-track-only frame");
  }

  driver.reset();
  stats = driver.stats();
  if (stats.tick_count != 0 ||
      stats.last_selected_pts_us != vr::kNoTimestampUs ||
      stats.cached_present_decision_available) {
    return fail("full reset did not clear presentation loop stats");
  }

  return 0;
}
