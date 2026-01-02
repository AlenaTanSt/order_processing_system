#include "test_framework.hpp"

#include <ops/blocking_queue.hpp>
#include <ops/order.hpp>
#include <ops/pipeline.hpp>

#include <chrono>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std::chrono_literals;

static Order MakeOrder(OrderId v) {
    return Order(v);
}

OPS_TEST("BlockingQueue: push/pop in single thread") {
    BlockingQueue<int> q;

    OPS_REQUIRE(q.push(1));
    OPS_REQUIRE(q.push(2));

    auto a = q.pop_blocking();
    auto b = q.pop_blocking();

    OPS_REQUIRE(a.has_value());
    OPS_REQUIRE(b.has_value());
    OPS_REQUIRE(*a == 1);
    OPS_REQUIRE(*b == 2);

    q.close();
    auto c = q.pop_blocking();
    OPS_REQUIRE(!c.has_value());
}

OPS_TEST("BlockingQueue: pop_blocking unblocks after close and returns nullopt") {
    BlockingQueue<int> q;

    std::optional<int> res;
    std::thread t([&] { res = q.pop_blocking(); });

    std::this_thread::sleep_for(50ms);

    q.close();
    t.join();

    OPS_REQUIRE(!res.has_value());
}

OPS_TEST("BlockingQueue: push returns false after close") {
    BlockingQueue<int> q;

    q.close();
    OPS_REQUIRE(!q.push(42));
}

OPS_TEST("BlockingQueue: multiple consumers drain all items exactly once") {
    BlockingQueue<int> q;

    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        OPS_REQUIRE(q.push(i));
    }
    q.close();

    std::vector<int> a, b, c, d;

    std::thread t1([&] { for (;;) { auto v = q.pop_blocking(); if (!v) break; a.push_back(*v); } });
    std::thread t2([&] { for (;;) { auto v = q.pop_blocking(); if (!v) break; b.push_back(*v); } });
    std::thread t3([&] { for (;;) { auto v = q.pop_blocking(); if (!v) break; c.push_back(*v); } });
    std::thread t4([&] { for (;;) { auto v = q.pop_blocking(); if (!v) break; d.push_back(*v); } });

    t1.join(); t2.join(); t3.join(); t4.join();

    std::vector<int> seen(N, 0);

    auto mark = [&](const std::vector<int>& local) {
        for (int v : local) {
            OPS_REQUIRE_MSG(v >= 0, "value < 0");
            OPS_REQUIRE_MSG(v < N, "value out of range");
            seen[v] += 1;
        }
        };

    mark(a); mark(b); mark(c); mark(d);

    for (int i = 0; i < N; ++i) {
        OPS_REQUIRE_MSG(seen[i] == 1, "missing or duplicated value in queue drain");
    }
}

OPS_TEST("BlockingQueue: close wakes all waiting consumers (no hang)") {
    BlockingQueue<int> q;

    std::optional<int> r1, r2, r3, r4;

    std::thread t1([&] { r1 = q.pop_blocking(); });
    std::thread t2([&] { r2 = q.pop_blocking(); });
    std::thread t3([&] { r3 = q.pop_blocking(); });
    std::thread t4([&] { r4 = q.pop_blocking(); });

    std::this_thread::sleep_for(50ms);

    q.close();

    t1.join(); t2.join(); t3.join(); t4.join();

    OPS_REQUIRE(!r1.has_value());
    OPS_REQUIRE(!r2.has_value());
    OPS_REQUIRE(!r3.has_value());
    OPS_REQUIRE(!r4.has_value());
}

OPS_TEST("Pipeline: process_all does nothing for empty batch") {
    Pipeline p;

    const auto before = p.metrics();
    OPS_REQUIRE(p.delivered_orders().empty());

    p.process_all();

    OPS_REQUIRE(p.delivered_orders().empty());

    const auto after = p.metrics();
    OPS_REQUIRE(after.accepted_count == before.accepted_count);
    OPS_REQUIRE(after.processed_count == before.processed_count);
    OPS_REQUIRE(after.delivered_count == before.delivered_count);
    OPS_REQUIRE(after.total_processing_time == before.total_processing_time);
}

OPS_TEST("Pipeline: submit increments accepted_count") {
    Pipeline p;

    p.submit(MakeOrder(OrderId{ 1 }));
    p.submit(MakeOrder(OrderId{ 2 }));
    p.submit(MakeOrder(OrderId{ 3 }));

    OPS_REQUIRE(p.metrics().accepted_count == 3);
    OPS_REQUIRE(p.delivered_orders().empty());
}

OPS_TEST("Pipeline: delivers all orders and preserves timestamp invariants") {
    Pipeline p;

    constexpr int N = 120;
    for (int i = 0; i < N; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(N));

    for (const auto& o : out) {
        OPS_REQUIRE(o.status == OrderStatus::Delivered);

        OPS_REQUIRE(o.accepted_time <= o.prepared_time);
        OPS_REQUIRE(o.prepared_time <= o.packed_time);
        OPS_REQUIRE(o.packed_time <= o.delivered_time);
    }

    OPS_REQUIRE(p.metrics().processed_count >= static_cast<std::size_t>(N));
    OPS_REQUIRE(p.metrics().delivered_count >= static_cast<std::size_t>(N));
}

