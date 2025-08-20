#pragma once
#include <type_traits>

/**
 * Base interface for all queues to test
 * 
 * includes basic queues contracts and can be used as base class to keep 
 * tests agnostic and all implementation compliant
 */

template<typename T>
class IQueue {
    static_assert(std::is_pointer_v<T>, "IQueue<T> requires T to be a pointer type");

public:
    
    virtual ~IQueue() = default;

    //Enqueue contract: enqueue an item (copy)
    virtual bool enqueue(const T item) = 0;

    //Dequeue contract: dequeue into a container
    virtual bool dequeue(T& container) = 0;

    //Capacity contract: of the internal queue container
    virtual size_t capacity() const = 0;

    //Size contract: current number of elements (may be approximate)
    virtual size_t size() const = 0;
    
};