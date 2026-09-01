/**
 * @file main.cpp
 * @brief Throughput benchmark, selected by name at startup.
 *
 * Replaces the previous `switch (queue_type)` over the integers 0-4, which could only
 * reach five standalone buffers. Selection is a compile-time fold over the registry, so
 * the measured loop runs on a statically known type -- no vtable, no std::function, no
 * variant -- and every registered implementation, proxies included, is reachable.
 */
#include <registry/Registry.hpp>
#include <util/threading/ThreadPinner.hpp>
#include <util/timing/AdditionalWork.hpp>

#include <atomic>
#include <barrier>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using TestItem = uint64_t*;

/// Optional simulated per-operation work, so contention can be varied.
struct Delay {
    size_t ticks = 0;
    float amplitude = 0.5f;
    bool on = false;

    void operator()() const {
        if (on) random_work(ticks, amplitude);
    }
};

template <typename Queue>
class Benchmark {
public:
    Benchmark(size_t producers, size_t consumers, uint64_t items, size_t capacity)
        : producers_{producers}, consumers_{consumers}, items_{items},
          instance_{capacity} {
        assert(producers_ != 0 && consumers_ != 0 && items_ != 0);
    }

    void set_pinning() { pinning_ = true; }
    void set_producer_delay(Delay d) { prod_delay_ = d; }
    void set_consumer_delay(Delay d) { cons_delay_ = d; }

    /// @return items per second.
    long double execute() {
        Queue& q = instance_.get();
        const uint64_t batch = items_ / producers_;
        const uint64_t remainder = items_ % producers_;

        std::barrier producers_done_barrier(producers_ + 1);
        std::barrier all_barrier(producers_ + consumers_ + 1);
        std::atomic<bool> producers_done{false};

        const auto producer = [&](uint64_t id) {
            // Start at 1 so no item is ever a null pointer.
            const uint64_t first = id * batch + (id < remainder ? id : remainder) + 1;
            const uint64_t count = batch + (id < remainder ? 1 : 0);
            [[maybe_unused]] auto joined = registry::Instance<Queue>::session(q);
            all_barrier.arrive_and_wait();
            for (uint64_t i = first; i < first + count; ++i) { // NB: first + count
                prod_delay_();
                while (!q.try_enqueue(std::bit_cast<TestItem>(i))) {}
            }
            producers_done_barrier.arrive_and_wait();
            all_barrier.arrive_and_wait();
        }; // `joined` releases the thread here, on every exit path

        const auto consumer = [&] {
            TestItem out = nullptr;
            [[maybe_unused]] auto joined = registry::Instance<Queue>::session(q);
            all_barrier.arrive_and_wait();
            // try_dequeue, not dequeue: the blocking form only returns once somebody closes
            // the queue, and this loop has to keep re-checking `producers_done` instead. The
            // registry surface has no generic close(), so the condition-variable path in
            // algo::Mutex is not what this harness measures -- see the note in the docs.
            while (!producers_done.load(std::memory_order_relaxed))
                if (q.try_dequeue(out)) cons_delay_();
            while (q.try_dequeue(out)) cons_delay_();
            all_barrier.arrive_and_wait();
        };

        std::vector<std::thread> prod, cons;
        prod.reserve(producers_);
        cons.reserve(consumers_);
        for (size_t i = 0; i < producers_; ++i) prod.emplace_back(producer, i);
        for (size_t i = 0; i < consumers_; ++i) cons.emplace_back(consumer);
        if (pinning_) {
            // Both of these matter to the result and neither is visible in the number
            // printed at the end, so they go to stderr: the throughput on stdout is what
            // the Python runner parses.
            if (pinner_.origin() != util::threading::ThreadPinner::Origin::file) {
                std::cerr << "note: pinning by " << pinner_.origin_name()
                          << ", not the generated topology -- the core order is ascending, "
                             "not the requested layout\n";
            }
            if (!pinner_.pin(prod, cons))
                std::cerr << "warning: could not pin threads; results are unpinned\n";
        }

        const auto start = std::chrono::high_resolution_clock::now();
        all_barrier.arrive_and_wait();
        producers_done_barrier.arrive_and_wait();
        producers_done.store(true, std::memory_order_release);
        all_barrier.arrive_and_wait();
        const auto end = std::chrono::high_resolution_clock::now();

        for (auto& t : prod) t.join();
        for (auto& t : cons) t.join();

        const std::chrono::nanoseconds elapsed = end - start;
        constexpr uint64_t kNsPerSec = 1'000'000'000;
        return static_cast<long double>(items_) * kNsPerSec /
               static_cast<long double>(elapsed.count());
    }

private:
    size_t producers_, consumers_;
    uint64_t items_;
    registry::Instance<Queue> instance_;
    util::threading::ThreadPinner pinner_;
    bool pinning_ = false;
    Delay prod_delay_{}, cons_delay_{};
};

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " <name> <producers> <consumers> <items> <capacity>"
                 " [pin] [prod_ticks prod_amp] [cons_ticks cons_amp]\n\n"
                 "  <capacity>  total items the queue holds. A linked queue splits it across\n"
                 "              the segments that will exist -- a count fixed by the type, and\n"
                 "              the same one for every bounded family -- so the per-segment\n"
                 "              size is capacity/segments, floored at 2 and rounded up by the\n"
                 "              algorithm. The queue may therefore hold more than asked.\n"
                 "\nregistered:\n";
    registry::for_each_name(registry::All<TestItem>{},
                            [](std::string_view n) { std::cerr << "  " << n << '\n'; });
}

} // namespace

int main(int argc, char** argv) {
    // Machine-readable name list: one per line, nothing else on stdout. The Python
    // tooling calls this so the C++ registry stays the single source of truth for what
    // exists -- previously the same list was duplicated into a QUEUE_MAP and a
    // STYLE_MAP, and both drifted.
    if (argc == 2 && std::string(argv[1]) == "--list") {
        registry::for_each_name(registry::All<TestItem>{},
                                [](std::string_view n) { std::cout << n << '\n'; });
        return 0;
    }

    if (argc < 6) {
        usage(argv[0]);
        return 1;
    }

    const std::string name = argv[1];
    const size_t producers = std::stoul(argv[2]);
    const size_t consumers = std::stoul(argv[3]);
    const uint64_t items = std::stoull(argv[4]);
    const size_t capacity = std::stoul(argv[5]);

    bool pin = false;
    Delay prod{}, cons{};
    int i = 6;
    if (i < argc && std::string(argv[i]) == "pin") {
        pin = true;
        ++i;
    }
    if (i + 1 < argc) {
        prod = {std::stoul(argv[i]), std::stof(argv[i + 1]), true};
        prod.on = prod.ticks != 0;
        i += 2;
    }
    if (i + 1 < argc) {
        cons = {std::stoul(argv[i]), std::stof(argv[i + 1]), true};
        cons.on = cons.ticks != 0;
    }

    // One branch per registered entry; the matching one is instantiated with the
    // concrete type and everything below runs monomorphically.
    const bool matched = registry::dispatch_impl(
        registry::All<TestItem>{}, name, [&]<typename Q>() {
            Benchmark<Q> bench(producers, consumers, items, capacity);
            if (pin) bench.set_pinning();
            bench.set_producer_delay(prod);
            bench.set_consumer_delay(cons);
            std::cout << bench.execute() << "\n";
        });

    if (!matched) {
        std::cerr << "unknown queue: " << name << "\n\n";
        usage(argv[0]);
        return 1;
    }
    return 0;
}
