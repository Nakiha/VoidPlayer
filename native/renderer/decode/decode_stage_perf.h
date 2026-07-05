#pragma once

#include <atomic>
#include <cstdint>

namespace vr {

struct DecodeStagePerfCounters {
    std::atomic<uint64_t> packet_send_count{0};
    std::atomic<uint64_t> packet_send_total_us{0};
    std::atomic<uint64_t> packet_send_max_us{0};

    std::atomic<uint64_t> receive_loop_count{0};
    std::atomic<uint64_t> receive_loop_frame_count{0};
    std::atomic<uint64_t> receive_loop_total_us{0};
    std::atomic<uint64_t> receive_loop_max_us{0};

    std::atomic<uint64_t> convert_count{0};
    std::atomic<uint64_t> convert_total_us{0};
    std::atomic<uint64_t> convert_max_us{0};
    std::atomic<uint64_t> convert_direct_planar_count{0};
    std::atomic<uint64_t> convert_direct_planar_total_us{0};
    std::atomic<uint64_t> convert_direct_planar_max_us{0};
    std::atomic<uint64_t> convert_nv12_layout_count{0};
    std::atomic<uint64_t> convert_nv12_layout_total_us{0};
    std::atomic<uint64_t> convert_nv12_layout_max_us{0};
    std::atomic<uint64_t> convert_nv12_alloc_count{0};
    std::atomic<uint64_t> convert_nv12_alloc_total_us{0};
    std::atomic<uint64_t> convert_nv12_alloc_max_us{0};
    std::atomic<uint64_t> convert_nv12_pack_count{0};
    std::atomic<uint64_t> convert_nv12_pack_total_us{0};
    std::atomic<uint64_t> convert_nv12_pack_max_us{0};

    std::atomic<uint64_t> publish_count{0};
    std::atomic<uint64_t> publish_total_us{0};
    std::atomic<uint64_t> publish_max_us{0};
    std::atomic<uint64_t> publish_lock_count{0};
    std::atomic<uint64_t> publish_lock_total_us{0};
    std::atomic<uint64_t> publish_lock_max_us{0};
    std::atomic<uint64_t> publish_wait_count{0};
    std::atomic<uint64_t> publish_wait_total_us{0};
    std::atomic<uint64_t> publish_wait_max_us{0};
    std::atomic<uint64_t> publish_ring_push_count{0};
    std::atomic<uint64_t> publish_ring_push_total_us{0};
    std::atomic<uint64_t> publish_ring_push_max_us{0};
    std::atomic<uint64_t> publish_ring_lock_count{0};
    std::atomic<uint64_t> publish_ring_lock_total_us{0};
    std::atomic<uint64_t> publish_ring_lock_max_us{0};
    std::atomic<uint64_t> publish_ring_assign_count{0};
    std::atomic<uint64_t> publish_ring_assign_total_us{0};
    std::atomic<uint64_t> publish_ring_assign_max_us{0};
    std::atomic<uint64_t> publish_ring_advance_count{0};
    std::atomic<uint64_t> publish_ring_advance_total_us{0};
    std::atomic<uint64_t> publish_ring_advance_max_us{0};
    std::atomic<uint64_t> publish_ring_overwrite_count{0};
    std::atomic<uint64_t> publish_ring_overwrite_total_bytes{0};
    std::atomic<uint64_t> publish_ring_overwrite_max_bytes{0};

    std::atomic<uint64_t> flush_count{0};
    std::atomic<uint64_t> flush_total_us{0};
    std::atomic<uint64_t> flush_max_us{0};

