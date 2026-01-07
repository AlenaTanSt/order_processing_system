#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include "test_framework.hpp"

#include "order.hpp"
#include "queue.hpp"
#include "pipeline.hpp"
#include "metrics.hpp"

namespace {

    using namespace std::chrono_literals;

    template <class F>
    bool throws_logic_error(F&& fn) {
        try {
            fn();
            return false;
        }
        catch (const std::logic_error&) {
            return true;
        }
        catch (...) {
            return false;
        }
    }

    template <class F>
    bool throws_runtime_error(F&& fn) {
        try {
            fn();
            return false;
        }
        catch (const std::runtime_error&) {
            return true;
        }
        catch (...) {
            return false;
        }
    }

    std::vector<std::uint64_t> extract_ids(const std::vector<Order>& orders) {
        std::vector<std::uint64_t> ids;
        ids.reserve(orders.size());
        for (const auto& o : orders) ids.push_back(o.id);
        return ids;
    }

    void require_all_unique(const std::vector<std::uint64_t>& ids) {
        std::unordered_set<std::uint64_t> s;
        s.reserve(ids.size());
        for (auto id : ids) {
            OPS_REQUIRE(s.insert(id).second);
        }
    }

    std::chrono::nanoseconds sum_lead_time_from_orders(const std::vector<Order>& delivered) {
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

    void require_shutdown_invariants_strict(const Pipeline::Config& cfg,
        const Metrics& m,
        std::size_t delivered_size) {
        OPS_REQUIRE(m.delivered_count <= m.packed_count);
        OPS_REQUIRE(m.packed_count <= m.prepared_count);
        OPS_REQUIRE(m.prepared_count <= m.accepted_count);

        OPS_REQUIRE(m.q_in_push == m.q_in_pop);
        OPS_REQUIRE(m.q_prepare_push == m.q_prepare_pop);
        OPS_REQUIRE(m.q_pack_push == m.q_pack_pop);

        OPS_REQUIRE(m.q_in_push == m.accepted_count);
        OPS_REQUIRE(m.q_in_pop == m.accepted_count);

        OPS_REQUIRE(m.q_prepare_push == m.prepared_count);
        OPS_REQUIRE(m.q_prepare_pop == m.prepared_count);

        OPS_REQUIRE(m.q_pack_push == m.packed_count);
        OPS_REQUIRE(m.q_pack_pop == m.packed_count);

        OPS_REQUIRE(static_cast<std::size_t>(m.delivered_count) == delivered_size);

        OPS_REQUIRE(m.prepare_workers_used == cfg.prepare_workers);
        OPS_REQUIRE(m.pack_workers_used == cfg.pack_workers);
        OPS_REQUIRE(m.deliver_workers_used == cfg.deliver_workers);

        OPS_REQUIRE(m.q_in_max_size <= cfg.q_in_capacity);
        OPS_REQUIRE(m.q_prepare_max_size <= cfg.q_prepare_capacity);
        OPS_REQUIRE(m.q_pack_max_size <= cfg.q_pack_capacity);
    }

} // namespace


OPS_TEST("Stage03 Queue: push_for times out when queue is full") {
    BoundedBlockingQueue<int> q(2);

    OPS_REQUIRE(q.push(1));
    OPS_REQUIRE(q.push(2));
    OPS_REQUIRE(q.size() == 2);

    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = q.push_for(3, 80ms);
    const auto t1 = std::chrono::steady_clock::now();

    OPS_REQUIRE(!ok);

    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    OPS_REQUIRE(waited_ms >= 40);
}

OPS_TEST("Stage03 Queue: wait_pop blocks until push then returns element") {
    BoundedBlockingQueue<int> q(4);

    std::promise<int> got;
    auto fut = got.get_future();

    std::jthread consumer([&] {
        int x = 0;
        const bool ok = q.wait_pop(x);
        OPS_REQUIRE(ok);
        got.set_value(x);
        });

    std::this_thread::sleep_for(30ms);
    OPS_REQUIRE(fut.wait_for(0ms) == std::future_status::timeout);

    OPS_REQUIRE(q.push(42));

    OPS_REQUIRE(fut.wait_for(2s) == std::future_status::ready);
    OPS_REQUIRE(fut.get() == 42);
}

OPS_TEST("Stage03 Queue: wait_pop_for times out when empty") {
    BoundedBlockingQueue<int> q(4);

    int out = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = q.wait_pop_for(out, 80ms);
    const auto t1 = std::chrono::steady_clock::now();

    OPS_REQUIRE(!ok);

    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    OPS_REQUIRE(waited_ms >= 40);
}

OPS_TEST("Stage03 Queue: close wakes wait_pop and it returns false when empty") {
    BoundedBlockingQueue<int> q(4);

    std::promise<bool> done;
    auto fut = done.get_future();

    std::jthread consumer([&] {
        int x = 0;
        const bool ok = q.wait_pop(x);
        done.set_value(ok);
        });

    std::this_thread::sleep_for(30ms);
    q.close();

    OPS_REQUIRE(fut.wait_for(2s) == std::future_status::ready);
    OPS_REQUIRE(fut.get() == false);

    OPS_REQUIRE(q.closed());
}

OPS_TEST("Stage03 Queue: close wakes push that waits for space and push returns false") {
    BoundedBlockingQueue<int> q(1);
    OPS_REQUIRE(q.push(1));

    std::promise<bool> push_ok;
    auto fut = push_ok.get_future();

    std::jthread producer([&] {
        const bool ok = q.push(2);
        push_ok.set_value(ok);
        });

    std::this_thread::sleep_for(30ms);
    q.close();

    OPS_REQUIRE(fut.wait_for(2s) == std::future_status::ready);
    OPS_REQUIRE(fut.get() == false);
}

OPS_TEST("Stage03 Queue: push returns false after close") {
    BoundedBlockingQueue<int> q(4);
    q.close();
    OPS_REQUIRE(!q.push(1));
}

OPS_TEST("Stage03 Queue: no spin in wait_pop_for (practical anti-busy-wait)") {
    BoundedBlockingQueue<int> q(4);

    int out = 0;
    int returns = 0;

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 250ms) {
        const bool ok = q.wait_pop_for(out, 50ms);
        OPS_REQUIRE(!ok);
        ++returns;
    }

