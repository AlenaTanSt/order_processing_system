#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include "order.hpp"
#include "pipeline.hpp"
#include "metrics.hpp"

using namespace std::chrono_literals;

namespace {

    template <typename Pred>
    void wait_until(Pred&& pred, std::chrono::milliseconds timeout = 2500ms) {
        const auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            if (std::chrono::steady_clock::now() - start > timeout) {
                OPS_FAIL("Timeout waiting for condition");
            }
            std::this_thread::yield();
        }
    }

    Order make_order(std::uint64_t id) {
        return Order(static_cast<OrderId>(id));
    }

    struct SubmitStats {
        std::uint64_t accepted = 0;
        std::uint64_t rejected = 0;
    };

    SubmitStats submit_parallel(Pipeline& p, std::size_t total, std::size_t producers) {
        std::atomic<std::uint64_t> next{ 1 };
        std::atomic<std::uint64_t> acc{ 0 }, rej{ 0 };

        std::vector<std::thread> th;
        th.reserve(producers);

        for (std::size_t i = 0; i < producers; ++i) {
            th.emplace_back([&] {
                for (;;) {
                    const auto id = next.fetch_add(1, std::memory_order_relaxed);
                    if (id > total) break;

                    if (p.submit(make_order(id))) acc.fetch_add(1, std::memory_order_relaxed);
                    else rej.fetch_add(1, std::memory_order_relaxed);
                }
                });
        }

        for (auto& t : th) t.join();
        return { acc.load(), rej.load() };
    }

    void require_stage_chain(const Metrics& m) {
        OPS_REQUIRE(m.delivered_count <= m.packed_count);
        OPS_REQUIRE(m.packed_count <= m.prepared_count);
        OPS_REQUIRE(m.prepared_count <= m.accepted_count);
    }

    void require_queue_chain(const Metrics& m) {
        OPS_REQUIRE(m.q_in_pop <= m.q_in_push);
        OPS_REQUIRE(m.q_prepare_pop <= m.q_prepare_push);
        OPS_REQUIRE(m.q_pack_pop <= m.q_pack_push);

        if (m.q_in_push == 0) OPS_REQUIRE(m.q_in_max_size == 0);
        if (m.q_prepare_push == 0) OPS_REQUIRE(m.q_prepare_max_size == 0);
        if (m.q_pack_push == 0) OPS_REQUIRE(m.q_pack_max_size == 0);
    }

    void require_delivered_orders_valid(const std::vector<Order>& delivered) {
        std::vector<OrderId> ids;
        ids.reserve(delivered.size());

        for (const auto& o : delivered) {
            OPS_REQUIRE(o.status == OrderStatus::Delivered);

            OPS_REQUIRE(o.accepted_time <= o.prepared_time);
            OPS_REQUIRE(o.prepared_time <= o.packed_time);
            OPS_REQUIRE(o.packed_time <= o.delivered_time);

            ids.push_back(o.id);
        }

        std::sort(ids.begin(), ids.end());
        OPS_REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    }

} // namespace

OPS_TEST("Stage04: Order advance_to only allows strict step transitions") {
    Order o(OrderId{ 1 });

    OPS_REQUIRE(o.status == OrderStatus::Accepted);

    try {
        o.advance_to(OrderStatus::Prepared);
    }
    catch (...) {
        OPS_FAIL("Expected no exception on Accepted->Prepared");
    }

    bool threw = false;
    try {
        o.advance_to(OrderStatus::Delivered);
    }
    catch (const std::logic_error&) {
        threw = true;
    }
    catch (...) {
        OPS_FAIL("Expected std::logic_error");
    }
    OPS_REQUIRE(threw);

    try {
        o.advance_to(OrderStatus::Packed);
        o.advance_to(OrderStatus::Delivered);
    }
    catch (...) {
        OPS_FAIL("Expected no exception on valid transitions to Delivered");
    }

    OPS_REQUIRE(o.accepted_time <= o.prepared_time);
    OPS_REQUIRE(o.prepared_time <= o.packed_time);
    OPS_REQUIRE(o.packed_time <= o.delivered_time);
}

OPS_TEST("Stage04: initial state is Created, not running, not stopped") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);

    OPS_REQUIRE(p.state() == PipelineState::Created);
    OPS_REQUIRE(!p.is_running());
    OPS_REQUIRE(!p.is_stopped());

    const auto m = p.metrics();
    OPS_REQUIRE(m.accepted_count == 0);
    OPS_REQUIRE(m.prepared_count == 0);
    OPS_REQUIRE(m.packed_count == 0);
    OPS_REQUIRE(m.delivered_count == 0);

    OPS_REQUIRE(m.q_in_push == 0);
    OPS_REQUIRE(m.q_in_pop == 0);
    OPS_REQUIRE(m.q_prepare_push == 0);
    OPS_REQUIRE(m.q_prepare_pop == 0);
    OPS_REQUIRE(m.q_pack_push == 0);
    OPS_REQUIRE(m.q_pack_pop == 0);
}

