#include <chrono>
#include <iostream>
#include <cmath>        // For std::abs
#include "TicksWait.h"  // Assumed available and functional

#define NSEC_TICKS          static_cast<size_t>(648)    // 100 ns ≈ 648 ticks
#define TOLERANCE_DEFAULT   0.1                         // 10%
#define RUNS_DEFAULT        static_cast<size_t>(100)

int main(int argc, char **argv) {
    using namespace std::chrono;

    // ---- Parse & validate arguments ----
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <desired_nsecs> [tolerance] [runs]\n";
        return 1;
    }

    size_t desired_nsecs;
    try {
        desired_nsecs = std::stoull(argv[1]);
    } catch (...) {
        std::cerr << "Error: Invalid <desired_nsecs>\n";
        return 1;
    }

    if (desired_nsecs < 100) {
        std::cerr << "Error: Desired nanoseconds too low (< 100)\n";
        return 1;
    }

    double tolerance = (argc >= 3) ? std::stod(argv[2]) : TOLERANCE_DEFAULT;
    if (tolerance <= 0.0 || tolerance >= 1.0) {
        std::cerr << "Error: Tolerance must be in (0, 1)\n";
        return 1;
    }

    size_t runs = (argc >= 4) ? std::stoull(argv[3]) : RUNS_DEFAULT;
    if (runs == 0) {
        std::cerr << "Error: Runs must be > 0\n";
        return 1;
    }

    const double absolute_tolerance = desired_nsecs * tolerance;

    // ---- Binary search with tracking best match ----
    size_t lower = 1;
    size_t upper = (desired_nsecs * NSEC_TICKS * 2) / 100;

    size_t best_ticks = 0;
    size_t best_error = std::numeric_limits<size_t>::max();
    size_t best_nsecs = 0;

    while (lower <= upper) {
        size_t mid = (lower + upper) / 2;

        auto start = high_resolution_clock::now();
        ticks_wait(static_cast<ticks>(mid) * runs);
        auto end = high_resolution_clock::now();

        size_t measured_total = duration_cast<nanoseconds>(end - start).count();
        size_t measured_nsecs = measured_total / runs;

        size_t error = std::abs(static_cast<long long>(measured_nsecs) - static_cast<long long>(desired_nsecs));

        // Track the best match so far
        if (error < best_error) {
            best_error = error;
            best_ticks = mid;
            best_nsecs = measured_nsecs;
        }

        if (error <= absolute_tolerance) {
            std::cout << mid << "\n";
            return 0;
        }

        if (measured_nsecs < desired_nsecs) {
            lower = mid + 1;
        } else {
            if (mid == 0) break;
            upper = mid - 1;
        }
    }

    // Convergence failed, return best approximation found
    std::cerr << "Warning: Failed to converge within tolerance.\n";
    std::cerr << "Best match: " << best_ticks << " ticks → ~" << best_nsecs << " ns (target: " << desired_nsecs << " ns)\n";
    std::cout << best_ticks << "\n";
    return 0;
}
