#include "video_renderer/track/track_present_policy.h"

#include <cstdio>
#include <memory>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

std::unique_ptr<vr::TrackPipeline> make_track(
    int file_id,
    uint64_t generation,
    int64_t offset_us) {
  auto track = std::make_unique<vr::TrackPipeline>();
  track->file_id = file_id;
  track->generation = generation;
  track->offset_us = offset_us;
  return track;
}

vr::TextureFrame make_frame(int64_t pts_us) {
  vr::TextureFrame frame;
  frame.pts_us = pts_us;
  frame.duration_us = 33'333;
  frame.width = 640;
  frame.height = 360;
  return frame;
}

void set_identity(vr::PresentDecision& decision,
                  size_t slot,
                  const vr::TrackPipeline& track) {
  decision.file_ids[slot] = track.file_id;
  decision.track_generations[slot] = track.generation;
}

}  // namespace

int main() {
  vr::TrackPipelineManager tracks;
  tracks[0] = make_track(10, 100, 0);
  tracks[1] = make_track(20, 200, 5'000);
  tracks[2] = make_track(30, 300, -2'000);

  vr::PresentDecision last;
  last.should_present = true;
  last.frames[0] = make_frame(100'000);
  last.frames[1] = make_frame(200'000);
  last.frames[2] = make_frame(300'000);
  set_identity(last, 0, *tracks[0]);
  set_identity(last, 1, *tracks[1]);
  set_identity(last, 2, *tracks[2]);

  vr::PresentDecision next;
  next.should_present = true;
  next.current_pts_us = 4'000;
  next.frames[2] = make_frame(333'000);
  set_identity(next, 2, *tracks[2]);

  apply_present_carry_forward(tracks, last, next);
  if (!next.frames[0].has_value() ||
      next.frames[0]->pts_us != 100'000 ||
      next.file_ids[0] != 10 ||
      next.track_generations[0] != 100) {
    return fail("carry-forward did not preserve matching slot 0 identity");
  }
  if (next.frames[1].has_value()) {
    return fail("carry-forward ignored positive offset effective time guard");
  }
  if (!next.frames[2].has_value() || next.frames[2]->pts_us != 333'000) {
    return fail("carry-forward overwrote a fresh present frame");
  }

  tracks[0]->generation = 101;
  vr::PresentDecision stale_identity;
  stale_identity.current_pts_us = 10'000;
  apply_present_carry_forward(tracks, last, stale_identity);
  if (stale_identity.frames[0].has_value()) {
    return fail("carry-forward accepted stale track identity");
  }

  return 0;
}
