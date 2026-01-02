#include "test_framework.hpp"

#include <chrono>
#include <cstdint>

#include "ops/pipeline.hpp"
#include "ops/order.hpp"
#include "ops/metrics.hpp"

OPS_TEST("Empty pipeline: process_all does not change metrics") {
    Pipeline pipeline;

    pipeline.process_all();

    const Metrics& m = pipeline.metrics();
    OPS_REQUIRE(m.accepted_count == 0);
    OPS_REQUIRE(m.processed_count == 0);
    OPS_REQUIRE(m.delivered_count == 0);
    OPS_REQUIRE(std::chrono::steady_clock::duration::zero() == m.total_processing_time);
}

OPS_TEST("Submit increases accepted_count") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });
    pipeline.submit(Order{ 3 });

    const Metrics& m = pipeline.metrics();
    OPS_REQUIRE(m.accepted_count == 3);
    OPS_REQUIRE(m.processed_count == 0);
    OPS_REQUIRE(m.delivered_count == 0);
}

OPS_TEST("Linear processing: all orders are delivered and metrics are correct") {
    Pipeline pipeline;

    for (std::uint64_t id = 1; id <= 5; ++id) {
        pipeline.submit(Order{ id });
    }

    pipeline.process_all();

    const Metrics& m = pipeline.metrics();
    OPS_REQUIRE(m.accepted_count == 5);
    OPS_REQUIRE(m.processed_count == 5);
    OPS_REQUIRE(m.delivered_count == 5);

    OPS_REQUIRE(m.total_processing_time >= std::chrono::steady_clock::duration::zero());
}

OPS_TEST("The order is preserved: delivered_orders go in the same id order") {
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
}

OPS_TEST("Statuses and timestamps: Delivered is set, timestamps are monotonous") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });

    pipeline.process_all();

    const auto& delivered = pipeline.delivered_orders();
    OPS_REQUIRE(delivered.size() == 2);

    for (const auto& o : delivered) {
        OPS_REQUIRE(o.status == OrderStatus::Delivered);

        OPS_REQUIRE(o.accepted_time <= o.prepared_time);
        OPS_REQUIRE(o.prepared_time <= o.packed_time);
        OPS_REQUIRE(o.packed_time <= o.delivered_time);

        OPS_REQUIRE((o.delivered_time - o.accepted_time) >= std::chrono::steady_clock::duration::zero());
    }
}

OPS_TEST("total_processing_time is equal to the sum of orders (delivered_time - accepted_time)") {
    Pipeline pipeline;

    pipeline.submit(Order{ 100 });
    pipeline.submit(Order{ 200 });
    pipeline.submit(Order{ 300 });

    pipeline.process_all();

    const auto& delivered = pipeline.delivered_orders();
    OPS_REQUIRE(delivered.size() == 3);

    std::chrono::steady_clock::duration expected{};
    for (const auto& o : delivered) {
        expected += (o.delivered_time - o.accepted_time);
    }

    const Metrics& m = pipeline.metrics();
    OPS_REQUIRE(m.total_processing_time == expected);
}

OPS_TEST("Calling process_all again without new orders does not change the result.") {
    Pipeline pipeline;

    pipeline.submit(Order{ 1 });
    pipeline.submit(Order{ 2 });

    pipeline.process_all();

    const Metrics first = pipeline.metrics();
    const auto first_delivered_size = pipeline.delivered_orders().size();

    pipeline.process_all();

    const Metrics second = pipeline.metrics();
    const auto second_delivered_size = pipeline.delivered_orders().size();

    OPS_REQUIRE(first.accepted_count == second.accepted_count);
    OPS_REQUIRE(first.processed_count == second.processed_count);
    OPS_REQUIRE(first.delivered_count == second.delivered_count);
    OPS_REQUIRE(first.total_processing_time == second.total_processing_time);

    OPS_REQUIRE(first_delivered_size == second_delivered_size);
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}