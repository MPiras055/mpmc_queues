# Segment Interfaces

This design provides a **base queue interface** (`IQueue<T>`) that is type-agnostic, with an optional static check for pointer-based types (common in lock-free / intrusive queues).  

We then extend it into two flavors:

- **Bounded segments** (`NotLinkedSegment<T>`) → stand-alone queues with a fixed capacity.  
- **Linked segments** (`LinkedSegment<T, Proxy>`) → bounded segments that can be chained together for unbounded growth, with additional contracts for `open/close` and segment linking.
- **Proxy** -> a proxy manages linked segments and implements additional logic to implement a queue from a linked list of `linked segments`

Finally, we support **proxy classes** (e.g. `UnboundedProxy`) that can only operate on linked segments.

## Base Queue Contract

The queue contract defines the **minimal operations**:

- `enqueue` – inserts an element.
- `dequeue` – removes into an output reference.
- `capacity` – fixed maximum number of elements (for bounded segments).
- `size` – current number of elements (may be approximate in concurrent queues).
- `open/close` and `isOpened/isClosed` – internal lifecycle methods (used by linked segments).

```cpp
#pragma once
#include <cstddef>
#include <type_traits>

// ==========================
// Tag for detection
// ==========================
template <typename T>
struct LinkedTag {};

// Helper trait for readability
template <typename T>
constexpr bool is_linked_segment_v =
    std::is_base_of_v<LinkedTag<typename T::value_type>, T>;

// ==========================
// Base Queue Interface
// ==========================
template <typename T>
class IQueue {
    // By default, restrict to pointer types (can be relaxed if needed)
    static_assert(std::is_pointer_v<T>, "IQueue requires T to be a pointer type");

public:
    using value_type = T;
    virtual ~IQueue() = default;

    // Core queue operations
    virtual bool enqueue(const T item) = 0;
    virtual bool dequeue(T& container) = 0;
    virtual size_t capacity() const = 0;
    virtual size_t size() const = 0;

protected: // only accessible to friends / derived classes
    // Lifecycle control (for linked segments)
    
    virtual bool close() = 0;
    virtual bool open()  = 0;
    virtual bool isClosed() const = 0;
    virtual bool isOpened() const = 0;
};
```

## Linked Segment Contract

Linked segments extend the base queue by supporting a **linked-list structure** (`getNext`/`setNext`).

This is abstracted into `ILinked`, which adds the `LinkedTag` so we can detect linked segments at compile time.

```cpp
// ==========================
// Linked Segment Interface
// ==========================
template<typename T, typename Derived>
class ILinked : public IQueue<T>, public LinkedTag<T> {
public:
    virtual Derived* getNext() const = 0;
    virtual void setNext(Derived* next) = 0;
};

```

## Concrete Bounded Segment

A bounded segment is a simple queue with fixed capacity.  
It **can detect at compile time** whether it’s being used as a linked segment, using `is_linked_segment_v`.

```cpp
// ==========================
// Example: Bounded Segment
// ==========================
template<typename T>
class NotLinkedSegment : public IQueue<T> {
public:
	using is_linked = is_linked_segment_v<std::decay_t<decltype(*this)>>;
    using value_type = T;

    bool enqueue(const T item) override {
        if constexpr (is_linked) {
            // Extra logic if this is part of a linked segment
        }
        // Normal bounded enqueue logic
        return true;
    }

    bool dequeue(T& item) override {
        if constexpr (is_linked) {
            // Extra logic if this is part of a linked segment
        }
        // Normal bounded dequeue logic
        return true;
    }

    size_t capacity() const override { return 128; }
    size_t size() const override { return 0; }

private:
	//methods that will be only used if the segment is linked inherited
    bool close() final override { return true; }
    bool open() final override { return true; }
    bool isClosed() final override const { return false; }
    bool isOpened() final override const { return true; }
};

```

## Concrete Linked Segment

A linked segment extends the bounded segment and implements the `ILinked` contract.  
It stores a `next` pointer to the following segment.  
The `Proxy` class is declared as a friend, so only the proxy can manage links and lifecycle.

```cpp
// ==========================
// Example: Linked Segment
// ==========================
template <typename T, typename Proxy>
class LinkedSegment
    : public NotLinkedSegment<T>,
      public ILinked<T, LinkedSegment<T, Proxy>> {
    friend Proxy; // Proxy can access private/protected methods

public:
    using value_type = T;

    LinkedSegment* getNext() const override { return next; }
    void setNext(LinkedSegment* n) override { next = n; }

private:
    LinkedSegment* next = nullptr;
};

```

# Proxy Interface

From the user’s perspective, a **proxy is the queue**.  
It implements `IQueue<T>`, but internally manages one or more **segments**:

- **BoundedProxy** → wraps a limited chain of `NotLinkedSegment<T>` (fixed capacity).
- **UnboundedProxy** → manages an unbounded chain of `LinkedSegment<T, Proxy>`.

This way, the end-user **never touches segments directly**.  
The proxy provides the *safe facade*, while segments provide the *raw storage*.

```cpp
// ==========================
// Proxy Base
// ==========================
template<typename T, typename SegmentType>
class IProxy : public IQueue<T> {

    static_assert(is_linked_segment_v<SegmentType>,
        "Proxy interfaces only allow Linked Segments");

public:
    using value_type = T;
    virtual ~IProxy() = default;

protected:
    // Disable lifecycle methods for proxies (they belong to segments)
    bool open() final override { return false; }
    bool close() final override { return false; }
    bool isOpened() final override const { return true; }
    bool isClosed() final override const { return false; }
};
```