OPS_TEST("Stage04: start transitions to Running and is idempotent in Running") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);

    p.start();
    OPS_REQUIRE(p.state() == PipelineState::Running);
    OPS_REQUIRE(p.is_running());
    OPS_REQUIRE(!p.is_stopped());

    p.start();
    OPS_REQUIRE(p.state() == PipelineState::Running);

    const auto m = p.metrics();
    OPS_REQUIRE(m.prepare_workers_used > 0);
    OPS_REQUIRE(m.pack_workers_used > 0);
    OPS_REQUIRE(m.deliver_workers_used > 0);

    p.shutdown_now();
}

OPS_TEST("Stage04: start in Stopped throws logic_error") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);

    p.start();
    p.shutdown_now();
    OPS_REQUIRE(p.is_stopped());

    bool threw = false;
    try {
        p.start();
    }
    catch (const std::logic_error&) {
        threw = true;
    }
    catch (...) {
        OPS_FAIL("Expected std::logic_error");
    }
    OPS_REQUIRE(threw);
}

OPS_TEST("Stage04: submit returns false if not Running; true in Running") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);

    OPS_REQUIRE(p.state() == PipelineState::Created);
    OPS_REQUIRE(!p.submit(make_order(1)));

    p.start();
    OPS_REQUIRE(p.submit(make_order(2)));

    p.shutdown();
    OPS_REQUIRE(p.is_stopped());
    OPS_REQUIRE(!p.submit(make_order(3)));
}

OPS_TEST("Stage04: graceful shutdown drains all accepted orders and makes metrics consistent") {
    Pipeline::Config cfg{};
    cfg.q_in_capacity = 20000;
    cfg.q_prepare_capacity = 20000;
    cfg.q_pack_capacity = 20000;

    cfg.prepare_workers = 2;
    cfg.pack_workers = 2;
    cfg.deliver_workers = 2;

    cfg.push_timeout = std::chrono::milliseconds{ 200 };
    cfg.pop_timeout = std::chrono::milliseconds{ 50 };

    Pipeline p(cfg);
    p.start();

    constexpr std::size_t n = 8000;
    for (std::size_t i = 1; i <= n; ++i) {
        OPS_REQUIRE(p.submit(make_order(i)));
    }

    p.shutdown();

    const auto m = p.metrics();

    OPS_REQUIRE(m.accepted_count == n);
    OPS_REQUIRE(m.prepared_count == n);
    OPS_REQUIRE(m.packed_count == n);
    OPS_REQUIRE(m.delivered_count == n);

    require_stage_chain(m);
    require_queue_chain(m);

    OPS_REQUIRE(m.q_in_push == m.accepted_count);
    OPS_REQUIRE(m.q_in_pop == m.prepared_count);

    OPS_REQUIRE(m.q_prepare_push == m.prepared_count);
    OPS_REQUIRE(m.q_prepare_pop == m.packed_count);

    OPS_REQUIRE(m.q_pack_push == m.packed_count);
    OPS_REQUIRE(m.q_pack_pop == m.delivered_count);

    const auto& delivered = p.delivered_orders();
    OPS_REQUIRE(delivered.size() == n);
    require_delivered_orders_valid(delivered);

    OPS_REQUIRE(m.total_lead_time.count() >= 0);
}

OPS_TEST("Stage04: metrics are monotonic under load (snapshots)") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);
    p.start();

    auto fut = std::async(std::launch::async, [&] {
        return submit_parallel(p, 120000, 6);
        });

    wait_until([&] {
        const auto m = p.metrics();
        return m.accepted_count > 0 || m.submit_timeout_count > 0;
        });

    Metrics prev = p.metrics();
    for (int i = 0; i < 300; ++i) {
        const auto cur = p.metrics();

        require_stage_chain(cur);
        require_queue_chain(cur);

        OPS_REQUIRE(cur.accepted_count >= prev.accepted_count);
        OPS_REQUIRE(cur.prepared_count >= prev.prepared_count);
        OPS_REQUIRE(cur.packed_count >= prev.packed_count);
        OPS_REQUIRE(cur.delivered_count >= prev.delivered_count);

        OPS_REQUIRE(cur.submit_timeout_count >= prev.submit_timeout_count);

        OPS_REQUIRE(cur.q_in_max_size >= prev.q_in_max_size);
        OPS_REQUIRE(cur.q_prepare_max_size >= prev.q_prepare_max_size);
        OPS_REQUIRE(cur.q_pack_max_size >= prev.q_pack_max_size);

        prev = cur;
        std::this_thread::yield();
    }

    p.shutdown_now();
    fut.wait();
}

