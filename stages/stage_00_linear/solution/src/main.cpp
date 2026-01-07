#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include "order.hpp"
#include "pipeline.hpp"
#include "metrics.hpp"

int main(int argc, char* argv[]) {
    std::size_t orders_count = 500;

    if (argc == 2) {
        try {
            orders_count = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        catch (...) {
            std::cerr << "Usage: ops_app [orders_count]\n";
            return 1;
        }
    }
    else if (argc > 2) {
        std::cerr << "Usage: ops_app [orders_count]\n";
        return 1;
    }

    Pipeline pipeline;

    std::uint64_t next_id = 1;

    for (std::size_t i = 0; i < orders_count; ++i) {
        Order order{ next_id++ };
        pipeline.submit(std::move(order));
    }

    pipeline.process_all();

    const Metrics& m = pipeline.metrics();

    std::cout << "Accepted:  " << m.accepted_count << "\n";
    std::cout << "Processed: " << m.processed_count << "\n";
    std::cout << "Delivered: " << m.delivered_count << "\n";

    using namespace std::chrono;
    std::cout << "Total processing time (ms): "
        << duration_cast<milliseconds>(m.total_processing_time).count()
        << "\n";

    return 0;
}