#include <BoundedMemProxy.hpp>
#include <PRQSegment.hpp>
#include <benchmark.hpp>

using item = bench::QueueItem*;

int main(void) {
    return bench::benchmark<
        BoundedMemProxy<item,LinkedPRQ>,
        bench::delay::CONS_DELAY,
        false>
    (4, 4, 1024 * 64,10000000,600,100);
}