    struct Snapshot {
        uint64_t packet_send_count = 0;
        uint64_t packet_send_total_us = 0;
        uint64_t packet_send_max_us = 0;
        uint64_t receive_loop_count = 0;
        uint64_t receive_loop_frame_count = 0;
        uint64_t receive_loop_total_us = 0;
        uint64_t receive_loop_max_us = 0;
        uint64_t convert_count = 0;
        uint64_t convert_total_us = 0;
        uint64_t convert_max_us = 0;
        uint64_t convert_direct_planar_count = 0;
        uint64_t convert_direct_planar_total_us = 0;
        uint64_t convert_direct_planar_max_us = 0;
        uint64_t convert_nv12_layout_count = 0;
        uint64_t convert_nv12_layout_total_us = 0;
        uint64_t convert_nv12_layout_max_us = 0;
        uint64_t convert_nv12_alloc_count = 0;
        uint64_t convert_nv12_alloc_total_us = 0;
        uint64_t convert_nv12_alloc_max_us = 0;
        uint64_t convert_nv12_pack_count = 0;
        uint64_t convert_nv12_pack_total_us = 0;
        uint64_t convert_nv12_pack_max_us = 0;
        uint64_t publish_count = 0;
        uint64_t publish_total_us = 0;
        uint64_t publish_max_us = 0;
        uint64_t publish_lock_count = 0;
        uint64_t publish_lock_total_us = 0;
        uint64_t publish_lock_max_us = 0;
        uint64_t publish_wait_count = 0;
        uint64_t publish_wait_total_us = 0;
        uint64_t publish_wait_max_us = 0;
        uint64_t publish_ring_push_count = 0;
        uint64_t publish_ring_push_total_us = 0;
        uint64_t publish_ring_push_max_us = 0;
        uint64_t publish_ring_lock_count = 0;
        uint64_t publish_ring_lock_total_us = 0;
        uint64_t publish_ring_lock_max_us = 0;
        uint64_t publish_ring_assign_count = 0;
        uint64_t publish_ring_assign_total_us = 0;
        uint64_t publish_ring_assign_max_us = 0;
        uint64_t publish_ring_advance_count = 0;
        uint64_t publish_ring_advance_total_us = 0;
        uint64_t publish_ring_advance_max_us = 0;
        uint64_t publish_ring_overwrite_count = 0;
        uint64_t publish_ring_overwrite_total_bytes = 0;
        uint64_t publish_ring_overwrite_max_bytes = 0;
        uint64_t flush_count = 0;
        uint64_t flush_total_us = 0;
        uint64_t flush_max_us = 0;
    };

    Snapshot snapshot() const {
        return {
            packet_send_count.load(std::memory_order_relaxed),
            packet_send_total_us.load(std::memory_order_relaxed),
            packet_send_max_us.load(std::memory_order_relaxed),
            receive_loop_count.load(std::memory_order_relaxed),
            receive_loop_frame_count.load(std::memory_order_relaxed),
            receive_loop_total_us.load(std::memory_order_relaxed),
            receive_loop_max_us.load(std::memory_order_relaxed),
            convert_count.load(std::memory_order_relaxed),
            convert_total_us.load(std::memory_order_relaxed),
            convert_max_us.load(std::memory_order_relaxed),
            convert_direct_planar_count.load(std::memory_order_relaxed),
            convert_direct_planar_total_us.load(std::memory_order_relaxed),
            convert_direct_planar_max_us.load(std::memory_order_relaxed),
            convert_nv12_layout_count.load(std::memory_order_relaxed),
            convert_nv12_layout_total_us.load(std::memory_order_relaxed),
            convert_nv12_layout_max_us.load(std::memory_order_relaxed),
            convert_nv12_alloc_count.load(std::memory_order_relaxed),
            convert_nv12_alloc_total_us.load(std::memory_order_relaxed),
            convert_nv12_alloc_max_us.load(std::memory_order_relaxed),
            convert_nv12_pack_count.load(std::memory_order_relaxed),
            convert_nv12_pack_total_us.load(std::memory_order_relaxed),
            convert_nv12_pack_max_us.load(std::memory_order_relaxed),
            publish_count.load(std::memory_order_relaxed),
            publish_total_us.load(std::memory_order_relaxed),
            publish_max_us.load(std::memory_order_relaxed),
            publish_lock_count.load(std::memory_order_relaxed),
            publish_lock_total_us.load(std::memory_order_relaxed),
            publish_lock_max_us.load(std::memory_order_relaxed),
            publish_wait_count.load(std::memory_order_relaxed),
            publish_wait_total_us.load(std::memory_order_relaxed),
            publish_wait_max_us.load(std::memory_order_relaxed),
            publish_ring_push_count.load(std::memory_order_relaxed),
            publish_ring_push_total_us.load(std::memory_order_relaxed),
            publish_ring_push_max_us.load(std::memory_order_relaxed),
            publish_ring_lock_count.load(std::memory_order_relaxed),
            publish_ring_lock_total_us.load(std::memory_order_relaxed),
            publish_ring_lock_max_us.load(std::memory_order_relaxed),
            publish_ring_assign_count.load(std::memory_order_relaxed),
            publish_ring_assign_total_us.load(std::memory_order_relaxed),
            publish_ring_assign_max_us.load(std::memory_order_relaxed),
            publish_ring_advance_count.load(std::memory_order_relaxed),
            publish_ring_advance_total_us.load(std::memory_order_relaxed),
            publish_ring_advance_max_us.load(std::memory_order_relaxed),
            publish_ring_overwrite_count.load(std::memory_order_relaxed),
            publish_ring_overwrite_total_bytes.load(std::memory_order_relaxed),
            publish_ring_overwrite_max_bytes.load(std::memory_order_relaxed),
            flush_count.load(std::memory_order_relaxed),
            flush_total_us.load(std::memory_order_relaxed),
            flush_max_us.load(std::memory_order_relaxed),
        };
    }