    OPS_REQUIRE(returns <= 20);
}

OPS_TEST("Stage03 Pipeline: start called twice throws logic_error") {
    Pipeline::Config cfg;
    Pipeline p(cfg);

    p.start();
    OPS_REQUIRE(throws_logic_error([&] {
        p.start();
        }));

    p.cancel();
}

OPS_TEST("Stage03 Pipeline: submit before start is allowed (then start+shutdown delivers)") {
    Pipeline::Config cfg;
    cfg.q_in_capacity = 256;
    cfg.q_prepare_capacity = 256;
    cfg.q_pack_capacity = 256;
    cfg.prepare_workers = 2;
    cfg.pack_workers = 2;
    cfg.deliver_workers = 2;
    cfg.push_timeout = 200ms;
    cfg.pop_timeout = 20ms;

    Pipeline p(cfg);

    const std::size_t total = 2000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.start();
    p.shutdown();

    const auto m = p.metrics();
    const auto& delivered = p.delivered_orders();

    OPS_REQUIRE(delivered.size() == total);
    require_all_unique(extract_ids(delivered));

    OPS_REQUIRE(m.accepted_count == total);
    OPS_REQUIRE(m.delivered_count == total);

    require_shutdown_invariants_strict(cfg, m, delivered.size());
    OPS_REQUIRE(m.total_lead_time == sum_lead_time_from_orders(delivered));
}

OPS_TEST("Stage03 Pipeline: submit backpressure timeout is deterministic before start") {
    Pipeline::Config cfg;
    cfg.q_in_capacity = 2;
    cfg.q_prepare_capacity = 2;
    cfg.q_pack_capacity = 2;

    cfg.prepare_workers = 1;
    cfg.pack_workers = 1;
    cfg.deliver_workers = 1;

    cfg.push_timeout = 30ms;
    cfg.pop_timeout = 20ms;

    Pipeline p(cfg);

    p.submit(Order{ 1 });
    p.submit(Order{ 2 });

    OPS_REQUIRE(throws_runtime_error([&] {
        p.submit(Order{ 3 });
        }));

    const auto m = p.metrics();
    OPS_REQUIRE(m.submit_timeout_count >= 1);

    p.start();
    p.shutdown();
}

OPS_TEST("Stage03 Pipeline: shutdown delivers all accepted and satisfies strict invariants") {
    Pipeline::Config cfg;
    cfg.q_in_capacity = 128;
    cfg.q_prepare_capacity = 128;
    cfg.q_pack_capacity = 128;

    cfg.prepare_workers = 2;
    cfg.pack_workers = 2;
    cfg.deliver_workers = 2;

    cfg.push_timeout = 100ms;
    cfg.pop_timeout = 20ms;

    Pipeline p(cfg);
    p.start();

    const std::size_t total = 5000;
    std::size_t accepted = 0;

    for (std::size_t i = 0; i < total; ++i) {
        try {
            p.submit(Order{ static_cast<OrderId>(i + 1) });
            ++accepted;
        }
        catch (const std::runtime_error&) {
        }
    }

    p.shutdown();

    const auto m = p.metrics();
    const auto& delivered = p.delivered_orders();

    OPS_REQUIRE(static_cast<std::size_t>(m.accepted_count) == accepted);
    OPS_REQUIRE(m.delivered_count == m.accepted_count);
    OPS_REQUIRE(delivered.size() == static_cast<std::size_t>(m.delivered_count));

    require_all_unique(extract_ids(delivered));
    require_shutdown_invariants_strict(cfg, m, delivered.size());

    OPS_REQUIRE(m.total_lead_time == sum_lead_time_from_orders(delivered));
}

