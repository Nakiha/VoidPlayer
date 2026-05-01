#include <catch2/catch_test_macros.hpp>
#include "media/packet_queue.h"
#include <thread>
#include <chrono>

using namespace vr;

TEST_CASE("PacketQueue: push and pop preserves order", "[packet_queue]") {
    PacketQueue pq(10);

    for (int i = 0; i < 3; ++i) {
        auto* pkt = av_packet_alloc();
        pkt->pts = i * 1000;
        pq.push(pkt);
    }

    for (int i = 0; i < 3; ++i) {
        auto* pkt = pq.pop();
        REQUIRE(pkt != nullptr);
        REQUIRE(pkt->pts == i * 1000);
        av_packet_free(&pkt);
    }
}

TEST_CASE("PacketQueue: capacity enforced", "[packet_queue]") {
    PacketQueue pq(2);
    auto* p1 = av_packet_alloc(); p1->pts = 1;
    auto* p2 = av_packet_alloc(); p2->pts = 2;
    pq.push(p1);
    pq.push(p2);
    REQUIRE(pq.size() == 2);

    // Third push should block; use try_pop to drain
    auto* popped = pq.try_pop();
    REQUIRE(popped != nullptr);
    av_packet_free(&popped);
}

TEST_CASE("PacketQueue: try_pop on empty returns nullptr", "[packet_queue]") {
    PacketQueue pq(10);
    REQUIRE(pq.try_pop() == nullptr);
    REQUIRE(pq.empty() == true);
}

TEST_CASE("PacketQueue: abort wakes popper", "[packet_queue]") {
    PacketQueue pq(10);

    std::thread consumer([&]() {
        auto* pkt = pq.pop();
        REQUIRE(pkt == nullptr);  // aborted, returns nullptr
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pq.abort();
    consumer.join();
    REQUIRE(pq.is_aborted() == true);
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
    auto* popped = pq.pop();
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
