#include "analysis/analysis_generation_service.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "analysis_generation_service_smoke failed: " << message << "\n";
    std::exit(1);
}

vr::analysis::AnalysisGenerationOverlayChunkRequest request_for(
    int32_t start,
    int32_t end,
    int32_t priority) {
    vr::analysis::AnalysisGenerationOverlayChunkRequest request;
    request.video_path = "video.mp4";
    request.hash = "hash";
    request.cache_root = "/tmp/voidplayer-analysis-service-smoke";
    request.start_frame = start;
    request.end_frame = end;
    request.priority = priority;
    return request;
}

} // namespace

int main() {
    setenv("VOIDPLAYER_ANALYSIS_WORKERS", "1", 1);

    std::mutex mutex;
    std::condition_variable cv;
    bool first_started = false;
    bool release_first = false;
    std::vector<int32_t> started_frames;

    vr::analysis::AnalysisGenerationService service(
        [&](const vr::analysis::AnalysisGenerationOverlayChunkRequest& request,
            std::string& message) -> int32_t {
            {
                std::unique_lock<std::mutex> lock(mutex);
                started_frames.push_back(request.start_frame);
                if (request.start_frame == 0) {
                    first_started = true;
                    cv.notify_all();
                    cv.wait(lock, [&] { return release_first; });
                }
            }
            if (request.start_frame == 192) {
                message = "synthetic failure";
                return NAKI_ANALYSIS_ERR_INTERNAL;
            }
            return NAKI_ANALYSIS_OK;
        });

    const uint64_t first = service.submit_overlay_chunk(request_for(0, 63, 0));
    check(first != 0, "first job id");
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(2), [&] { return first_started; });
        check(first_started, "first job started");
    }

    const uint64_t low = service.submit_overlay_chunk(request_for(64, 127, 10));
    const uint64_t high = service.submit_overlay_chunk(request_for(128, 191, 1));
    const uint64_t high_dupe = service.submit_overlay_chunk(request_for(128, 191, 0));
    check(low != 0, "low-priority job id");
    check(high != 0, "high-priority job id");
    check(high_dupe == high, "deduplicated job id");

    NakiAnalysisGenerationServiceStats stats{};
    service.copy_stats(stats);
    check(stats.worker_count == 1, "worker count");
    check(stats.active_workers == 1, "active workers");
    check(stats.pending_jobs == 2, "pending jobs");
    check(stats.deduped_jobs == 1, "dedupe count");

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_first = true;
    }
    cv.notify_all();

    NakiAnalysisGenerationJobResult results[4]{};
    int32_t total = 0;
    for (int i = 0; i < 100 && total < 3; ++i) {
        total += service.poll_results(results + total, 4 - total);
        if (total < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    check(total == 3, "three successful jobs completed");
    check(started_frames.size() == 3, "started frame count");
    check(started_frames[0] == 0, "first job order");
    check(started_frames[1] == 128, "reprioritized job order");
    check(started_frames[2] == 64, "low-priority job order");
    check(results[0].ok != 0, "first result ok");
    check(results[1].ok != 0, "second result ok");
    check(results[1].priority == 0, "duplicate priority update");
    check(results[2].ok != 0, "third result ok");

    const uint64_t failed = service.submit_overlay_chunk(request_for(192, 255, 0));
    check(failed != 0, "failure job id");
    NakiAnalysisGenerationJobResult failure{};
    int32_t failure_count = 0;
    for (int i = 0; i < 100 && failure_count == 0; ++i) {
        failure_count = service.poll_results(&failure, 1);
        if (failure_count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    check(failure_count == 1, "failure result completed");
    check(failure.job_id == failed, "failure job id matches");
    check(failure.ok == 0, "failure result ok flag");
    check(failure.status == NAKI_ANALYSIS_ERR_INTERNAL, "failure status");
    check(std::string(failure.message).find("synthetic failure") != std::string::npos,
          "failure message");

    service.copy_stats(stats);
    check(stats.submitted_jobs == 4, "submitted job count");
    check(stats.completed_jobs == 4, "completed job count");
    check(stats.failed_jobs == 1, "failed job count");

    service.shutdown();
    return 0;
}