OPS_TEST("Stage03 Pipeline: submit after shutdown throws runtime_error") {
    Pipeline::Config cfg;
    cfg.q_in_capacity = 8;
    cfg.q_prepare_capacity = 8;
    cfg.q_pack_capacity = 8;
    cfg.push_timeout = 50ms;
    cfg.pop_timeout = 20ms;

    Pipeline p(cfg);
    p.start();

    p.submit(Order{ 1 });
    p.shutdown();

    OPS_REQUIRE(throws_runtime_error([&] {
        p.submit(Order{ 2 });
        }));
}

OPS_TEST("Stage03 Pipeline: metrics and delivered are stable after shutdown") {
    Pipeline::Config cfg;
    cfg.q_in_capacity = 128;
    cfg.q_prepare_capacity = 128;
    cfg.q_pack_capacity = 128;
    cfg.prepare_workers = 2;
    cfg.pack_workers = 2;
    cfg.deliver_workers = 2;
    cfg.push_timeout = 200ms;
    cfg.pop_timeout = 20ms;

    Pipeline p(cfg);
    p.start();

    const std::size_t total = 3000;
    for (std::size_t i = 0; i < total; ++i) {
        try {
            p.submit(Order{ static_cast<OrderId>(i + 1) });
        }
        catch (const std::runtime_error&) {
        }
    }

    p.shutdown();

    const auto m0 = p.metrics();
    const auto& d0 = p.delivered_orders();
    const auto d0_size = d0.size();

    for (int i = 0; i < 200; ++i) {
        const auto mi = p.metrics();
        const auto& di = p.delivered_orders();

        OPS_REQUIRE(mi.accepted_count == m0.accepted_count);
        OPS_REQUIRE(mi.prepared_count == m0.prepared_count);
        OPS_REQUIRE(mi.packed_count == m0.packed_count);
        OPS_REQUIRE(mi.delivered_count == m0.delivered_count);

        OPS_REQUIRE(mi.q_in_push == m0.q_in_push);
        OPS_REQUIRE(mi.q_in_pop == m0.q_in_pop);
        OPS_REQUIRE(mi.q_prepare_push == m0.q_prepare_push);
        OPS_REQUIRE(mi.q_prepare_pop == m0.q_prepare_pop);
        OPS_REQUIRE(mi.q_pack_push == m0.q_pack_push);
        OPS_REQUIRE(mi.q_pack_pop == m0.q_pack_pop);

        OPS_REQUIRE(mi.total_lead_time == m0.total_lead_time);
        OPS_REQUIRE(di.size() == d0_size);
    }
}

OPS_TEST("Stage03 Pipeline: cancel completes quickly (no deadlock)") {
    Pipeline::Config cfg;
    cfg.q_in_capacity = 64;
    cfg.q_prepare_capacity = 64;
    cfg.q_pack_capacity = 64;
    cfg.prepare_workers = 2;
    cfg.pack_workers = 2;
    cfg.deliver_workers = 2;
    cfg.push_timeout = 50ms;
    cfg.pop_timeout = 20ms;

    Pipeline p(cfg);
    p.start();

    for (std::size_t i = 0; i < 20000; ++i) {
        try {
            p.submit(Order{ static_cast<OrderId>(i + 1) });
        }
        catch (const std::runtime_error&) {
        }
    }

    std::promise<void> done;
    auto fut = done.get_future();

    std::jthread t([&] {
        p.cancel();
        done.set_value();
        });

    OPS_REQUIRE(fut.wait_for(2s) == std::future_status::ready);

    const auto m = p.metrics();
    const auto& d = p.delivered_orders();
    require_all_unique(extract_ids(d));
    OPS_REQUIRE(static_cast<std::size_t>(m.delivered_count) == d.size());
}

OPS_TEST("Stage03 Pipeline: destructor is safe without explicit shutdown/cancel") {
    {
        Pipeline::Config cfg;
        cfg.q_in_capacity = 64;
        cfg.q_prepare_capacity = 64;
        cfg.q_pack_capacity = 64;
        cfg.prepare_workers = 2;
        cfg.pack_workers = 2;
        cfg.deliver_workers = 2;
        cfg.push_timeout = 50ms;
        cfg.pop_timeout = 20ms;

        Pipeline p(cfg);
        p.start();

        for (std::size_t i = 0; i < 10000; ++i) {
            try {
                p.submit(Order{ static_cast<OrderId>(i + 1) });
            }
            catch (const std::runtime_error&) {
            }
        }
    }

    OPS_REQUIRE(true);
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}