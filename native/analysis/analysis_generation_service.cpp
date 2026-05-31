#include "analysis/analysis_generation_service.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <utility>

namespace vr::analysis {
namespace {

int32_t default_worker_count() {
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const int32_t fallback =
        std::min<int32_t>(std::max<int32_t>(2, static_cast<int32_t>(hardware / 2)), 8);
    const char* env = std::getenv("VOIDPLAYER_ANALYSIS_WORKERS");
    if (!env || env[0] == '\0') {
        return std::min<int32_t>(fallback, static_cast<int32_t>(hardware));
    }
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end == env || parsed <= 0) {
        return std::min<int32_t>(fallback, static_cast<int32_t>(hardware));
    }
    return std::clamp<int32_t>(
        static_cast<int32_t>(parsed),
        1,
        std::max<int32_t>(1, static_cast<int32_t>(hardware)));
}

std::string make_overlay_key(const AnalysisGenerationOverlayChunkRequest& request) {
    return "overlay:" + request.hash + ":" + std::to_string(request.start_frame) +
           ":" + std::to_string(request.end_frame);
}

void copy_string(char* dest, size_t cap, const std::string& value) {
    if (!dest || cap == 0) return;
    const size_t n = std::min(cap - 1, value.size());
    std::memcpy(dest, value.data(), n);
    dest[n] = '\0';
}

} // namespace

AnalysisGenerationService::AnalysisGenerationService(OverlayChunkExecutor executor)
    : executor_(std::move(executor)), worker_count_(default_worker_count()) {}

AnalysisGenerationService::~AnalysisGenerationService() {
    shutdown();
}

uint64_t AnalysisGenerationService::submit_overlay_chunk(
    AnalysisGenerationOverlayChunkRequest request) {
    if (request.hash.empty() || request.video_path.empty() ||
        request.cache_root.empty() || request.start_frame < 0 ||
        request.end_frame < request.start_frame) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ensure_started_locked();
    const std::string key = make_overlay_key(request);
    const auto live = live_jobs_by_key_.find(key);
    if (live != live_jobs_by_key_.end()) {
        for (auto& pending : pending_) {
            if (pending.id == live->second &&
                request.priority < pending.request.priority) {
                pending.request.priority = request.priority;
                std::stable_sort(
                    pending_.begin(), pending_.end(), [](const Job& a, const Job& b) {
                        if (a.request.priority != b.request.priority) {
                            return a.request.priority < b.request.priority;
                        }
                        return a.sequence < b.sequence;
                    });
                break;
            }
        }
        deduped_jobs_++;
        return live->second;
    }

    Job job;
    job.id = next_job_id_++;
    job.sequence = next_sequence_++;
    job.key = key;
    job.request = std::move(request);
    live_jobs_by_key_[job.key] = job.id;
    pending_.push_back(std::move(job));
    submitted_jobs_++;
    std::stable_sort(pending_.begin(), pending_.end(), [](const Job& a, const Job& b) {
        if (a.request.priority != b.request.priority) {
            return a.request.priority < b.request.priority;
        }
        return a.sequence < b.sequence;
    });
    cv_.notify_one();
    return live_jobs_by_key_[key];
}

int32_t AnalysisGenerationService::poll_results(
    NakiAnalysisGenerationJobResult* out,
    int32_t max_count) {
    if (!out || max_count <= 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t count = 0;
    while (count < max_count && !completed_.empty()) {
        Result result = std::move(completed_.front());
        completed_.pop_front();
        NakiAnalysisGenerationJobResult& dst = out[count];
        std::memset(&dst, 0, sizeof(dst));
        dst.job_id = result.id;
        dst.kind = NAKI_ANALYSIS_GENERATION_JOB_OVERLAY_CHUNK;
        dst.ok = result.ok;
        dst.status = result.status;
        dst.start_frame = result.request.start_frame;
        dst.end_frame = result.request.end_frame;
        dst.priority = result.request.priority;
        copy_string(dst.hash, sizeof(dst.hash), result.request.hash);
        copy_string(dst.message, sizeof(dst.message), result.message);
        count++;
    }
    return count;
}

void AnalysisGenerationService::copy_stats(
    NakiAnalysisGenerationServiceStats& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memset(&out, 0, sizeof(out));
    out.worker_count = worker_count_;
    out.active_workers = active_workers_;
    out.pending_jobs = static_cast<int32_t>(pending_.size());
    out.running_jobs = active_workers_;
    out.completed_jobs = completed_jobs_;
    out.failed_jobs = failed_jobs_;
    out.deduped_jobs = deduped_jobs_;
    out.backpressure_drop_count = backpressure_drop_count_;
    out.submitted_jobs = submitted_jobs_;
}

void AnalysisGenerationService::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
}

void AnalysisGenerationService::ensure_started_locked() {
    if (!workers_.empty()) return;
    stopping_ = false;
    workers_.reserve(static_cast<size_t>(worker_count_));
    for (int32_t i = 0; i < worker_count_; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

void AnalysisGenerationService::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) return;
            job = std::move(pending_.front());
            pending_.pop_front();
            active_workers_++;
        }

        std::string message;
        int32_t status = NAKI_ANALYSIS_ERR_INTERNAL;
        try {
            status = executor_(job.request, message);
        } catch (const std::exception& e) {
            status = NAKI_ANALYSIS_ERR_INTERNAL;
            message = e.what();
        } catch (...) {
            status = NAKI_ANALYSIS_ERR_INTERNAL;
            message = "analysis generation job threw an unknown exception";
        }

        Result result;
        result.id = job.id;
        result.ok = status == NAKI_ANALYSIS_OK ? 1 : 0;
        result.status = status;
        result.request = std::move(job.request);
        result.message = std::move(message);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_workers_ > 0) active_workers_--;
            live_jobs_by_key_.erase(job.key);
            completed_.push_back(std::move(result));
            completed_jobs_++;
            if (status != NAKI_ANALYSIS_OK) failed_jobs_++;
        }
    }
}

} // namespace vr::analysis
