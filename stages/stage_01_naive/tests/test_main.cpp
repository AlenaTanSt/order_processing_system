#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

#include "test_framework.hpp"

#include "order.hpp"
#include "pipeline.hpp"
#include "metrics.hpp"

namespace {

    std::uint32_t expected_worker_threads() {
        const auto hw = std::thread::hardware_concurrency();
        return hw == 0 ? 1u : hw;
    }

    std::vector<std::uint64_t> extract_ids(const std::vector<Order>& orders) {
        std::vector<std::uint64_t> ids;
        ids.reserve(orders.size());
        for (const auto& o : orders) {
            ids.push_back(o.id);
        }
        return ids;
    }

    void require_all_unique(const std::vector<std::uint64_t>& ids) {
        std::unordered_set<std::uint64_t> s;
        s.reserve(ids.size());
        for (auto id : ids) {
            OPS_REQUIRE(s.insert(id).second);
        }
    }

    void require_invariants(const Metrics& m) {
        OPS_REQUIRE(m.delivered_count <= m.processed_count);
        OPS_REQUIRE(m.processed_count <= m.accepted_count);

        OPS_REQUIRE(m.queue_push_count == m.accepted_count);
        OPS_REQUIRE(m.queue_pop_count == m.processed_count);

        OPS_REQUIRE(m.total_processing_time >= std::chrono::nanoseconds{ 0 });
    }

    std::chrono::nanoseconds sum_processing_time_from_orders(const std::vector<Order>& delivered) {
        std::chrono::nanoseconds sum{ 0 };
        for (const auto& o : delivered) {
            OPS_REQUIRE(o.status == OrderStatus::Delivered);
            OPS_REQUIRE(o.delivered_time >= o.accepted_time);

            sum += std::chrono::duration_cast<std::chrono::nanoseconds>(
                o.delivered_time - o.accepted_time
            );
        }
        return sum;
    }

} // namespace

OPS_TEST("Stage01: worker_threads_used equals max(1, hardware_concurrency)") {
    Pipeline p;

    const auto n = expected_worker_threads();
    const std::size_t total = static_cast<std::size_t>(n) * 8u;

    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.process_all();

    const auto m = p.metrics();
    OPS_REQUIRE(m.worker_threads_used == n);
}

OPS_TEST("Stage01: process_all on empty pipeline is allowed and stable") {
    Pipeline p;

    const auto m0 = p.metrics();
    OPS_REQUIRE(m0.accepted_count == 0);
    OPS_REQUIRE(m0.processed_count == 0);
    OPS_REQUIRE(m0.delivered_count == 0);
    OPS_REQUIRE(m0.queue_push_count == 0);
    OPS_REQUIRE(m0.queue_pop_count == 0);

    p.process_all();

    const auto m1 = p.metrics();
    OPS_REQUIRE(m1.accepted_count == 0);
    OPS_REQUIRE(m1.processed_count == 0);
    OPS_REQUIRE(m1.delivered_count == 0);
    OPS_REQUIRE(m1.queue_push_count == 0);
    OPS_REQUIRE(m1.queue_pop_count == 0);
    OPS_REQUIRE(m1.worker_threads_used == expected_worker_threads());

    OPS_REQUIRE(p.delivered_orders().empty());
}

OPS_TEST("Stage01: concurrent submit is safe and no orders are lost or duplicated") {
    Pipeline p;

    const std::size_t producers = 8;
    const std::size_t per_thread = 250;
    const std::size_t total = producers * per_thread;

    std::vector<std::jthread> threads;
    threads.reserve(producers);

    for (std::size_t t = 0; t < producers; ++t) {
        threads.emplace_back([&, t] {
            for (std::size_t i = 0; i < per_thread; ++i) {
                const auto id = static_cast<OrderId>(t * per_thread + i + 1);
                p.submit(Order{ id });
            }
            });
    }

    p.process_all();

    const auto m = p.metrics();
    require_invariants(m);

    OPS_REQUIRE(m.accepted_count == total);
    OPS_REQUIRE(m.queue_push_count == total);
    OPS_REQUIRE(m.processed_count == total);
    OPS_REQUIRE(m.queue_pop_count == total);
    OPS_REQUIRE(m.delivered_count == total);

    const auto delivered = p.delivered_orders();
    OPS_REQUIRE(delivered.size() == total);

    const auto ids = extract_ids(delivered);
    require_all_unique(ids);
}

OPS_TEST("Stage01: delivered_orders size equals delivered_count and total_processing_time matches sum") {
    Pipeline p;

    const std::size_t total = 4000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.process_all();

    const auto m = p.metrics();
    require_invariants(m);

    const auto& delivered = p.delivered_orders();
    OPS_REQUIRE(delivered.size() == static_cast<std::size_t>(m.delivered_count));

    const auto sum = sum_processing_time_from_orders(delivered);
    OPS_REQUIRE(m.total_processing_time == sum);
}

