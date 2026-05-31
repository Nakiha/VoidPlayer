#pragma once

#include "analysis/analysis_ffi_abi.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vr::analysis {

struct AnalysisGenerationOverlayChunkRequest {
    std::string video_path;
    std::string hash;
    std::string cache_root;
    int32_t start_frame = 0;
    int32_t end_frame = 0;
    int64_t max_cache_bytes = 0;
    int32_t priority = 0;
};

class AnalysisGenerationService {
public:
    using OverlayChunkExecutor = std::function<int32_t(
        const AnalysisGenerationOverlayChunkRequest& request,
        std::string& message)>;

    explicit AnalysisGenerationService(OverlayChunkExecutor executor);
    ~AnalysisGenerationService();

    AnalysisGenerationService(const AnalysisGenerationService&) = delete;
    AnalysisGenerationService& operator=(const AnalysisGenerationService&) = delete;

    uint64_t submit_overlay_chunk(AnalysisGenerationOverlayChunkRequest request);
    int32_t poll_results(NakiAnalysisGenerationJobResult* out, int32_t max_count);
    void copy_stats(NakiAnalysisGenerationServiceStats& out) const;
    void shutdown();

private:
    struct Job {
        uint64_t id = 0;
        uint64_t sequence = 0;
        std::string key;
        AnalysisGenerationOverlayChunkRequest request;
    };

    struct Result {
        uint64_t id = 0;
        int32_t ok = 0;
        int32_t status = NAKI_ANALYSIS_ERR_INTERNAL;
        AnalysisGenerationOverlayChunkRequest request;
        std::string message;
    };

    void ensure_started_locked();
    void worker_loop();

    OverlayChunkExecutor executor_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    int32_t worker_count_ = 1;
    uint64_t next_job_id_ = 1;
    uint64_t next_sequence_ = 1;
    std::vector<std::thread> workers_;
    std::deque<Job> pending_;
    std::deque<Result> completed_;
    std::unordered_map<std::string, uint64_t> live_jobs_by_key_;
    int32_t active_workers_ = 0;
    int32_t completed_jobs_ = 0;
    int32_t failed_jobs_ = 0;
    int32_t deduped_jobs_ = 0;
    int32_t backpressure_drop_count_ = 0;
    uint64_t submitted_jobs_ = 0;
};

} // namespace vr::analysis
