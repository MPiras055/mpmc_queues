#pragma once
#include <type_traits>

/**
 * Base interface that defines basic contracts for segments that will
 * be used inside linked queues. 
 * 
 * The interface is CRTP to ensure that linked lists can be constructed only
 * amongst segments of the same effective type
 */

template<typename T, typename Derived>
class ILinkedSegment {
    static_assert(std::is_pointer_v<T>, "IQueue<T> requires T to be a pointer type");

public:
    
    virtual ~ILinkedSegment() = default;

    //close contract: closes the segment to further insertions
    virtual void close() = 0;

    //open contract: opens a closed segment
    virtual void open() = 0;

    //closed check contract: checks if a segment is closed
    virtual bool isClosed() const = 0;

    //open check contract
    virtual bool isOpen() const = 0;

    //next contract: returns the next logical linked segment
    virtual Derived* next() const = 0;
    
};