OPS_TEST("Stage01: metrics are stable after process_all returns") {
    Pipeline p;

    const std::size_t total = 2000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.process_all();

    const auto m0 = p.metrics();
    require_invariants(m0);

    for (int i = 0; i < 300; ++i) {
        const auto mi = p.metrics();
        OPS_REQUIRE(mi.accepted_count == m0.accepted_count);
        OPS_REQUIRE(mi.processed_count == m0.processed_count);
        OPS_REQUIRE(mi.delivered_count == m0.delivered_count);
        OPS_REQUIRE(mi.queue_push_count == m0.queue_push_count);
        OPS_REQUIRE(mi.queue_pop_count == m0.queue_pop_count);
        OPS_REQUIRE(mi.total_processing_time == m0.total_processing_time);
        OPS_REQUIRE(mi.worker_threads_used == m0.worker_threads_used);
    }
}

OPS_TEST("Stage01: process_all is idempotent when queue is empty") {
    Pipeline p;

    const std::size_t total = 1500;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.process_all();

    const auto m1 = p.metrics();
    const auto delivered1 = p.delivered_orders();
    OPS_REQUIRE(delivered1.size() == static_cast<std::size_t>(m1.delivered_count));

    p.process_all();

    const auto m2 = p.metrics();
    const auto delivered2 = p.delivered_orders();

    OPS_REQUIRE(m2.accepted_count == m1.accepted_count);
    OPS_REQUIRE(m2.processed_count == m1.processed_count);
    OPS_REQUIRE(m2.delivered_count == m1.delivered_count);
    OPS_REQUIRE(m2.queue_push_count == m1.queue_push_count);
    OPS_REQUIRE(m2.queue_pop_count == m1.queue_pop_count);
    OPS_REQUIRE(m2.total_processing_time == m1.total_processing_time);

    OPS_REQUIRE(delivered2.size() == delivered1.size());
    require_all_unique(extract_ids(delivered2));
}

OPS_TEST("Stage01: submit blocks while process_all is running") {
    Pipeline p;

    const std::size_t total = 10000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    std::atomic<bool> process_started{ false };

    std::jthread processing([&] {
        process_started.store(true, std::memory_order_release);
        p.process_all();
        });

    while (!process_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::promise<void> submitted;
    auto fut = submitted.get_future();

    std::jthread submitter([&] {
        p.submit(Order{ static_cast<OrderId>(9'000'000) });
        submitted.set_value();
        });

    const auto st = fut.wait_for(std::chrono::milliseconds(30));
    OPS_REQUIRE(st == std::future_status::timeout);

    const auto st2 = fut.wait_for(std::chrono::seconds(3));
    OPS_REQUIRE(st2 == std::future_status::ready);

    const auto m1 = p.metrics();
    OPS_REQUIRE(m1.accepted_count == total + 1);
    OPS_REQUIRE(m1.queue_push_count == total + 1);
    OPS_REQUIRE(m1.processed_count == total);
    OPS_REQUIRE(m1.delivered_count == total);

    p.process_all();
    const auto m2 = p.metrics();
    OPS_REQUIRE(m2.processed_count == total + 1);
    OPS_REQUIRE(m2.delivered_count == total + 1);
    OPS_REQUIRE(m2.queue_pop_count == total + 1);
}

OPS_TEST("Stage01: stress multiple runs with random batch sizes") {
    Pipeline p;

    std::mt19937_64 rng(0xDEADBEEF);
    std::uniform_int_distribution<int> producers_dist(2, 6);
    std::uniform_int_distribution<int> per_thread_dist(0, 600);

    std::uint64_t next_id = 1;
    const int runs = 80;

    for (int r = 0; r < runs; ++r) {
        const int producers = producers_dist(rng);
        const int per_thread = per_thread_dist(rng);
        const std::size_t total = static_cast<std::size_t>(producers) * static_cast<std::size_t>(per_thread);

        std::vector<std::jthread> threads;
        threads.reserve(static_cast<std::size_t>(producers));

        std::vector<std::uint64_t> expected;
        expected.reserve(total);

        for (int t = 0; t < producers; ++t) {
            std::vector<std::uint64_t> ids;
            ids.reserve(static_cast<std::size_t>(per_thread));
            for (int i = 0; i < per_thread; ++i) {
                ids.push_back(next_id++);
                expected.push_back(ids.back());
            }

            threads.emplace_back([&, ids = std::move(ids)]() mutable {
                for (auto id_u64 : ids) {
                    p.submit(Order{ static_cast<OrderId>(id_u64) });
                }
                });
        }

        p.process_all();

        const auto m = p.metrics();
        require_invariants(m);

        const auto delivered = p.delivered_orders();
        std::unordered_set<std::uint64_t> delivered_ids;
        delivered_ids.reserve(delivered.size());
        for (const auto& o : delivered) {
            delivered_ids.insert(o.id);
        }

        for (auto id : expected) {
#if __cplusplus >= 202002L
            OPS_REQUIRE(delivered_ids.contains(id));
#else
            OPS_REQUIRE(delivered_ids.find(id) != delivered_ids.end());
#endif
        }

        require_all_unique(extract_ids(delivered));
    }
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}