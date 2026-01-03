#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

#include "ops/pipeline.hpp"
#include "ops/metrics.hpp"
#include "ops/blocking_queue.hpp"

using namespace std::chrono_literals;

namespace {

    template <class ClockPoint>
    static bool nondecreasing(const ClockPoint& a, const ClockPoint& b) {
        return !(b < a);
    }

    static void require_order_invariants(const Order& o) {
        OPS_REQUIRE(nondecreasing(o.accepted_time, o.prepared_time));
        OPS_REQUIRE(nondecreasing(o.prepared_time, o.packed_time));
        OPS_REQUIRE(nondecreasing(o.packed_time, o.delivered_time));
        OPS_REQUIRE(o.status == OrderStatus::Delivered);
    }

    static Order make_order(std::size_t id) {
        return Order{ id };
    }

} // namespace

OPS_TEST("BoundedBlockingQueue: push_blocking blocks when full and continues after pop") {
    BoundedBlockingQueue<int> q(1);

    std::mutex m;
    std::condition_variable cv;
    bool second_push_entered = false;
    bool second_push_done = false;

    OPS_REQUIRE(q.push_blocking(1));

    std::thread producer([&] {
        {
            std::lock_guard<std::mutex> lk(m);
            second_push_entered = true;
        }
        cv.notify_one();

        const bool ok = q.push_blocking(2);
        OPS_REQUIRE(ok);

        {
            std::lock_guard<std::mutex> lk(m);
            second_push_done = true;
        }
        cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(cv.wait_for(lk, 200ms, [&] { return second_push_entered; }));
    }

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(!cv.wait_for(lk, 50ms, [&] { return second_push_done; }));
    }

    const auto a = q.pop_blocking();
    OPS_REQUIRE(a.has_value());
    OPS_REQUIRE(*a == 1);

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(cv.wait_for(lk, 200ms, [&] { return second_push_done; }));
    }

    const auto b = q.pop_blocking();
    OPS_REQUIRE(b.has_value());
    OPS_REQUIRE(*b == 2);

    q.close();
    producer.join();
}

OPS_TEST("BoundedBlockingQueue: pop_blocking blocks when empty and unblocks on push") {
    BoundedBlockingQueue<int> q(4);

    std::mutex m;
    std::condition_variable cv;
    bool pop_started = false;
    bool pop_done = false;

    std::optional<int> got;

    std::thread consumer([&] {
        {
            std::lock_guard<std::mutex> lk(m);
            pop_started = true;
        }
        cv.notify_one();

        got = q.pop_blocking();

        {
            std::lock_guard<std::mutex> lk(m);
            pop_done = true;
        }
        cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(cv.wait_for(lk, 200ms, [&] { return pop_started; }));
    }

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(!cv.wait_for(lk, 50ms, [&] { return pop_done; }));
    }

    OPS_REQUIRE(q.push_blocking(42));

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(cv.wait_for(lk, 200ms, [&] { return pop_done; }));
    }

    OPS_REQUIRE(got.has_value());
    OPS_REQUIRE(*got == 42);

    q.close();
    consumer.join();
}

OPS_TEST("BoundedBlockingQueue: close wakes waiters and makes push fail; pop returns nullopt when empty") {
    BoundedBlockingQueue<int> q(1);

    std::mutex m;
    std::condition_variable cv;
    bool consumer_done = false;

    std::optional<int> got;

    std::thread consumer([&] {
        got = q.pop_blocking();
        {
            std::lock_guard<std::mutex> lk(m);
            consumer_done = true;
        }
        cv.notify_one();
        });

    q.close();

    {
        std::unique_lock<std::mutex> lk(m);
        OPS_REQUIRE(cv.wait_for(lk, 200ms, [&] { return consumer_done; }));
    }
    OPS_REQUIRE(!got.has_value());

    OPS_REQUIRE(!q.push_blocking(1));

    consumer.join();
}

