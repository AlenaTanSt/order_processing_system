#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include "order.hpp"
#include "queue.hpp"
#include "metrics.hpp"
#include "pipeline.hpp"

namespace {

    template <typename F>
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

    template <typename F>
    bool throws_out_of_range(F&& fn) {
        try {
            fn();
            return false;
        }
        catch (const std::out_of_range&) {
            return true;
        }
        catch (...) {
            return false;
        }
    }

} // namespace

OPS_TEST("Pipeline: process_all on empty pipeline does nothing") {
    Pipeline p;

    const auto m0 = p.metrics();
    OPS_REQUIRE(m0.accepted_count == 0);
    OPS_REQUIRE(m0.processed_count == 0);
    OPS_REQUIRE(m0.delivered_count == 0);
    OPS_REQUIRE(m0.total_processing_time == std::chrono::steady_clock::duration{});
    OPS_REQUIRE(p.delivered_orders().empty());

    p.process_all();

    const auto m1 = p.metrics();
    OPS_REQUIRE(m1.accepted_count == 0);
    OPS_REQUIRE(m1.processed_count == 0);
    OPS_REQUIRE(m1.delivered_count == 0);
    OPS_REQUIRE(m1.total_processing_time == std::chrono::steady_clock::duration{});
    OPS_REQUIRE(p.delivered_orders().empty());
}

OPS_TEST("Order: initial state is Accepted and accepted_time set") {
    Order o(123);

    OPS_REQUIRE(o.id == 123);
    OPS_REQUIRE(o.status == OrderStatus::Accepted);

    OPS_REQUIRE(o.accepted_time.time_since_epoch() != std::chrono::steady_clock::duration{});
}

OPS_TEST("Order: valid transitions set timestamps and keep previous ones") {
    Order o(1);

    const auto t_acc = o.accepted_time;

    o.advance_to(OrderStatus::Prepared);
    OPS_REQUIRE(o.status == OrderStatus::Prepared);
    OPS_REQUIRE(o.accepted_time == t_acc);
    OPS_REQUIRE(o.prepared_time >= t_acc);

    const auto t_prep = o.prepared_time;

    o.advance_to(OrderStatus::Packed);
    OPS_REQUIRE(o.status == OrderStatus::Packed);
    OPS_REQUIRE(o.accepted_time == t_acc);
    OPS_REQUIRE(o.prepared_time == t_prep);
    OPS_REQUIRE(o.packed_time >= t_prep);

    const auto t_pack = o.packed_time;

    o.advance_to(OrderStatus::Delivered);
    OPS_REQUIRE(o.status == OrderStatus::Delivered);
    OPS_REQUIRE(o.accepted_time == t_acc);
    OPS_REQUIRE(o.prepared_time == t_prep);
    OPS_REQUIRE(o.packed_time == t_pack);
    OPS_REQUIRE(o.delivered_time >= t_pack);
}

OPS_TEST("Order: invalid transition throws logic_error and does not mutate state") {
    Order o(7);

    const auto id0 = o.id;
    const auto st0 = o.status;
    const auto t_acc0 = o.accepted_time;
    const auto t_prep0 = o.prepared_time;
    const auto t_pack0 = o.packed_time;
    const auto t_del0 = o.delivered_time;

    OPS_REQUIRE(throws_logic_error([&] { o.advance_to(OrderStatus::Packed); }));

    OPS_REQUIRE(o.id == id0);
    OPS_REQUIRE(o.status == st0);
    OPS_REQUIRE(o.accepted_time == t_acc0);
    OPS_REQUIRE(o.prepared_time == t_prep0);
    OPS_REQUIRE(o.packed_time == t_pack0);
    OPS_REQUIRE(o.delivered_time == t_del0);

    o.advance_to(OrderStatus::Prepared);
    OPS_REQUIRE(o.status == OrderStatus::Prepared);

    const auto st1 = o.status;
    const auto t_acc1 = o.accepted_time;
    const auto t_prep1 = o.prepared_time;
    const auto t_pack1 = o.packed_time;
    const auto t_del1 = o.delivered_time;

    OPS_REQUIRE(throws_logic_error([&] { o.advance_to(OrderStatus::Accepted); }));

    OPS_REQUIRE(o.status == st1);
    OPS_REQUIRE(o.accepted_time == t_acc1);
    OPS_REQUIRE(o.prepared_time == t_prep1);
    OPS_REQUIRE(o.packed_time == t_pack1);
    OPS_REQUIRE(o.delivered_time == t_del1);
}

OPS_TEST("OrderQueue: FIFO and pop empty throws out_of_range") {
    OrderQueue q;

    OPS_REQUIRE(q.empty());
    OPS_REQUIRE(throws_out_of_range([&] { (void)q.pop(); }));

    q.push(Order(1));
    q.push(Order(2));
    q.push(Order(3));

    OPS_REQUIRE(!q.empty());

    auto a = q.pop();
    auto b = q.pop();
    auto c = q.pop();

    OPS_REQUIRE(a.id == 1);
    OPS_REQUIRE(b.id == 2);
    OPS_REQUIRE(c.id == 3);

    OPS_REQUIRE(q.empty());
    OPS_REQUIRE(throws_out_of_range([&] { (void)q.pop(); }));
}

OPS_TEST("Pipeline: processes all orders sequentially, preserves order, updates metrics") {
    Pipeline p;

    p.submit(Order(10));
    p.submit(Order(11));
    p.submit(Order(12));

    {
        const auto& m = p.metrics();
        OPS_REQUIRE(m.accepted_count == 3);
        OPS_REQUIRE(m.processed_count == 0);
        OPS_REQUIRE(m.delivered_count == 0);
        OPS_REQUIRE(m.total_processing_time == std::chrono::steady_clock::duration{});
        OPS_REQUIRE(p.delivered_orders().empty());
    }

    p.process_all();

    const auto& delivered = p.delivered_orders();
    OPS_REQUIRE(delivered.size() == 3);

    OPS_REQUIRE(delivered[0].id == 10);
    OPS_REQUIRE(delivered[1].id == 11);
    OPS_REQUIRE(delivered[2].id == 12);

    for (const auto& o : delivered) {
        OPS_REQUIRE(o.status == OrderStatus::Delivered);

        OPS_REQUIRE(o.accepted_time.time_since_epoch() != std::chrono::steady_clock::duration{});
        OPS_REQUIRE(o.prepared_time >= o.accepted_time);
        OPS_REQUIRE(o.packed_time >= o.prepared_time);
        OPS_REQUIRE(o.delivered_time >= o.packed_time);
    }

    const auto& m = p.metrics();
    OPS_REQUIRE(m.accepted_count == 3);
    OPS_REQUIRE(m.processed_count == 3);
    OPS_REQUIRE(m.delivered_count == 3);

    std::chrono::steady_clock::duration expected{};
    for (const auto& o : delivered) {
        expected += (o.delivered_time - o.accepted_time);
    }
    OPS_REQUIRE(m.total_processing_time == expected);

    const auto before = p.metrics();
    const auto delivered_before = delivered.size();

    p.process_all();

    const auto after = p.metrics();
    OPS_REQUIRE(after.accepted_count == before.accepted_count);
    OPS_REQUIRE(after.processed_count == before.processed_count);
    OPS_REQUIRE(after.delivered_count == before.delivered_count);
    OPS_REQUIRE(after.total_processing_time == before.total_processing_time);
    OPS_REQUIRE(p.delivered_orders().size() == delivered_before);
}

int main(int argc, char** argv) {
    return ops_test::run(argc, argv);
}