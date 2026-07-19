#include "analysis/quality/quality_video_analyzer.h"
#include "analysis/quality/quality_wgpu_backend.h"
#include "media/media_input_session.h"
#include "media/video_decode_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/video_enc_params.h>
}

namespace vr::analysis::quality {
namespace {

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

const char* backend_name(QualityComputeBackend backend) {
    switch (backend) {
    case QualityComputeBackend::Cpu:
        return "cpu";
    case QualityComputeBackend::Wgpu:
        return "wgpu";
    case QualityComputeBackend::Auto:
        return "auto";
    }
    return "unknown";
}

const char* cpu_mode_name(QualityCpuMode mode) {
    return mode == QualityCpuMode::Scalar ? "scalar" : "auto";
}

bool make_luma_view(const AVFrame* frame, LumaPlaneView& view) {
    if (!frame || !frame->data[0] || frame->width <= 0 ||
        frame->height <= 0) {
        return false;
    }
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor || descriptor->nb_components < 1) {
        return false;
    }
    if ((descriptor->flags &
         (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL |
          AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL |
          AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_FLOAT)) != 0) {
        return false;
    }

    const AVComponentDescriptor& luma = descriptor->comp[0];
    if (luma.plane != 0 || (luma.step != 1 && luma.step != 2) ||
        luma.depth <= 0 || luma.depth > 16 || luma.shift < 0 ||
        luma.offset < 0 || luma.offset >= luma.step) {
        return false;
    }

    view.data = frame->data[0];
    view.width = frame->width;
    view.height = frame->height;
    view.stride_bytes = frame->linesize[0];
    view.sample_step_bytes = luma.step;
    view.sample_offset_bytes = luma.offset;
    view.bit_depth = luma.depth;
    view.sample_shift = luma.shift;
    return is_valid_luma_plane(view);
}

double frame_average_qp(AVFrame* frame) {
    AVFrameSideData* side_data =
        av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
    if (!side_data || !side_data->data ||
        side_data->size < sizeof(AVVideoEncParams)) {
        return -1.0;
    }

    auto* parameters =
        reinterpret_cast<AVVideoEncParams*>(side_data->data);
    if (parameters->nb_blocks == 0) {
        return static_cast<double>(parameters->qp);
    }
    if (parameters->block_size < sizeof(AVVideoBlockParams) ||
        parameters->blocks_offset > side_data->size) {
        return -1.0;
    }
    const size_t available = side_data->size - parameters->blocks_offset;
    if (parameters->nb_blocks > available / parameters->block_size) {
        return -1.0;
    }

    double weighted_qp = 0.0;
    uint64_t weighted_area = 0;
    for (unsigned int index = 0; index < parameters->nb_blocks; ++index) {
        const AVVideoBlockParams* block =
            av_video_enc_params_block(parameters, index);
        if (block->w <= 0 || block->h <= 0) {
            continue;
        }
        const uint64_t area =
            static_cast<uint64_t>(block->w) * block->h;
        weighted_qp +=
            static_cast<double>(parameters->qp + block->delta_qp) *
            static_cast<double>(area);
        weighted_area += area;
    }
    if (weighted_area == 0) {
        return static_cast<double>(parameters->qp);
    }
    return weighted_qp / static_cast<double>(weighted_area);
}

int64_t frame_pts_us(const AVFrame* frame,
                     AVRational time_base,
                     uint64_t decoded_index,
                     AVRational frame_rate) {
    int64_t timestamp = frame->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame->pts;
    }
    if (timestamp != AV_NOPTS_VALUE) {
        return av_rescale_q(timestamp, time_base, AVRational{1, 1'000'000});
    }
    if (frame_rate.num > 0 && frame_rate.den > 0) {
        return av_rescale_q(
            static_cast<int64_t>(decoded_index),
            AVRational{frame_rate.den, frame_rate.num},
            AVRational{1, 1'000'000});
    }
    return static_cast<int64_t>(decoded_index) * 33'333;
}