OPS_TEST("Pipeline: process_all is a barrier for orders accepted before call") {
    Pipeline p;

    constexpr int n = 20;
    for (int i = 0; i < n; ++i) {
        Order o = make_order(static_cast<std::size_t>(i));
        p.submit(std::move(o));
    }

    const std::size_t accepted_before = p.metrics().accepted_count.load();

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() >= accepted_before);

    const auto& m = p.metrics();
    OPS_REQUIRE(m.delivered_count.load() >= accepted_before);
    OPS_REQUIRE(m.processed_count.load() >= accepted_before);

    for (std::size_t i = 0; i < accepted_before; ++i) {
        require_order_invariants(out[i]);
    }
}

OPS_TEST("Pipeline: repeated process_all accumulates delivered and metrics") {
    Pipeline p;

    for (int i = 0; i < 5; ++i) {
        Order o = make_order(static_cast<std::size_t>(i));
        p.submit(std::move(o));
    }
    p.process_all();

    for (int i = 0; i < 3; ++i) {
        Order o = make_order(static_cast<std::size_t>(100 + i));
        p.submit(std::move(o));
    }
    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == 8);

    const auto& m = p.metrics();
    OPS_REQUIRE(m.accepted_count.load() == 8);
    OPS_REQUIRE(m.processed_count.load() == 8);
    OPS_REQUIRE(m.delivered_count.load() == 8);

    for (const auto& o : out) {
        require_order_invariants(o);
    }
}

OPS_TEST("Pipeline: concurrent submit + barrier completes and produces all orders") {
    Pipeline p;

    constexpr int threads = 8;
    constexpr int per_thread = 50;
    constexpr int total = threads * per_thread;

    std::vector<std::thread> ts;
    ts.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&p, t] {
            for (int i = 0; i < per_thread; ++i) {
                const std::size_t id = static_cast<std::size_t>(t) * per_thread + static_cast<std::size_t>(i);
                Order o = make_order(id);
                p.submit(std::move(o));
            }
            });
    }

    for (auto& t : ts) {
        t.join();
    }

    const std::size_t accepted_before = p.metrics().accepted_count.load();
    OPS_REQUIRE(accepted_before == static_cast<std::size_t>(total));

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(total));

    for (std::size_t i = 1; i < out.size(); ++i) {
        OPS_REQUIRE(nondecreasing(out[i - 1].accepted_time, out[i].accepted_time));
    }

    for (const auto& o : out) {
        require_order_invariants(o);
    }

    const auto& m = p.metrics();
    OPS_REQUIRE(m.accepted_count.load() == static_cast<std::size_t>(total));
    OPS_REQUIRE(m.processed_count.load() == static_cast<std::size_t>(total));
    OPS_REQUIRE(m.delivered_count.load() == static_cast<std::size_t>(total));
}

OPS_TEST("Pipeline: process_all on empty pipeline returns immediately and does not change metrics") {
    Pipeline p;

    const auto& before = p.metrics();

    p.process_all();

    const auto& after = p.metrics();
    OPS_REQUIRE(after.accepted_count.load() == before.accepted_count.load());
    OPS_REQUIRE(after.processed_count.load() == before.processed_count.load());
    OPS_REQUIRE(after.delivered_count.load() == before.delivered_count.load());
    OPS_REQUIRE(after.total_processing_time == before.total_processing_time);

    OPS_REQUIRE(p.delivered_orders().empty());
}

OPS_TEST("Pipeline: destruction does not hang with pending orders") {
    {
        Pipeline p(4);

        for (int i = 0; i < 100; ++i) {
            Order o = make_order(static_cast<std::size_t>(i));
            p.submit(std::move(o));
        }
    }
    OPS_REQUIRE(true);
}

