#include <catch2/catch_test_macros.hpp>
#include "media/packet_queue.h"
#include <thread>
#include <chrono>
#include <deque>
#include <random>

using namespace vr;

namespace {

AVPacket* make_packet(int64_t pts) {
    auto* pkt = av_packet_alloc();
    REQUIRE(pkt != nullptr);
    pkt->pts = pts;
    return pkt;
}

} // namespace

TEST_CASE("PacketQueue: push and pop preserves order", "[packet_queue]") {
    PacketQueue pq(10);

    for (int i = 0; i < 3; ++i) {
        auto* pkt = av_packet_alloc();
        pkt->pts = i * 1000;
        pq.push(pkt);
    }

    for (int i = 0; i < 3; ++i) {
        auto result = pq.pop();
        REQUIRE(result.status == PacketPopStatus::Packet);
        auto* pkt = result.packet;
        REQUIRE(pkt != nullptr);
        REQUIRE(pkt->pts == i * 1000);
        av_packet_free(&pkt);
    }
}

TEST_CASE("PacketQueue: default capacity comes from native resource budget",
          "[packet_queue]") {
    PacketQueue pq;
    REQUIRE(pq.capacity() == default_native_resource_budget().packet_queue_capacity);
}

TEST_CASE("PacketQueue: capacity enforced", "[packet_queue]") {
    PacketQueue pq(2);
    auto* p1 = av_packet_alloc(); p1->pts = 1;
    auto* p2 = av_packet_alloc(); p2->pts = 2;
    pq.push(p1);
    pq.push(p2);
    REQUIRE(pq.size() == 2);

    // Third push should block; use try_pop to drain
    auto pop_result = pq.try_pop();
    REQUIRE(pop_result.status == PacketPopStatus::Packet);
    auto* popped = pop_result.packet;
    REQUIRE(popped != nullptr);
    av_packet_free(&popped);
}

TEST_CASE("PacketQueue: try_pop on empty returns nullptr", "[packet_queue]") {
    PacketQueue pq(10);
    auto result = pq.try_pop();
    REQUIRE(result.status == PacketPopStatus::Empty);
    REQUIRE(result.packet == nullptr);
    REQUIRE(pq.empty() == true);
}

TEST_CASE("PacketQueue: try_push returns false when full", "[packet_queue]") {
    PacketQueue pq(1);
    auto* p1 = av_packet_alloc(); p1->pts = 1;
    auto* p2 = av_packet_alloc(); p2->pts = 2;
    REQUIRE(pq.try_push(p1) == true);
    REQUIRE(pq.try_push(p2) == false);
    REQUIRE(pq.size() == 1);
    av_packet_free(&p2);

    auto pop_result = pq.pop();
    REQUIRE(pop_result.status == PacketPopStatus::Packet);
    auto* popped = pop_result.packet;
    REQUIRE(popped != nullptr);
    REQUIRE(popped->pts == 1);
    av_packet_free(&popped);
}