void count_frame_type(AVPictureType type, StreamStatistics& stream) {
    switch (type) {
    case AV_PICTURE_TYPE_I:
        ++stream.i_frames;
        break;
    case AV_PICTURE_TYPE_P:
        ++stream.p_frames;
        break;
    case AV_PICTURE_TYPE_B:
        ++stream.b_frames;
        break;
    default:
        break;
    }
}

struct OwnedLumaPlane {
    std::vector<uint8_t> storage;
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    int sample_step_bytes = 1;
    int bit_depth = 8;
    int sample_shift = 0;

    LumaPlaneView view() const {
        return LumaPlaneView{
            storage.data(),
            width,
            height,
            stride_bytes,
            sample_step_bytes,
            0,
            bit_depth,
            sample_shift,
        };
    }
};

std::shared_ptr<OwnedLumaPlane> copy_luma_plane(
    const LumaPlaneView& source) {
    auto owned = std::make_shared<OwnedLumaPlane>();
    owned->width = source.width;
    owned->height = source.height;
    owned->sample_step_bytes = source.sample_step_bytes;
    owned->stride_bytes =
        source.width * source.sample_step_bytes;
    owned->bit_depth = source.bit_depth;
    owned->sample_shift = source.sample_shift;
    owned->storage.resize(
        static_cast<size_t>(owned->stride_bytes) *
        static_cast<size_t>(source.height));
    const size_t sample_bytes =
        static_cast<size_t>(source.sample_step_bytes);
    for (int y = 0; y < source.height; ++y) {
        const uint8_t* input =
            source.data +
            static_cast<ptrdiff_t>(y) * source.stride_bytes;
        uint8_t* output =
            owned->storage.data() +
            static_cast<ptrdiff_t>(y) * owned->stride_bytes;
        if (source.sample_offset_bytes == 0) {
            std::memcpy(
                output,
                input,
                static_cast<size_t>(owned->stride_bytes));
            continue;
        }
        for (int x = 0; x < source.width; ++x) {
            std::memcpy(
                output +
                    static_cast<ptrdiff_t>(x) *
                        source.sample_step_bytes,
                input +
                    static_cast<ptrdiff_t>(x) *
                        source.sample_step_bytes +
                    source.sample_offset_bytes,
                sample_bytes);
        }
    }
    return owned;
}

struct MetricTaskResult {
    double score = 0.0;
    double elapsed_ms = 0.0;
};