OPS_TEST("Stage04: shutdown is idempotent and does not change final metrics") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);
    p.start();

    for (std::size_t i = 1; i <= 5000; ++i) {
        (void)p.submit(make_order(i));
    }

    p.shutdown();
    const auto m1 = p.metrics();

    p.shutdown();
    const auto m2 = p.metrics();

    OPS_REQUIRE(m2.accepted_count == m1.accepted_count);
    OPS_REQUIRE(m2.prepared_count == m1.prepared_count);
    OPS_REQUIRE(m2.packed_count == m1.packed_count);
    OPS_REQUIRE(m2.delivered_count == m1.delivered_count);

    OPS_REQUIRE(m2.submit_timeout_count == m1.submit_timeout_count);

    OPS_REQUIRE(m2.q_in_push == m1.q_in_push);
    OPS_REQUIRE(m2.q_in_pop == m1.q_in_pop);
    OPS_REQUIRE(m2.q_prepare_push == m1.q_prepare_push);
    OPS_REQUIRE(m2.q_prepare_pop == m1.q_prepare_pop);
    OPS_REQUIRE(m2.q_pack_push == m1.q_pack_push);
    OPS_REQUIRE(m2.q_pack_pop == m1.q_pack_pop);

    OPS_REQUIRE(m2.q_in_max_size == m1.q_in_max_size);
    OPS_REQUIRE(m2.q_prepare_max_size == m1.q_prepare_max_size);
    OPS_REQUIRE(m2.q_pack_max_size == m1.q_pack_max_size);
}

OPS_TEST("Stage04: shutdown_now unblocks producers and stops accepting") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);
    p.start();

    auto fut = std::async(std::launch::async, [&] {
        return submit_parallel(p, 300000, 8);
        });

    wait_until([&] { return p.metrics().accepted_count > 0; });

    p.shutdown_now();

    OPS_REQUIRE(fut.wait_for(2500ms) == std::future_status::ready);
    (void)fut.get();

    OPS_REQUIRE(p.is_stopped());
    OPS_REQUIRE(!p.submit(make_order(999999)));

    const auto m = p.metrics();
    require_stage_chain(m);
    require_queue_chain(m);
}

OPS_TEST("Stage04: backpressure triggers with tiny capacities and short timeout") {
    Pipeline::Config cfg{};
    cfg.q_in_capacity = 1;
    cfg.q_prepare_capacity = 1;
    cfg.q_pack_capacity = 1;

    cfg.prepare_workers = 1;
    cfg.pack_workers = 1;
    cfg.deliver_workers = 1;

    cfg.push_timeout = std::chrono::milliseconds{ 1 };
    cfg.pop_timeout = std::chrono::milliseconds{ 1 };

    Pipeline p(cfg);
    p.start();

    auto stats = submit_parallel(p, 80000, 12);

    p.shutdown_now();

    const auto m = p.metrics();

    OPS_REQUIRE(stats.rejected > 0);
    OPS_REQUIRE(m.submit_timeout_count > 0);
    OPS_REQUIRE(m.submit_timeout_count >= stats.rejected);

    require_stage_chain(m);
    require_queue_chain(m);
}

OPS_TEST("Stage04: backpressure accounting - if submit rejects, submit_timeout_count must increase") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);
    p.start();

    auto stats = submit_parallel(p, 400000, 10);

    p.shutdown_now();

    const auto m = p.metrics();

    if (stats.rejected > 0) {
        OPS_REQUIRE(m.submit_timeout_count > 0);
        OPS_REQUIRE(m.submit_timeout_count >= stats.rejected);
    }
    else {
        require_stage_chain(m);
        require_queue_chain(m);
    }
}

OPS_TEST("Stage04: concurrent read-only calls are safe during heavy submit load") {
    Pipeline::Config cfg{};
    Pipeline p(cfg);
    p.start();

    std::atomic<bool> done{ false };

    auto prod = std::async(std::launch::async, [&] {
        (void)submit_parallel(p, 250000, 8);
        done.store(true, std::memory_order_relaxed);
        });

    auto reader = [&] {
        while (!done.load(std::memory_order_relaxed)) {
            (void)p.state();
            (void)p.is_running();
            (void)p.is_stopped();
            (void)p.metrics();
            (void)p.delivered_orders();
            std::this_thread::yield();
        }
        };

    std::thread r1(reader), r2(reader), r3(reader), r4(reader);

    prod.wait();
    r1.join(); r2.join(); r3.join(); r4.join();

    p.shutdown_now();
}

OPS_TEST("Stage04: destructor does not hang under overload (implicit shutdown_now)") {
    auto fut = std::async(std::launch::async, [] {
        Pipeline::Config cfg{};
        Pipeline p(cfg);
        p.start();
        (void)submit_parallel(p, 300000, 8);
        return true;
        });

    OPS_REQUIRE(fut.wait_for(3000ms) == std::future_status::ready);
    OPS_REQUIRE(fut.get());
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}