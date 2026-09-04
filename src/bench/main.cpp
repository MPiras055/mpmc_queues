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
#include <type_traits>
#include <thread>
#include <vector>

namespace {

using TestItem = uint64_t*;

/**
 * @brief Which registry list this binary sweeps.
 *
 * `mpmc_tune` is the same harness over `registry::Tuning` -- the instrumented and backoff
 * variants — so the two binaries cannot drift in how they measure. See Registry.hpp for why the
 * tuning entries are kept out of `All`.
 */
#ifdef MPMC_BENCH_TUNING
template <typename T> using BenchSet = registry::Tuning<T>;
#else
template <typename T> using BenchSet = registry::All<T>;
#endif

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
    /**
     * @brief Turn a sequence number into an item every tag scheme can carry.
     *
     * Even, at least 2, and far below the top bit. All three matter, and the benchmark used to
     * get the first two wrong by sending `1`:
     *
     *  - `cell::LowTag` (FAAArray, HQ) reserves **0 as empty and 1 as consumed**, so item `1`
     *    read back as an already-consumed cell. Measured directly: 8 enqueued, 7 drained.
     *  - `cell::MsbTag` (PRQ, PSCQ) reserves the top bit, so items must stay below it.
     *  - `algo::VyukovNoABA` encodes an empty cell as a word with the top *and* bottom bits set,
     *    which an even word can never collide with.
     */
    static TestItem encode(uint64_t seq) noexcept {
        return std::bit_cast<TestItem>((seq + 1) * 2);
    }

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
            const uint64_t first = id * batch + (id < remainder ? id : remainder) + 1;
            const uint64_t count = batch + (id < remainder ? 1 : 0);
            [[maybe_unused]] auto joined = registry::Instance<Queue>::session(q);
            all_barrier.arrive_and_wait();
            for (uint64_t i = first; i < first + count; ++i) { // NB: first + count
                prod_delay_();
                while (!q.try_enqueue(encode(i))) {}
                produced_.fetch_add(1, std::memory_order_relaxed);
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
                if (q.try_dequeue(out)) { consumed_.fetch_add(1, std::memory_order_relaxed);
                                          cons_delay_(); }
            while (q.try_dequeue(out)) { consumed_.fetch_add(1, std::memory_order_relaxed);
                                         cons_delay_(); }
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
    /// Counted rather than assumed: a run that loses items must not report a throughput.
    std::atomic<uint64_t> produced_{0}, consumed_{0};

public:
    uint64_t produced() const noexcept { return produced_.load(std::memory_order_relaxed); }
    uint64_t consumed() const noexcept { return consumed_.load(std::memory_order_relaxed); }
    /// Non-const: registry::Instance::get() is not const-qualified.
    Queue& queue() noexcept { return instance_.get(); }
};

/**
 * @brief `key=value` lines, one per metric.
 *
 * Deliberately not the default: everything downstream reads a bare number today. The slot
 * efficiency the notes define -- W = S*n - i, eta = i/(S*n) -- is derived in Python from
 * `segments_linked` (S), `segment_capacity` (n) and `produced` (i), so nothing here needs a
 * per-cell counter.
 */
template <typename B>
void emit_metrics(B& bench, long double rate, uint64_t items) {
    std::cout << "throughput=" << rate << "\n"
              << "produced=" << bench.produced() << "\n"
              << "consumed=" << bench.consumed() << "\n"
              << "items=" << items << "\n"
              << "capacity=" << bench.queue().capacity() << "\n";
    using Q = std::remove_cvref_t<decltype(bench.queue())>;
    if constexpr (requires { bench.queue().segment_capacity(); })
        std::cout << "segment_capacity=" << bench.queue().segment_capacity() << "\n";
    if constexpr (requires { bench.queue().segments_linked(); }) {
        std::cout << "segments_linked=" << bench.queue().segments_linked() << "\n"
                  << "segments_retired=" << bench.queue().segments_retired() << "\n"
                  << "segments_discarded=" << bench.queue().segments_discarded() << "\n";
    }
    (void)sizeof(Q);
}

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
    registry::for_each_name(BenchSet<TestItem>{},
                            [](std::string_view n) { std::cerr << "  " << n << '\n'; });
}

} // namespace

int main(int argc, char** argv) {
    // Machine-readable name list: one per line, nothing else on stdout. The Python
    // tooling calls this so the C++ registry stays the single source of truth for what
    // exists -- previously the same list was duplicated into a QUEUE_MAP and a
    // STYLE_MAP, and both drifted.
    if (argc == 2 && std::string(argv[1]) == "--list") {
        registry::for_each_name(BenchSet<TestItem>{},
                                [](std::string_view n) { std::cout << n << '\n'; });
        return 0;
    }

    // Opt-in structured output. The default stays a single bare number, because runner.py
    // parses it with float(stdout) and every CSV already on disk depends on that.
    bool metrics = false;
    for (int a = 1; a < argc; ++a)
        if (std::string(argv[a]) == "--metrics") metrics = true;

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
    bool lost = false;
    const bool matched = registry::dispatch_impl(
        BenchSet<TestItem>{}, name, [&]<typename Q>() {
            Benchmark<Q> bench(producers, consumers, items, capacity);
            if (pin) bench.set_pinning();
            bench.set_producer_delay(prod);
            bench.set_consumer_delay(cons);
            const long double rate = bench.execute();

            // The gate. A run that loses or duplicates items must not report a throughput --
            // this tree has had three segments do exactly that while looking fast.
            if (bench.produced() != bench.consumed()) {
                std::cerr << "LOST ITEMS: produced=" << bench.produced()
                          << " consumed=" << bench.consumed()
                          << " delta=" << static_cast<int64_t>(bench.produced()) -
                                             static_cast<int64_t>(bench.consumed())
                          << "\n";
                lost = true;
                return;
            }
            if (metrics) emit_metrics(bench, rate, items);
            else std::cout << rate << "\n";
        });
    if (lost) return 2;

    if (!matched) {
        std::cerr << "unknown queue: " << name << "\n\n";
        usage(argv[0]);
        return 1;
    }
    return 0;
}