OPS_TEST("Pipeline: stress concurrent submit (few thousand orders) + invariants") {
    Pipeline p(8);

    constexpr int threads = 16;
    constexpr int per_thread = 250;
    constexpr int total = threads * per_thread;

    std::vector<std::thread> ts;
    ts.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&p, t] {
            for (int i = 0; i < per_thread; ++i) {
                const std::size_t id = static_cast<std::size_t>(t) * per_thread + static_cast<std::size_t>(i);
                Order o = make_order(id);
                p.submit(std::move(o));
            }
            });
    }

    for (auto& t : ts) {
        t.join();
    }

    OPS_REQUIRE(p.metrics().accepted_count.load() == static_cast<std::size_t>(total));

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(total));

    for (const auto& o : out) {
        require_order_invariants(o);
    }

    for (std::size_t i = 1; i < out.size(); ++i) {
        OPS_REQUIRE(nondecreasing(out[i - 1].accepted_time, out[i].accepted_time));
    }
}

OPS_TEST("Pipeline: process_all waits only for snapshot of accepted orders while submits continue") {
    Pipeline p(4);

    constexpr int total = 1000;
    constexpr int snapshot_threshold = 200;

    std::atomic<int> submitted{ 0 };

    std::thread producer([&] {
        for (int i = 0; i < total; ++i) {
            Order o = make_order(static_cast<std::size_t>(i));
            p.submit(std::move(o));
            submitted.fetch_add(1, std::memory_order_release);

            if ((i % 8) == 0) {
                std::this_thread::yield();
            }
        }
        });

    while (submitted.load(std::memory_order_acquire) < snapshot_threshold) {
        std::this_thread::yield();
    }

    const std::size_t accepted_snapshot = p.metrics().accepted_count.load();

    p.process_all();

    OPS_REQUIRE(p.metrics().delivered_count.load() >= accepted_snapshot);
    OPS_REQUIRE(p.delivered_orders().size() >= accepted_snapshot);

    producer.join();
    OPS_REQUIRE(submitted.load(std::memory_order_acquire) == total);

    p.process_all();

    OPS_REQUIRE(p.metrics().accepted_count.load() == static_cast<std::size_t>(total));
    OPS_REQUIRE(p.metrics().delivered_count.load() == static_cast<std::size_t>(total));
    OPS_REQUIRE(p.delivered_orders().size() == static_cast<std::size_t>(total));

    for (const auto& o : p.delivered_orders()) {
        require_order_invariants(o);
    }
}

OPS_TEST("Pipeline: repeated scenario run does not hang and stays consistent") {
    constexpr int rounds = 50;

    for (int r = 0; r < rounds; ++r) {
        Pipeline p(2);

        for (int i = 0; i < 64; ++i) {
            Order o = make_order(static_cast<std::size_t>(i));
            p.submit(std::move(o));
        }
        p.process_all();

        OPS_REQUIRE(p.metrics().accepted_count.load() == 64);
        OPS_REQUIRE(p.metrics().delivered_count.load() == 64);
        OPS_REQUIRE(p.delivered_orders().size() == 64);

        for (int i = 0; i < 32; ++i) {
            Order o = make_order(static_cast<std::size_t>(1000 + i));
            p.submit(std::move(o));
        }
        p.process_all();

        OPS_REQUIRE(p.metrics().accepted_count.load() == 96);
        OPS_REQUIRE(p.metrics().delivered_count.load() == 96);
        OPS_REQUIRE(p.delivered_orders().size() == 96);

        for (const auto& o : p.delivered_orders()) {
            require_order_invariants(o);
        }
    }

    OPS_REQUIRE(true);
}

OPS_TEST("Pipeline: destructor stress (many pipelines) does not hang") {
    constexpr int rounds = 200;

    for (int r = 0; r < rounds; ++r) {
        Pipeline p(1);

        for (int i = 0; i < 50; ++i) {
            Order o = make_order(static_cast<std::size_t>(i));
            p.submit(std::move(o));
            if ((i % 4) == 0) {
                std::this_thread::yield();
            }
        }
    }
    OPS_REQUIRE(true);
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}