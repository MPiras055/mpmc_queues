Make a basic queue interface type-agnostic. Even though most queues are pointer based we can check that using static assertion over the type `T`.

## Queue Contracts
We make the virtual implementation as simple as possible only basic methods:
- `bool enqueue(const T item)` 
- `bool dequeue(&T container)`
- `size_t capacity()` capacity of the internal container
- `size_t size()` number of elements in the underlying container (may be an approximation)

```cpp
#pragma once
#include <cstddef>
#include <type_traits>

template <typename T>
class IMPMCQueue {
	//this can be changed or removed to relax the the restriction
    static_assert(std::is_pointer_v<T>, "IMPMCQueue requires T to be a pointer type");

public:
    virtual ~IQueue() = default;
    
    // Enqueue an item (copy)
    virtual bool enqueue(const T item) = 0;
    
    // Dequeue into container (output parameter)
    virtual bool dequeue(T& container) = 0;
    
    // Capacity of the internal container
    virtual size_t capacity() const = 0;
    
    // Current number of elements (may be approximate)
    virtual size_t size() const = 0;
};
```

## Follow-Up: Concepts
In order to support clean integration of an implementation both as a bounded segment and as an [[Unbounded Interface|unbounded]] we can also use C++20 `concepts`.

With this we don't need to define a base interface but we can define 2 basic concepts that encapsulate the logic of `bounded` and `unbounded` segments.

```cpp
//MPMC_Concepts.hpp
#pragma once
#include <concepts>
#include <cstddef>
template <typename T, typename Q>
concept BoundedQueue = requires (Q q, T item, T& out) {
	{q.enqueue(item)} -> std::same_as<bool>;
	{q.dequeue(out)} -> std::same_as<bool>;
	{q.capacity()} -> std::convertible_to<size_t>;
	{q.size()} -> std::convertible_to<size_t>;
}

template <typename T, typename S>
concept UnboundedSegment = BoundedQueue<T, S> &&
requires(S seg) {
    { seg.open() } -> std::same_as<void>;
    { seg.close() } -> std::same_as<void>;
    { seg.isOpen() } -> std::same_as<bool>;
    { seg.isClosed() } -> std::same_as<bool>;
    { seg.next } -> std::same_as<S*>;
};
```

With this, we don't have to explicitly handle polymorphism and expect everything to work without inheritance. Since concepts are lazy evaluated we can make a specific implementation having all methods
