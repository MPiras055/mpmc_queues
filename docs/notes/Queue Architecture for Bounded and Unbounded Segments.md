
## Overview

In order to support the clean integration of new queues that support both bounded and unbounded methods and fields (which will be used in the `UnboundedProxy`), we need to modify the interface architecture.

### Bounded Queue
A **bounded queue** exposes the following methods:

- `bool enqueue(T item)`
- `bool dequeue(T& item)`
- `size_t capacity() const`
- `size_t size()`

### Unbounded Queue
An **unbounded queue** uses bounded queues in a linked list (or other memoization structures) and has to expose additional methods:

- `bool isOpen()`
- `bool isClosed()`
- `bool close()`
- `bool open()`

> The `open/close` mechanism controls whether a segment is closed to further insertions. When a segment is closed, a new one must be allocated and linked to the list.

### Unbounded Methods
All unbounded methods (e.g., `open()`, `close()`, `isOpen()`, `isClosed()`) are only exposed to the specific **`UnboundedProxy`** that adds the logic to maintain the linked list, while internally calling the `enqueue()`/`dequeue()` methods on the bounded segments.

Additionally, **unbounded segments** should support an extra field:

- `next*`: A pointer to the next queue in the logical linked list.

## Interface Architecture

We will achieve this by using **concepts** to define the behavior of both bounded and unbounded segments and use wrapper calls to encapsulate the `next*` field of an unbounded-based segment.

Instead of using traditional interfaces, we can use templates with the following structure:

```cpp
/*
IBoundedQueue<T> is the interface for supporting virtual bounded and unbounded proxy methods.
IUnboundedSegment<T> is the interface for supporting virtual private unbounded segment methods.
*/

template <typename T, bool bounded = true>
class concreteBoundedSegment: public IQueue<T> {
public:
    // Bounded-specific methods
private:
    // Private members for bounded segments
    
    bool enqueue(T item) {
	    if constexpr (!bounded) {
		    //additional logic for unbounded segments
		    //default disable in this concrete class
	    }
	    
	    //rest of bounded method
    
    }
};

template <typename T, typename UProxy> // CRTP pattern
class concreteUnboundedSegment : public IUnboundedSegment<T>, public concreteBoundedSegment<T, false> {
    friend class UProxy;  // UProxy can access unbounded private members
private:
    // Unbounded-specific methods (open, close, isOpen, isClosed, next)
    // next pointer for linked list
    
    // doesn't have to override enqueue since the bounded parameter toggles the behaviour
};