class QualityThreadPool {
public:
    explicit QualityThreadPool(uint32_t worker_count) {
        workers_.reserve(worker_count);
        try {
            for (uint32_t index = 0; index < worker_count; ++index) {
                workers_.emplace_back(
                    [this]() { worker_loop(); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            ready_.notify_all();
            for (std::thread& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    ~QualityThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    QualityThreadPool(const QualityThreadPool&) = delete;
    QualityThreadPool& operator=(const QualityThreadPool&) = delete;

    std::future<MetricTaskResult> submit(
        std::function<MetricTaskResult()> work) {
        auto task = std::make_shared<
            std::packaged_task<MetricTaskResult()>>(
            std::move(work));
        std::future<MetricTaskResult> future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                throw std::runtime_error(
                    "quality worker pool is stopping");
            }
            tasks_.emplace_back([task]() { (*task)(); });
        }
        ready_.notify_one();
        return future;
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

uint32_t resolve_cpu_workers(uint32_t requested) {
    if (requested > 0) {
        return requested;
    }
    const uint32_t hardware = std::thread::hardware_concurrency();
    return std::max<uint32_t>(1, hardware == 0 ? 1 : hardware);
}

uint32_t resolve_cpu_in_flight(uint32_t requested,
                               uint32_t worker_count) {
    if (requested > 0) {
        return requested;
    }
    const uint32_t frames_for_two_waves =
        ((worker_count + 3) / 4) * 2;
    return std::clamp<uint32_t>(
        frames_for_two_waves, 2, 64);
}

}  // namespace

bool analyze_video_quality(const std::string& video_path,
                           const QualityVideoAnalyzerOptions& options,
                           QualityReport& report,
                           std::string& error) {
    report = QualityReport{};
    report.sample_interval_us = std::max<int64_t>(
        0, options.sample_interval_us);
    report.execution.requested_backend =
        backend_name(options.backend);
    report.execution.cpu_mode =
        cpu_mode_name(options.cpu_mode);
    error.clear();

    MediaInputSession input_session;
    VideoDecodeSession decoder_session;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    auto cleanup = [&]() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        decoder_session.close();
        input_session.close();
    };

    MediaInputOpenOptions input_options;
    if (!input_session.open(video_path, input_options, error)) {
        cleanup();
        return false;
    }

    int result = 0;
    const int stream_index =
        input_session.best_stream_index(AVMEDIA_TYPE_VIDEO);
    if (stream_index < 0) {
        error = "video stream not found: " + ffmpeg_error(stream_index);
        cleanup();
        return false;
    }
    AVCodecParameters* codec_parameters =
        input_session.codec_parameters(stream_index);
    const AVRational stream_time_base =
        input_session.time_base_for_stream(stream_index);
    if (!codec_parameters || stream_time_base.num <= 0 ||
        stream_time_base.den <= 0) {
        error = "video stream metadata is incomplete";
        cleanup();
        return false;
    }
    VideoDecodeSessionOptions decode_options;
    decode_options.export_side_data =
        AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
    decode_options.thread_count =
        options.decode_threads == 0
            ? 0
            : static_cast<int>(std::min<uint32_t>(
                  options.decode_threads,
                  static_cast<uint32_t>(
                      std::numeric_limits<int>::max())));
    if (!decoder_session.initialize(
            codec_parameters, decode_options, error) ||
        !decoder_session.open(error)) {
        cleanup();
        return false;
    }
    AVCodecContext* codec_context =
        decoder_session.codec_context();

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!packet || !frame) {
        error = "failed to allocate decode packet/frame";
        cleanup();
        return false;
    }

    std::vector<double> packet_sizes;
    std::vector<double> qp_values;
    std::vector<double> blockiness_values;
    std::vector<double> banding_values;
    std::vector<double> blur_values;
    std::vector<double> noise_values;
    std::vector<double> flicker_values;
    std::vector<double> blockiness_timings;
    std::vector<double> banding_timings;
    std::vector<double> blur_timings;
    std::vector<double> noise_timings;
    std::vector<double> temporal_timings;
    std::vector<double> gpu_pack_timings;
    std::vector<double> gpu_submit_timings;
    std::vector<double> gpu_wait_timings;
    std::vector<double> gpu_submit_wait_timings;
    std::vector<double> gpu_readback_timings;
    std::vector<double> gpu_total_timings;
    std::vector<double> gpu_latency_timings;
    packet_sizes.reserve(1024);
    qp_values.reserve(1024);

    int64_t next_sample_us = std::numeric_limits<int64_t>::min();
    LumaTemporalSignature previous_previous_signature;
    LumaTemporalSignature previous_signature;
    uint32_t temporal_signature_count = 0;
    bool stop = false;
    bool decode_failed = false;
    bool quality_backend_failed = false;
    bool cpu_backend_failed = false;
    uint64_t accepted_samples = 0;
    const AVRational frame_rate =
        input_session.frame_rate_for_stream(stream_index);
    const uint32_t cpu_worker_count =
        resolve_cpu_workers(options.cpu_workers);
    const uint32_t cpu_in_flight =
        resolve_cpu_in_flight(
            options.cpu_in_flight, cpu_worker_count);
    report.execution.cpu_dispatch =
        options.cpu_mode == QualityCpuMode::Scalar
            ? "scalar-forced"
            : quality_cpu_dispatch_name();
    report.execution.decode_threads_requested =
        options.decode_threads;
    report.execution.decoder_thread_count =
        codec_context->thread_count;
    report.execution.decoder_thread_type =
        codec_context->active_thread_type;
    report.execution.cpu_workers = cpu_worker_count;
    report.execution.cpu_in_flight = cpu_in_flight;
    report.execution.scheduling =
        "bounded-multi-frame-four-metric-tasks";
    const std::string decode_diagnostic =
        std::string("inputCore=shared-native; inputBackend=") +
        input_session.backend_name() +
        "; decodeCore=shared-native; decodeThreads=" +
        (options.decode_threads == 0
             ? "auto"
             : std::to_string(options.decode_threads)) +
        "; decoderThreadCount=" +
        std::to_string(codec_context->thread_count) +
        "; decoderThreadType=" +
        std::to_string(codec_context->active_thread_type);
    std::unique_ptr<QualityThreadPool> cpu_pool;
    auto ensure_cpu_pool = [&]() -> bool {
        if (cpu_pool) {
            return true;
        }
        try {
            cpu_pool =
                std::make_unique<QualityThreadPool>(
                    cpu_worker_count);
            report.execution.cpu_worker_pool_active = true;
            return true;
        } catch (const std::exception& exception) {
            error = std::string(
                        "failed to create CPU quality worker pool: ") +
                    exception.what();
            cpu_backend_failed = true;
            stop = true;
            return false;
        }
    };

    std::unique_ptr<WgpuQualityBackend> wgpu_backend;
    if (options.backend == QualityComputeBackend::Cpu) {
        if (!ensure_cpu_pool()) {
            cleanup();
            return false;
        }
        report.backend_diagnostic =
            std::string("cpuDispatch=") +
            (options.cpu_mode == QualityCpuMode::Scalar
                 ? "scalar-forced"
                 : quality_cpu_dispatch_name()) +
            "; cpuWorkers=" + std::to_string(cpu_worker_count) +
            "; cpuInFlight=" + std::to_string(cpu_in_flight) +
            "; " + decode_diagnostic +
            "; scheduling=multi-frame+four-metric-tasks"
            "; avx2Kernels=u8,u16,p010"
            "(blockiness,banding,blur,noise); "
            "scalarFallback=unsupported-layouts-and-temporal";
        report.execution.resolved_backend = report.backend;
    } else {
        std::string wgpu_error;
        wgpu_backend = WgpuQualityBackend::create(wgpu_error);
        if (!wgpu_backend) {
            if (options.backend == QualityComputeBackend::Wgpu) {
                error = "wgpu quality backend unavailable: " + wgpu_error;
                cleanup();
                return false;
            }
            report.backend = kQualityBackendName;
            report.backend_diagnostic =
                "wgpu unavailable; CPU fallback: " + wgpu_error +
                "; cpuDispatch=" + quality_cpu_dispatch_name() +
                "; cpuWorkers=" +
                std::to_string(cpu_worker_count) +
                "; cpuInFlight=" +
                std::to_string(cpu_in_flight) +
                "; " + decode_diagnostic;
            if (!ensure_cpu_pool()) {
                cleanup();
                return false;
            }
            report.execution.resolved_backend = report.backend;
        } else {
            const auto& creation = wgpu_backend->creation_timings();
            std::ostringstream diagnostic;
            diagnostic << "adapter=" << wgpu_backend->adapter_name()
                       << "; createMs(instance/adapter/device/pipeline/total)="
                       << creation.instance_ms << "/"
                       << creation.adapter_ms << "/"
                       << creation.device_ms << "/"
                       << creation.pipeline_ms << "/"
                       << creation.total_ms
                       << "; inFlight=" << wgpu_backend->max_in_flight()
                       << "; " << decode_diagnostic;
            report.backend = "wgpu-compute";
            report.execution.resolved_backend = report.backend;
            report.execution.gpu_adapter =
                wgpu_backend->adapter_name();
            report.execution.gpu_in_flight =
                wgpu_backend->max_in_flight();
            report.backend_diagnostic = diagnostic.str();
        }
    }

    struct PendingGpuSample {
        WgpuQualityBackend::Ticket ticket = 0;
        FrameQualitySample sample;
    };
    std::deque<PendingGpuSample> pending_gpu_samples;
    struct PendingCpuSample {
        FrameQualitySample sample;
        std::array<std::future<MetricTaskResult>, 4> metrics;
    };
    std::deque<PendingCpuSample> pending_cpu_samples;

    auto commit_sample = [&](FrameQualitySample sample) {
        blockiness_values.push_back(sample.blockiness);
        banding_values.push_back(sample.banding);
        blur_values.push_back(sample.blur);
        noise_values.push_back(sample.noise);
        report.timeline.push_back(std::move(sample));
    };

    auto commit_gpu_sample = [&](PendingGpuSample pending,
                                 const WgpuQualityScores& scores) {
        pending.sample.blockiness = scores.blockiness;
        pending.sample.banding = scores.banding;
        pending.sample.blur = scores.blur;
        pending.sample.noise = scores.noise;
        gpu_pack_timings.push_back(scores.pack_ms);
        gpu_submit_timings.push_back(scores.submit_ms);
        gpu_wait_timings.push_back(scores.wait_ms);
        gpu_submit_wait_timings.push_back(scores.submit_wait_ms);
        gpu_readback_timings.push_back(scores.readback_ms);
        gpu_total_timings.push_back(scores.total_ms);
        gpu_latency_timings.push_back(scores.latency_ms);
        commit_sample(std::move(pending.sample));
    };

    auto collect_oldest_gpu = [&](bool wait) {
        if (pending_gpu_samples.empty() || !wgpu_backend) {
            return true;
        }
        WgpuQualityScores scores;
        std::string gpu_error;
        const auto ticket = pending_gpu_samples.front().ticket;
        if (!wait) {
            const auto status =
                wgpu_backend->try_collect_plane(ticket, scores, gpu_error);
            if (status == WgpuQualityBackend::CollectStatus::Pending) {
                return true;
            }
            if (status == WgpuQualityBackend::CollectStatus::Error) {
                error = "wgpu quality collection failed: " + gpu_error;
                quality_backend_failed = true;
                stop = true;
                return false;
            }
        } else if (!wgpu_backend->collect_plane(
                       ticket, scores, gpu_error)) {
            error = "wgpu quality collection failed: " + gpu_error;
            quality_backend_failed = true;
            stop = true;
            return false;
        }
        PendingGpuSample pending =
            std::move(pending_gpu_samples.front());
        pending_gpu_samples.pop_front();
        commit_gpu_sample(std::move(pending), scores);
        return true;
    };

    auto collect_ready_gpu = [&]() {
        while (!pending_gpu_samples.empty()) {
            const size_t before = pending_gpu_samples.size();
            if (!collect_oldest_gpu(false)) {
                return false;
            }
            if (pending_gpu_samples.size() == before) {
                break;
            }
        }
        return true;
    };

    auto collect_oldest_cpu = [&](bool wait) {
        if (pending_cpu_samples.empty()) {
            return true;
        }
        if (!wait) {
            for (auto& metric :
                 pending_cpu_samples.front().metrics) {
                if (metric.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
                    return true;
                }
            }
        }
        PendingCpuSample pending =
            std::move(pending_cpu_samples.front());
        pending_cpu_samples.pop_front();
        try {
            const MetricTaskResult blockiness =
                pending.metrics[0].get();
            const MetricTaskResult banding =
                pending.metrics[1].get();
            const MetricTaskResult blur =
                pending.metrics[2].get();
            const MetricTaskResult noise =
                pending.metrics[3].get();
            pending.sample.blockiness = blockiness.score;
            pending.sample.banding = banding.score;
            pending.sample.blur = blur.score;
            pending.sample.noise = noise.score;
            blockiness_timings.push_back(
                blockiness.elapsed_ms);
            banding_timings.push_back(banding.elapsed_ms);
            blur_timings.push_back(blur.elapsed_ms);
            noise_timings.push_back(noise.elapsed_ms);
            commit_sample(std::move(pending.sample));
            return true;
        } catch (const std::exception& exception) {
            error = std::string(
                        "CPU quality task failed: ") +
                    exception.what();
            cpu_backend_failed = true;
            stop = true;
            return false;
        } catch (...) {
            error = "CPU quality task failed with unknown exception";
            cpu_backend_failed = true;
            stop = true;
            return false;
        }
    };

    auto collect_ready_cpu = [&]() {
        while (!pending_cpu_samples.empty()) {
            const size_t before = pending_cpu_samples.size();
            if (!collect_oldest_cpu(false)) {
                return false;
            }
            if (pending_cpu_samples.size() == before) {
                break;
            }
        }
        return true;
    };

    auto process_frame = [&](AVFrame* decoded) {
        if (!collect_ready_gpu() || !collect_ready_cpu()) {
            return;
        }
        const uint64_t decoded_index = report.stream.decoded_frames++;
        count_frame_type(decoded->pict_type, report.stream);
        const double average_qp = frame_average_qp(decoded);
        if (average_qp >= 0.0 && std::isfinite(average_qp)) {
            qp_values.push_back(average_qp);
        }

        const int64_t pts_us = frame_pts_us(
            decoded,
            stream_time_base,
            decoded_index,
            frame_rate);
        LumaPlaneView luma;
        const bool supported_luma = make_luma_view(decoded, luma);
        double flicker = -1.0;
        if (supported_luma) {
            const auto temporal_start = std::chrono::steady_clock::now();
            LumaTemporalSignature current_signature;
            if (make_temporal_signature(luma, current_signature)) {
                if (temporal_signature_count >= 2) {
                    flicker = measure_flicker_proxy(
                        previous_previous_signature,
                        previous_signature,
                        current_signature);
                    if (flicker >= 0.0) {
                        flicker_values.push_back(flicker);
                    }
                }
                previous_previous_signature =
                    std::move(previous_signature);
                previous_signature = std::move(current_signature);
                ++temporal_signature_count;
            }
            temporal_timings.push_back(
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - temporal_start)
                    .count());
        }

        bool sample = report.sample_interval_us == 0;
        if (next_sample_us == std::numeric_limits<int64_t>::min()) {
            sample = true;
        } else if (pts_us >= next_sample_us) {
            sample = true;
        }
        if (!sample) {
            return;
        }
        next_sample_us =
            pts_us + std::max<int64_t>(1, report.sample_interval_us);

        if (!supported_luma) {
            ++report.unsupported_pixel_frames;
            return;
        }
        if (report.width == 0) {
            report.width = luma.width;
            report.height = luma.height;
            report.bit_depth = luma.bit_depth;
        }

        FrameQualitySample sample_result;
        sample_result.sample_index = accepted_samples;
        sample_result.decoded_frame_index = decoded_index;
        sample_result.pts_us = pts_us;
        sample_result.flicker = flicker;
        sample_result.average_qp = average_qp;
        bool submitted_to_gpu = false;
        if (wgpu_backend) {
            while (pending_gpu_samples.size() >=
                   wgpu_backend->max_in_flight()) {
                if (!collect_oldest_gpu(true)) {
                    return;
                }
            }
            WgpuQualityBackend::Ticket ticket = 0;
            std::string gpu_error;
            if (wgpu_backend->submit_plane(luma, ticket, gpu_error)) {
                pending_gpu_samples.push_back(PendingGpuSample{
                    ticket,
                    sample_result,
                });
                submitted_to_gpu = true;
            } else if (options.backend == QualityComputeBackend::Wgpu) {
                error = "wgpu quality submission failed: " + gpu_error;
                quality_backend_failed = true;
                stop = true;
                return;
            } else {
                while (!pending_gpu_samples.empty()) {
                    if (!collect_oldest_gpu(true)) {
                        return;
                    }
                }
                report.backend = "wgpu-compute+cpu-fallback";
                report.execution.resolved_backend =
                    report.backend;
                report.backend_diagnostic =
                    "adapter=" + wgpu_backend->adapter_name() +
                    "; submission fallback: " + gpu_error +
                    "; cpuWorkers=" +
                    std::to_string(cpu_worker_count) +
                    "; cpuInFlight=" +
                    std::to_string(cpu_in_flight);
                wgpu_backend.reset();
            }
        }
        if (!submitted_to_gpu) {
            if (!ensure_cpu_pool()) {
                return;
            }
            while (pending_cpu_samples.size() >=
                   cpu_in_flight) {
                if (!collect_oldest_cpu(true)) {
                    return;
                }
            }
            try {
                const auto owned_luma = copy_luma_plane(luma);
                auto metric_task =
                    [owned_luma,
                     cpu_mode = options.cpu_mode](int metric) {
                        return [owned_luma,
                                cpu_mode,
                                metric]() {
                            const auto start =
                                std::chrono::steady_clock::now();
                            const LumaPlaneView view =
                                owned_luma->view();
                            double score = 0.0;
                            switch (metric) {
                            case 0:
                                score =
                                    measure_blockiness(
                                        view, cpu_mode);
                                break;
                            case 1:
                                score =
                                    measure_banding_proxy(
                                        view, cpu_mode);
                                break;
                            case 2:
                                score =
                                    measure_blur_proxy(
                                        view, cpu_mode);
                                break;
                            case 3:
                                score =
                                    measure_noise_proxy(
                                        view, cpu_mode);
                                break;
                            default:
                                throw std::runtime_error(
                                    "invalid metric task");
                            }
                            return MetricTaskResult{
                                score,
                                std::chrono::duration<
                                    double,
                                    std::milli>(
                                    std::chrono::steady_clock::now() -
                                    start)
                                    .count(),
                            };
                        };
                    };
                PendingCpuSample pending;
                pending.sample = std::move(sample_result);
                for (int metric = 0; metric < 4; ++metric) {
                    pending.metrics[static_cast<size_t>(metric)] =
                        cpu_pool->submit(metric_task(metric));
                }
                pending_cpu_samples.push_back(
                    std::move(pending));
            } catch (const std::exception& exception) {
                error = std::string(
                            "CPU quality submission failed: ") +
                        exception.what();
                cpu_backend_failed = true;
                stop = true;
                return;
            }
        }

        ++accepted_samples;
        if (options.max_samples > 0 &&
            accepted_samples >= options.max_samples) {
            stop = true;
        }
    };

    auto receive_frames = [&]() {
        while (!stop) {
            const int receive_result =
                decoder_session.receive_frame(frame);
            if (receive_result == AVERROR(EAGAIN) ||
                receive_result == AVERROR_EOF) {
                return true;
            }
            if (receive_result < 0) {
                error = "avcodec_receive_frame failed: " +
                        ffmpeg_error(receive_result);
                return false;
            }
            process_frame(frame);
            av_frame_unref(frame);
        }
        return true;
    };

    int read_result = 0;
    while (!stop &&
           (read_result = input_session.read_packet(packet)) >= 0) {
        if (packet->stream_index == stream_index) {
            ++report.stream.packet_count;
            report.stream.packet_bytes +=
                static_cast<uint64_t>(std::max(packet->size, 0));
            if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
                ++report.stream.keyframe_packets;
            }
            packet_sizes.push_back(
                static_cast<double>(std::max(packet->size, 0)));

            result = decoder_session.send_packet(packet);
            if (result == AVERROR(EAGAIN)) {
                if (!receive_frames()) {
                    decode_failed = true;
                } else {
                    result = decoder_session.send_packet(packet);
                }
            }
            if (!decode_failed && result < 0) {
                error = "avcodec_send_packet failed: " +
                        ffmpeg_error(result);
                decode_failed = true;
            }
            if (!decode_failed && !receive_frames()) {
                decode_failed = true;
            }
        }
        av_packet_unref(packet);
        if (decode_failed) {
            break;
        }
    }
    if (!stop && !decode_failed &&
        read_result < 0 && read_result != AVERROR_EOF) {
        error = "input packet read failed: " +
                ffmpeg_error(read_result);
        decode_failed = true;
    }

    if (!stop && !decode_failed) {
        result = decoder_session.send_packet(nullptr);
        if (result >= 0 && !receive_frames()) {
            decode_failed = true;
        } else if (result < 0 && result != AVERROR_EOF) {
            error = "decoder flush failed: " + ffmpeg_error(result);
            decode_failed = true;
        }
    }

    while (!pending_gpu_samples.empty() &&
           !quality_backend_failed) {
        if (!collect_oldest_gpu(true)) {
            break;
        }
    }
    while (!pending_cpu_samples.empty() &&
           !cpu_backend_failed) {
        if (!collect_oldest_cpu(true)) {
            break;
        }
    }
    std::stable_sort(
        report.timeline.begin(),
        report.timeline.end(),
        [](const FrameQualitySample& left,
           const FrameQualitySample& right) {
            return left.pts_us < right.pts_us;
        });

    report.stream.packet_size_bytes =
        summarize_distribution(std::move(packet_sizes));
    report.stream.average_qp =
        summarize_distribution(std::move(qp_values));
    report.blockiness =
        summarize_distribution(std::move(blockiness_values));
    report.banding =
        summarize_distribution(std::move(banding_values));
    report.blur =
        summarize_distribution(std::move(blur_values));
    report.noise =
        summarize_distribution(std::move(noise_values));
    report.flicker =
        summarize_distribution(std::move(flicker_values));
    report.timings.blockiness_ms =
        summarize_distribution(std::move(blockiness_timings));
    report.timings.banding_ms =
        summarize_distribution(std::move(banding_timings));
    report.timings.blur_ms =
        summarize_distribution(std::move(blur_timings));
    report.timings.noise_ms =
        summarize_distribution(std::move(noise_timings));
    report.timings.temporal_ms =
        summarize_distribution(std::move(temporal_timings));
    report.timings.gpu_pack_ms =
        summarize_distribution(std::move(gpu_pack_timings));
    report.timings.gpu_submit_ms =
        summarize_distribution(std::move(gpu_submit_timings));
    report.timings.gpu_wait_ms =
        summarize_distribution(std::move(gpu_wait_timings));
    report.timings.gpu_submit_wait_ms =
        summarize_distribution(std::move(gpu_submit_wait_timings));
    report.timings.gpu_readback_ms =
        summarize_distribution(std::move(gpu_readback_timings));
    report.timings.gpu_total_ms =
        summarize_distribution(std::move(gpu_total_timings));
    report.timings.gpu_latency_ms =
        summarize_distribution(std::move(gpu_latency_timings));

    if (quality_backend_failed &&
        options.backend == QualityComputeBackend::Auto) {
        const std::string gpu_failure = error;
        cleanup();
        QualityVideoAnalyzerOptions cpu_options = options;
        cpu_options.backend = QualityComputeBackend::Cpu;
        QualityReport cpu_report;
        std::string cpu_error;
        if (analyze_video_quality(
                video_path,
                cpu_options,
                cpu_report,
                cpu_error)) {
            cpu_report.backend = "wgpu-compute+cpu-restart";
            cpu_report.backend_diagnostic =
                gpu_failure + "; restarted analysis on CPU";
            cpu_report.execution.requested_backend =
                backend_name(options.backend);
            cpu_report.execution.resolved_backend =
                cpu_report.backend;
            report = std::move(cpu_report);
            error.clear();
            return true;
        }
        error = gpu_failure + "; CPU restart failed: " + cpu_error;
        return false;
    }
    if (decode_failed || quality_backend_failed ||
        cpu_backend_failed) {
        cleanup();
        return false;
    }
    if (report.timeline.empty()) {
        error = report.unsupported_pixel_frames > 0
                    ? "decoded pixel format is unsupported by CPU reference metrics"
                    : "no video frames were decoded";
        cleanup();
        return false;
    }

    cleanup();
    return true;
}

}  // namespace vr::analysis::quality
