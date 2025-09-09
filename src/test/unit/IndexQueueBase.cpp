#include <IndexQueue.hpp>


int main() {
    const size_t queue_size = 1024;  // Size of the queue to test
    util::hazard::IndexQueueTest test(queue_size);
    
    test.testConcurrency();  // Run the concurrent test
    return 0;
}