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

    void require_pipeline_invariants_strict(const Metrics& m, std::size_t delivered_size) {
        OPS_REQUIRE(m.delivered_count <= m.packed_count);
        OPS_REQUIRE(m.packed_count <= m.prepared_count);
        OPS_REQUIRE(m.prepared_count <= m.accepted_count);

        OPS_REQUIRE(static_cast<std::size_t>(m.delivered_count) == delivered_size);

        OPS_REQUIRE(m.q_in_push == m.q_in_pop);
        OPS_REQUIRE(m.q_prepare_push == m.q_prepare_pop);
        OPS_REQUIRE(m.q_pack_push == m.q_pack_pop);

        OPS_REQUIRE(m.q_in_push == m.accepted_count);
        OPS_REQUIRE(m.q_in_pop == m.accepted_count);

        OPS_REQUIRE(m.q_prepare_push == m.prepared_count);
        OPS_REQUIRE(m.q_prepare_pop == m.prepared_count);

        OPS_REQUIRE(m.q_pack_push == m.packed_count);
        OPS_REQUIRE(m.q_pack_pop == m.packed_count);
    }

} // namespace

OPS_TEST("Stage02 Queue: wait_pop blocks until push then returns element") {
    BlockingQueue<int> q;

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

    q.push(42);

    OPS_REQUIRE(fut.wait_for(2s) == std::future_status::ready);
    OPS_REQUIRE(fut.get() == 42);
}

OPS_TEST("Stage02 Queue: wait_pop_for times out when no data") {
    BlockingQueue<int> q;

    int out = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = q.wait_pop_for(out, 80ms);
    const auto t1 = std::chrono::steady_clock::now();

    OPS_REQUIRE(!ok);

    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    OPS_REQUIRE(waited_ms >= 40);
}

OPS_TEST("Stage02 Queue: close wakes wait_pop and it returns false when empty") {
    BlockingQueue<int> q;

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

OPS_TEST("Stage02 Queue: push after close throws logic_error") {
    BlockingQueue<int> q;
    q.close();

    OPS_REQUIRE(throws_logic_error([&] {
        q.push(1);
        }));
}

OPS_TEST("Stage02 Queue: close is idempotent") {
    BlockingQueue<int> q;
    q.close();
    q.close();
    q.close();

    OPS_REQUIRE(q.closed());

    int out = 0;
    OPS_REQUIRE(!q.wait_pop_for(out, 10ms));
}

OPS_TEST("Stage02 Queue: wait_pop_for does not spin (practical anti-busy-wait)") {
    BlockingQueue<int> q;

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

OPS_TEST("Stage02 Pipeline: basic flow delivers all orders and metrics match strictly") {
    Pipeline p;
    p.start();

    const std::size_t total = 3000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.shutdown();

    const auto m = p.metrics();
    const auto& delivered = p.delivered_orders();

    OPS_REQUIRE(delivered.size() == total);
    require_all_unique(extract_ids(delivered));

    OPS_REQUIRE(m.accepted_count == total);
    OPS_REQUIRE(m.prepared_count == total);
    OPS_REQUIRE(m.packed_count == total);
    OPS_REQUIRE(m.delivered_count == total);

    require_pipeline_invariants_strict(m, delivered.size());

    OPS_REQUIRE(m.total_lead_time == sum_lead_time_from_orders(delivered));
}

OPS_TEST("Stage02 Pipeline: concurrent submit from multiple threads") {
    Pipeline p;
    p.start();

    const std::size_t producers = 8;
    const std::size_t per_thread = 800;
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

    p.shutdown();

    const auto m = p.metrics();
    const auto& delivered = p.delivered_orders();

    OPS_REQUIRE(delivered.size() == total);
    require_all_unique(extract_ids(delivered));

    OPS_REQUIRE(m.accepted_count == total);
    OPS_REQUIRE(m.delivered_count == total);

    require_pipeline_invariants_strict(m, delivered.size());
    OPS_REQUIRE(m.total_lead_time == sum_lead_time_from_orders(delivered));
}

OPS_TEST("Stage02 Pipeline: shutdown completes (no deadlock)") {
    Pipeline p;
    p.start();

    for (std::size_t i = 0; i < 7000; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    std::promise<void> done;
    auto fut = done.get_future();

    std::jthread t([&] {
        p.shutdown();
        done.set_value();
        });

    OPS_REQUIRE(fut.wait_for(3s) == std::future_status::ready);

    const auto m = p.metrics();
    const auto& delivered = p.delivered_orders();
    require_pipeline_invariants_strict(m, delivered.size());
}

OPS_TEST("Stage02 Pipeline: shutdown is idempotent and state is stable after shutdown") {
    Pipeline p;
    p.start();

    const std::size_t total = 2000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.shutdown();
    const auto m1 = p.metrics();
    const auto delivered1 = p.delivered_orders();

    OPS_REQUIRE(delivered1.size() == total);
    require_pipeline_invariants_strict(m1, delivered1.size());

    p.shutdown();

    const auto m2 = p.metrics();
    const auto delivered2 = p.delivered_orders();

    OPS_REQUIRE(delivered2.size() == delivered1.size());

    OPS_REQUIRE(m2.accepted_count == m1.accepted_count);
    OPS_REQUIRE(m2.prepared_count == m1.prepared_count);
    OPS_REQUIRE(m2.packed_count == m1.packed_count);
    OPS_REQUIRE(m2.delivered_count == m1.delivered_count);

    OPS_REQUIRE(m2.q_in_push == m1.q_in_push);
    OPS_REQUIRE(m2.q_in_pop == m1.q_in_pop);
    OPS_REQUIRE(m2.q_prepare_push == m1.q_prepare_push);
    OPS_REQUIRE(m2.q_prepare_pop == m1.q_prepare_pop);
    OPS_REQUIRE(m2.q_pack_push == m1.q_pack_push);
    OPS_REQUIRE(m2.q_pack_pop == m1.q_pack_pop);

    OPS_REQUIRE(m2.total_lead_time == m1.total_lead_time);

    for (int i = 0; i < 200; ++i) {
        const auto mi = p.metrics();
        OPS_REQUIRE(mi.delivered_count == m1.delivered_count);
        OPS_REQUIRE(mi.total_lead_time == m1.total_lead_time);
    }
}

OPS_TEST("Stage02 Pipeline: submit after shutdown is rejected") {
    Pipeline p;
    p.start();

    p.submit(Order{ 1 });
    p.shutdown();

    OPS_REQUIRE(throws_logic_error([&] {
        p.submit(Order{ 2 });
        }));
}

OPS_TEST("Stage02 Pipeline: total_lead_time equals sum(delivered_time - accepted_time)") {
    Pipeline p;
    p.start();

    const std::size_t total = 5000;
    for (std::size_t i = 0; i < total; ++i) {
        p.submit(Order{ static_cast<OrderId>(i + 1) });
    }

    p.shutdown();

    const auto m = p.metrics();
    const auto& delivered = p.delivered_orders();

    OPS_REQUIRE(delivered.size() == total);
    const auto sum = sum_lead_time_from_orders(delivered);
    OPS_REQUIRE(m.total_lead_time == sum);
}

OPS_TEST("Stage02 Pipeline: destructor is safe without explicit shutdown") {
    {
        Pipeline p;
        p.start();

        const std::size_t total = 4000;
        for (std::size_t i = 0; i < total; ++i) {
            p.submit(Order{ static_cast<OrderId>(i + 1) });
        }
    }

    OPS_REQUIRE(true);
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}