OPS_TEST("Pipeline: delivered order matches submission order by id") {
    Pipeline p;

    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(N));

    for (int i = 0; i < N; ++i) {
        OPS_REQUIRE(out[static_cast<std::size_t>(i)].id == OrderId{ static_cast<std::size_t>(i) });
    }
}

OPS_TEST("Pipeline: no lost or duplicated orders (ids are unique and complete)") {
    Pipeline p;

    constexpr std::uint64_t N = 400;
    for (std::uint64_t i = 0; i < N; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(N));

    std::vector<int> seen(static_cast<std::size_t>(N), 0);

    for (const auto& o : out) {
        const auto id = o.id;
        OPS_REQUIRE_MSG(id < N, "id out of expected range");
        seen[static_cast<std::size_t>(id)] += 1;
    }

    for (std::uint64_t i = 0; i < N; ++i) {
        OPS_REQUIRE_MSG(seen[static_cast<std::size_t>(i)] == 1, "missing or duplicated order id");
    }
}

OPS_TEST("Pipeline: total_processing_time equals sum(delivered-accepted) and is cumulative") {
    Pipeline p;

    auto sum_processing = [](const std::vector<Order>& orders) {
        std::chrono::steady_clock::duration sum{};
        for (const auto& o : orders) {
            sum += (o.delivered_time - o.accepted_time);
        }
        return sum;
        };

    // batch1
    for (int i = 0; i < 25; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }
    p.process_all();

    {
        const auto& out = p.delivered_orders();
        OPS_REQUIRE(out.size() == 25);

        const auto expected = sum_processing(out);
        const auto m = p.metrics();

        OPS_REQUIRE_MSG(m.total_processing_time == expected,
            "total_processing_time must equal sum(delivered_time - accepted_time) for all delivered orders");
    }

    // batch2
    for (int i = 25; i < 70; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }
    p.process_all();

    {
        const auto& out = p.delivered_orders();
        OPS_REQUIRE(out.size() == 70);

        const auto expected = sum_processing(out);
        const auto m = p.metrics();

        OPS_REQUIRE_MSG(m.total_processing_time == expected,
            "total_processing_time must be cumulative and equal sum(delivered-accepted) for all delivered orders");
    }
}

OPS_TEST("Pipeline: metrics are cumulative across multiple process_all calls") {
    Pipeline p;

    // batch1
    for (int i = 0; i < 15; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }
    p.process_all();

    const auto m1 = p.metrics();
    OPS_REQUIRE(m1.accepted_count == 15);
    OPS_REQUIRE(m1.processed_count >= 15);
    OPS_REQUIRE(m1.delivered_count >= 15);

    // batch2
    for (int i = 15; i < 55; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }
    p.process_all();

    const auto m2 = p.metrics();
    OPS_REQUIRE(m2.accepted_count == 55);
    OPS_REQUIRE(m2.processed_count >= 55);
    OPS_REQUIRE(m2.delivered_count >= 55);

    OPS_REQUIRE(m2.total_processing_time >= m1.total_processing_time);
    OPS_REQUIRE(p.delivered_orders().size() == 55);
}

OPS_TEST("Pipeline: stability stress - many small batches") {
    Pipeline p;

    int total = 0;
    for (int round = 0; round < 120; ++round) {
        int n = (round % 9) + 1;
        for (int i = 0; i < n; ++i) {
            p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(total + i) }));
        }
        total += n;

        p.process_all();

        OPS_REQUIRE(p.delivered_orders().size() == static_cast<std::size_t>(total));
        OPS_REQUIRE(p.delivered_orders().back().status == OrderStatus::Delivered);
    }

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(total));
    for (int i = 0; i < total; ++i) {
        OPS_REQUIRE(out[static_cast<std::size_t>(i)].id == OrderId{ static_cast<std::size_t>(i) });
        OPS_REQUIRE(out[static_cast<std::size_t>(i)].status == OrderStatus::Delivered);
    }
}

#ifdef OPS_TESTING
OPS_TEST("Pipeline: uses at least two different threads in Deliver stage when N>=2 (via last_worker)") {
    Pipeline p;

    constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        p.submit(MakeOrder(OrderId{ static_cast<std::size_t>(i) }));
    }

    p.process_all();

    const auto& out = p.delivered_orders();
    OPS_REQUIRE(out.size() == static_cast<std::size_t>(N));

    std::unordered_set<std::thread::id> threads;
    threads.reserve(out.size());

    for (const auto& o : out) {
        threads.insert(o.last_worker);
    }

    OPS_REQUIRE_MSG(threads.size() >= 2, "expected at least 2 unique worker thread ids on Delivered stage");
}
#endif

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}