#pragma once
#include <cstdlib>
#include <random>
#include "TicksWait.h"
#ifndef NDEBUG
#include <cassert>
#endif

static std::random_device random_device;
static thread_local std::minstd_rand random_engine{random_device()};
static thread_local std::uniform_real_distribution<double> random_01_distribution{};


/**
 * Random number between 0 and 1
 */
static inline double next_double() {
    return random_01_distribution(random_engine);
}

/**
 * @brief loop function that should not be optimized out
 */
inline void loop(size_t stop) {
    for(size_t i = 0; i < stop; i++) {
        asm volatile ("nop");
    }
}

static inline void random_work(const double mean) {
    if (mean >= 1.0){
        const double ref = 1. / mean;
        while (next_double() >= ref);
    }
    return;
}

inline size_t randint(size_t center, size_t amplitude) {
    assert(amplitude <= center); // avoid underflow

    // Generate random double in [0.0, 1.0)
    double rand01 = random_01_distribution(random_engine);

    // Calculate result in [center - amplitude, center + amplitude)
    return static_cast<size_t>(static_cast<double>(center - amplitude) + rand01 * (2 * amplitude));

}


/**
 * @brief Random work function
 *
 * @param center (size_t) center of the distribution
 * @param amplitude (size_t) amplitude of the distribution
 *
 * loops for a random number between center - amplitude and center + amplitude
 */
inline void random_work(size_t center,size_t amplitude){
    ticks_wait((ticks) randint(center,amplitude));
}

/**
 * @brief Random number between 0 and max
 */
inline size_t randint(size_t max){
    return randint(0,max);
}