TEST_CASE("PacketQueue: abort wakes popper", "[packet_queue]") {
    PacketQueue pq(10);

    std::thread consumer([&]() {
        auto result = pq.pop();
        REQUIRE(result.status == PacketPopStatus::Aborted);
        REQUIRE(result.packet == nullptr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pq.abort();
    consumer.join();
    REQUIRE(pq.is_aborted() == true);
}

TEST_CASE("PacketQueue: EOF wakes popper", "[packet_queue]") {
    PacketQueue pq(10);

    std::thread consumer([&]() {
        auto result = pq.pop();
        REQUIRE(result.status == PacketPopStatus::Eof);
        REQUIRE(result.packet == nullptr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pq.signal_eof();
    consumer.join();
    REQUIRE(pq.is_eof() == true);
}

TEST_CASE("PacketQueue: flush wakes popper", "[packet_queue]") {
    PacketQueue pq(10);

    std::thread consumer([&]() {
        auto result = pq.pop();
        REQUIRE(result.status == PacketPopStatus::Flushed);
        REQUIRE(result.packet == nullptr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pq.flush();
    consumer.join();
    REQUIRE(pq.is_eof() == false);
}

TEST_CASE("PacketQueue: pop distinguishes abort flush EOF and empty",
          "[packet_queue]") {
    {
        PacketQueue pq(10);
        pq.abort();
        auto result = pq.pop();
        REQUIRE(result.status == PacketPopStatus::Aborted);
        REQUIRE(result.packet == nullptr);
    }

    {
        PacketQueue pq(10);
        pq.flush();
        auto result = pq.try_pop();
        REQUIRE(result.status == PacketPopStatus::Flushed);
        REQUIRE(result.packet == nullptr);
        REQUIRE(pq.try_pop().status == PacketPopStatus::Empty);
    }

    {
        PacketQueue pq(10);
        pq.signal_eof();
        auto result = pq.try_pop();
        REQUIRE(result.status == PacketPopStatus::Eof);
        REQUIRE(result.packet == nullptr);
    }
}

TEST_CASE("PacketQueue: abort wakes pusher", "[packet_queue]") {
    PacketQueue pq(1);
    auto* p1 = av_packet_alloc();
    pq.push(p1);  // fills capacity

    std::thread producer([&]() {
        auto* p2 = av_packet_alloc();
        bool ok = pq.push(p2);
        REQUIRE(ok == false);  // aborted
        av_packet_free(&p2);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pq.abort();
    producer.join();
}

TEST_CASE("PacketQueue: reset after abort", "[packet_queue]") {
    PacketQueue pq(10);
    pq.signal_eof();
    pq.abort();
    REQUIRE(pq.is_aborted() == true);
    REQUIRE(pq.is_eof() == true);

    pq.reset();
    REQUIRE(pq.is_aborted() == false);
    REQUIRE(pq.empty() == true);
    REQUIRE(pq.is_eof() == false);

    // Should be reusable
    auto* pkt = av_packet_alloc();
    pkt->pts = 42;
    pq.push(pkt);
    auto pop_result = pq.pop();
    REQUIRE(pop_result.status == PacketPopStatus::Packet);
    auto* popped = pop_result.packet;
    REQUIRE(popped != nullptr);
    REQUIRE(popped->pts == 42);
    av_packet_free(&popped);
}

TEST_CASE("PacketQueue: flush discards all packets", "[packet_queue]") {
    PacketQueue pq(10);
    for (int i = 0; i < 5; ++i) {
        pq.push(av_packet_alloc());
    }
    pq.signal_eof();
    REQUIRE(pq.size() == 5);
    REQUIRE(pq.is_eof() == true);

    pq.flush();
    REQUIRE(pq.empty() == true);
    REQUIRE(pq.size() == 0);
    REQUIRE(pq.is_eof() == false);
}

TEST_CASE("PacketQueue: deterministic nonblocking sequence preserves FIFO and states",
          "[packet_queue]") {
    constexpr size_t kCapacity = 4;
    PacketQueue pq(kCapacity);
    std::deque<int64_t> expected;
    std::mt19937 rng(0xFACE);
    int64_t next_pts = 0;

    for (int step = 0; step < 500; ++step) {
        const int op = static_cast<int>(rng() % 6);
        if (op <= 1) {
            auto* pkt = make_packet(next_pts);
            const bool ok = pq.try_push(pkt);
            REQUIRE(ok == (expected.size() < kCapacity));
            if (ok) {
                expected.push_back(next_pts);
                ++next_pts;
            } else {
                av_packet_free(&pkt);
            }
        } else if (op == 2) {
            const auto result = pq.try_pop();
            if (expected.empty()) {
                REQUIRE(result.status == PacketPopStatus::Empty);
                REQUIRE(result.packet == nullptr);
            } else {
                REQUIRE(result.status == PacketPopStatus::Packet);
                REQUIRE(result.packet != nullptr);
                REQUIRE(result.packet->pts == expected.front());
                expected.pop_front();
                auto* pkt = result.packet;
                av_packet_free(&pkt);
            }
        } else if (op == 3) {
            pq.flush();
            expected.clear();
            REQUIRE(pq.empty());
            auto result = pq.try_pop();
            REQUIRE(result.status == PacketPopStatus::Flushed);
            REQUIRE(result.packet == nullptr);
            REQUIRE(pq.try_pop().status == PacketPopStatus::Empty);
        } else if (op == 4) {
            pq.signal_eof();
            if (expected.empty()) {
                auto result = pq.try_pop();
                REQUIRE(result.status == PacketPopStatus::Eof);
                REQUIRE(result.packet == nullptr);
            }
            pq.clear_eof();
            REQUIRE_FALSE(pq.is_eof());
        } else {
            pq.abort();
            REQUIRE(pq.is_aborted());
            auto result = pq.try_pop();
            REQUIRE(result.status == PacketPopStatus::Aborted);
            REQUIRE(result.packet == nullptr);
            auto* pkt = make_packet(next_pts);
            REQUIRE_FALSE(pq.try_push(pkt));
            av_packet_free(&pkt);
            pq.reset();
            expected.clear();
            REQUIRE_FALSE(pq.is_aborted());
            REQUIRE(pq.empty());
        }

        REQUIRE(pq.size() == expected.size());
    }
}