    void record_packet_send(uint64_t elapsed_us) {
        record(packet_send_count, packet_send_total_us, packet_send_max_us, elapsed_us);
    }

    void record_receive_loop(uint64_t elapsed_us, uint64_t frames) {
        receive_loop_count.fetch_add(1, std::memory_order_relaxed);
        receive_loop_frame_count.fetch_add(frames, std::memory_order_relaxed);
        receive_loop_total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
        update_max(receive_loop_max_us, elapsed_us);
    }

    void record_convert(uint64_t elapsed_us) {
        record(convert_count, convert_total_us, convert_max_us, elapsed_us);
    }

    void record_convert_direct_planar(uint64_t elapsed_us) {
        record(convert_direct_planar_count,
               convert_direct_planar_total_us,
               convert_direct_planar_max_us,
               elapsed_us);
    }

    void record_convert_nv12_layout(uint64_t elapsed_us) {
        record(convert_nv12_layout_count,
               convert_nv12_layout_total_us,
               convert_nv12_layout_max_us,
               elapsed_us);
    }

    void record_convert_nv12_alloc(uint64_t elapsed_us) {
        record(convert_nv12_alloc_count,
               convert_nv12_alloc_total_us,
               convert_nv12_alloc_max_us,
               elapsed_us);
    }

    void record_convert_nv12_pack(uint64_t elapsed_us) {
        record(convert_nv12_pack_count,
               convert_nv12_pack_total_us,
               convert_nv12_pack_max_us,
               elapsed_us);
    }

    void record_publish(uint64_t elapsed_us) {
        record(publish_count, publish_total_us, publish_max_us, elapsed_us);
    }

    void record_publish_lock(uint64_t elapsed_us) {
        record(publish_lock_count,
               publish_lock_total_us,
               publish_lock_max_us,
               elapsed_us);
    }

    void record_publish_wait(uint64_t elapsed_us) {
        record(publish_wait_count,
               publish_wait_total_us,
               publish_wait_max_us,
               elapsed_us);
    }

    void record_publish_ring_push(uint64_t elapsed_us) {
        record(publish_ring_push_count,
               publish_ring_push_total_us,
               publish_ring_push_max_us,
               elapsed_us);
    }

    void record_publish_ring_lock(uint64_t elapsed_us) {
        record(publish_ring_lock_count,
               publish_ring_lock_total_us,
               publish_ring_lock_max_us,
               elapsed_us);
    }

    void record_publish_ring_assign(uint64_t elapsed_us) {
        record(publish_ring_assign_count,
               publish_ring_assign_total_us,
               publish_ring_assign_max_us,
               elapsed_us);
    }

    void record_publish_ring_advance(uint64_t elapsed_us) {
        record(publish_ring_advance_count,
               publish_ring_advance_total_us,
               publish_ring_advance_max_us,
               elapsed_us);
    }

    void record_publish_ring_overwrite_bytes(uint64_t bytes) {
        publish_ring_overwrite_count.fetch_add(1, std::memory_order_relaxed);
        publish_ring_overwrite_total_bytes.fetch_add(bytes, std::memory_order_relaxed);
        update_max(publish_ring_overwrite_max_bytes, bytes);
    }

    void record_flush(uint64_t elapsed_us) {
        record(flush_count, flush_total_us, flush_max_us, elapsed_us);
    }

private:
    static void record(std::atomic<uint64_t>& count,
                       std::atomic<uint64_t>& total_us,
                       std::atomic<uint64_t>& max_us,
                       uint64_t elapsed_us) {
        count.fetch_add(1, std::memory_order_relaxed);
        total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
        update_max(max_us, elapsed_us);
    }

    static void update_max(std::atomic<uint64_t>& target, uint64_t value) {
        uint64_t current = target.load(std::memory_order_relaxed);
        while (value > current &&
               !target.compare_exchange_weak(current,
                                             value,
                                             std::memory_order_relaxed)) {}
    }
};

} // namespace vr
