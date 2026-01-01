#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <unordered_set>
#include <vector>
#include <thread>

#include "ops/metrics.hpp"
#include "ops/order.hpp"
#include "ops/pipeline.hpp"

namespace {

    using duration_t = std::chrono::steady_clock::duration;

    void require_monotonic_timestamps(const Order& o) {
        OPS_REQUIRE(o.accepted_time <= o.prepared_time);
        OPS_REQUIRE(o.prepared_time <= o.packed_time);
        OPS_REQUIRE(o.packed_time <= o.delivered_time);
    }

    duration_t sum_processing_time(const std::vector<Order>& delivered) {
        duration_t total{};
        for (const auto& o : delivered) {
            total += (o.delivered_time - o.accepted_time);
        }
        return total;
    }

} // namespace

OPS_TEST("process_all: empty queue is a no-op (no threads, no results, no metric changes)") {
    Pipeline pipeline;

    const Metrics before = pipeline.metrics();
    const auto delivered_before = pipeline.delivered_orders().size();

    pipeline.process_all();

    const Metrics after = pipeline.metrics();
    const auto delivered_after = pipeline.delivered_orders().size();

    OPS_REQUIRE(after.accepted_count == before.accepted_count);
    OPS_REQUIRE(after.processed_count == before.processed_count);
    OPS_REQUIRE(after.delivered_count == before.delivered_count);
    OPS_REQUIRE(after.total_processing_time == before.total_processing_time);
    OPS_REQUIRE(delivered_after == delivered_before);
}

OPS_TEST("submit: increments accepted_count; other metrics unchanged until process_all") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });
    pipeline.submit(Order{ 3 });

    const Metrics m = pipeline.metrics();
    OPS_REQUIRE(m.accepted_count == 3);
    OPS_REQUIRE(m.processed_count == 0);
    OPS_REQUIRE(m.delivered_count == 0);
    OPS_REQUIRE(m.total_processing_time == duration_t::zero());

    OPS_REQUIRE(pipeline.delivered_orders().empty());
}

OPS_TEST("process_all: one order becomes Delivered; timestamps are monotonic; metrics updated") {
    Pipeline pipeline;

    pipeline.submit(Order{ 42 });
    pipeline.process_all();

    const auto& delivered = pipeline.delivered_orders();
    OPS_REQUIRE(delivered.size() == 1);
    OPS_REQUIRE(delivered[0].id == 42);
    OPS_REQUIRE(delivered[0].status == OrderStatus::Delivered);
    require_monotonic_timestamps(delivered[0]);

    const Metrics& m = pipeline.metrics();
    OPS_REQUIRE(m.accepted_count == 1);
    OPS_REQUIRE(m.processed_count == 1);
    OPS_REQUIRE(m.delivered_count == 1);

    OPS_REQUIRE(m.total_processing_time >= duration_t::zero());
}

OPS_TEST("process_all: multiple orders preserve submit order and all become Delivered") {
    Pipeline pipeline;

    pipeline.submit(Order{ 10 });
    pipeline.submit(Order{ 11 });
    pipeline.submit(Order{ 12 });
    pipeline.submit(Order{ 13 });
    pipeline.process_all();

    const auto& delivered = pipeline.delivered_orders();
    OPS_REQUIRE(delivered.size() == 4);

    OPS_REQUIRE(delivered[0].id == 10);
    OPS_REQUIRE(delivered[1].id == 11);
    OPS_REQUIRE(delivered[2].id == 12);
    OPS_REQUIRE(delivered[3].id == 13);

    for (const auto& o : delivered) {
        OPS_REQUIRE(o.status == OrderStatus::Delivered);
        require_monotonic_timestamps(o);
    }
}

OPS_TEST("total_processing_time: equals sum(delivered_time - accepted_time) for delivered orders (single batch)") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });
    pipeline.submit(Order{ 3 });

    pipeline.process_all();

    const auto& delivered = pipeline.delivered_orders();
    OPS_REQUIRE(delivered.size() == 3);

    const duration_t expected = sum_processing_time(delivered);
    const Metrics& m = pipeline.metrics();
    OPS_REQUIRE(m.total_processing_time == expected);
}

OPS_TEST("cumulative: two batches accumulate delivered_orders and metrics; order is stable across batches") {
    Pipeline pipeline;

    pipeline.submit(Order{ 100 });
    pipeline.submit(Order{ 101 });
    pipeline.process_all();

    const Metrics after1 = pipeline.metrics();
    OPS_REQUIRE(after1.accepted_count == 2);
    OPS_REQUIRE(after1.processed_count == 2);
    OPS_REQUIRE(after1.delivered_count == 2);

    pipeline.submit(Order{ 200 });
    pipeline.submit(Order{ 201 });
    pipeline.submit(Order{ 202 });
    pipeline.process_all();

    const auto& delivered = pipeline.delivered_orders();
    OPS_REQUIRE(delivered.size() == 5);

    OPS_REQUIRE(delivered[0].id == 100);
    OPS_REQUIRE(delivered[1].id == 101);
    OPS_REQUIRE(delivered[2].id == 200);
    OPS_REQUIRE(delivered[3].id == 201);
    OPS_REQUIRE(delivered[4].id == 202);

    const Metrics after2 = pipeline.metrics();
    OPS_REQUIRE(after2.accepted_count == 5);
    OPS_REQUIRE(after2.processed_count == 5);
    OPS_REQUIRE(after2.delivered_count == 5);
    OPS_REQUIRE(after2.total_processing_time >= after1.total_processing_time);
}

OPS_TEST("idempotency: calling process_all twice without new orders changes nothing") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });
    pipeline.process_all();

    const Metrics m1 = pipeline.metrics();
    const auto delivered1 = pipeline.delivered_orders().size();

    pipeline.process_all();

    const Metrics m2 = pipeline.metrics();
    const auto delivered2 = pipeline.delivered_orders().size();

    OPS_REQUIRE(m2.accepted_count == m1.accepted_count);
    OPS_REQUIRE(m2.processed_count == m1.processed_count);
    OPS_REQUIRE(m2.delivered_count == m1.delivered_count);
    OPS_REQUIRE(m2.total_processing_time == m1.total_processing_time);
    OPS_REQUIRE(delivered2 == delivered1);
}

#ifdef OPS_TESTING
OPS_TEST("threads: for N >= 2, delivered orders contain at least 2 unique worker thread ids") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });
    pipeline.submit(Order{ 3 });
    pipeline.submit(Order{ 4 });

    pipeline.process_all();

    std::unordered_set<std::thread::id> ids;
    for (const auto& o : pipeline.delivered_orders()) {
        ids.insert(o.last_worker);
    }

    OPS_REQUIRE(ids.size() >= 2);
}
#endif

int main() {
    return ops_test::run_all();
}
