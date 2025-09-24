#include <chrono>
#include <barrier>
#include <thread>
#include <vector>
#include <iostream>
#include <unistd.h>
#include <cassert>
#include <IProxy.hpp>
#include <ThreadPinner.hpp>
#include <AdditionalWork.hpp>

//Assumes that CORE_TOPOLOGY is defined if pinning is active

namespace bench {

static constexpr size_t NSEC_IN_SEC = 1'000'000'000ull;

enum class delay {
    NO_DELAY,
    PROD_DELAY,
    CONS_DELAY,
    BOTH_DELAY
};

struct QueueItem {  //dummy pointer
    int value;
};

using item = QueueItem*;

template<typename Q>
Q create_queue(size_t size_queue, size_t threads) {
    if constexpr (base::is_proxy_v<Q>) {
        return Q(size_queue,threads);
    } else {
        return Q(size_queue);
    }
}

/**
 * @brief runs a performance benchmark sending
 */
template<typename Q, delay do_delay, bool pin_threads>
bool benchmark(
    size_t prod,
    size_t cons,
    size_t size_queue,
    size_t iterations,
    size_t delay_center,
    size_t delay_amplitude) {

    using namespace std::chrono;

    if(prod == 0 || cons == 0 || iterations == 0 || size_queue <= 1)
        return false;

    Q queue = create_queue<Q>(size_queue,prod + cons);

    //balance work for producers
    size_t iter_per_prod = iterations / prod;
    size_t remaining_per_prod = iterations % prod;

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    std::barrier<> threadBarrier(prod + cons + 1);
    std::barrier<> producerBarrier(prod + 1);
    std::atomic_bool consumerStop{false};
    std::atomic_bool producerStop{false};

    for(size_t i = 0; i < prod; i++) {
        producers.emplace_back([&,i]{
            QueueItem dummyItem;
            QueueItem* dummy = &dummyItem;
            size_t iterations = iter_per_prod + (i < remaining_per_prod? 1 : 0);
            //acquire a ticket for the queue if proxy
            if constexpr (base::is_proxy_v<Q>) {
                bool ok = queue.acquire();
                assert(ok && "[Producers] Ticket for proxy coudn't be acquired");
            }
            threadBarrier.arrive_and_wait();    //threads wait for main thread to signal

            for(size_t j = 0; j < iterations && (!producerStop.load(std::memory_order_relaxed)); j++) {
                (void) j;
                //perform random work only before each enqueue
                if constexpr ((do_delay == delay::PROD_DELAY) || (do_delay == delay::BOTH_DELAY)) {
                    random_work(delay_center,delay_amplitude);
                }
                while(!queue.enqueue(dummy));
            }

            //release the queue ticket if not proxy
            if constexpr (base::is_proxy_v<Q>) {
                queue.release();
            }

            producerBarrier.arrive_and_wait();  //producers are done
            threadBarrier.arrive_and_wait();

        });
    }

    for(size_t i = 0; i < cons; i++) {
        consumers.emplace_back([&]{
            QueueItem* dummy;

            //acquire a ticket for the queue if proxy
            if constexpr (base::is_proxy_v<Q>) {
                bool ok = queue.acquire();
                assert(ok && "[Consumers] Ticket for proxy coudn't be acquired");
            }
            threadBarrier.arrive_and_wait(); //waits for pinning setting

            while(!consumerStop.load(std::memory_order_relaxed)) {
                while(!queue.dequeue(dummy) && !consumerStop.load(std::memory_order_relaxed));
                //perform random work only after successful dequeue
                if constexpr ((do_delay == delay::CONS_DELAY) || (do_delay == delay::BOTH_DELAY)) {
                    random_work(delay_center,delay_amplitude);
                }
            }
            //right now drain the queue
            while(queue.dequeue(dummy)) {
                if constexpr ((do_delay == delay::CONS_DELAY) || (do_delay == delay::BOTH_DELAY)) {
                    random_work(delay_center,delay_amplitude);
                }
            }

            if constexpr (base::is_proxy_v<Q>) {
               queue.release();
            }

            threadBarrier.arrive_and_wait();
        });
    }

    if constexpr(pin_threads) {
        ThreadPinner T;
        bool ok = T.pin_threads(producers,consumers);
        if(!ok) {
            //threads coudn't be pinned
            producerStop.store(true,std::memory_order_release);
            consumerStop.store(true,std::memory_order_release);
            threadBarrier.arrive_and_wait();
            producerBarrier.arrive_and_wait();
            threadBarrier.arrive_and_wait();
            for(auto& p : producers) p.join();
            for(auto& c : consumers) c.join();
            return false;
        }

    }

    threadBarrier.arrive_and_wait();    //starts thread iteration

    auto start  = high_resolution_clock::now(); //starts clock

    producerBarrier.arrive_and_wait();  //main waits for producers
    consumerStop.store(true,std::memory_order_release); //after producers are done set the consumer flag

    threadBarrier.arrive_and_wait();    // wait for all threads to be done
    auto end    = high_resolution_clock::now();

    //return the ops per sec
    std::chrono::nanoseconds deltaTime = end - start;

    for(auto& p : producers) {
        p.join();
    }
    for(auto& c : consumers ) {
        c.join();
    }


    std::cout << static_cast<long double>(iterations * NSEC_IN_SEC) / deltaTime.count() << "\n";
    return 0;
}

}   //bench namespace
