In order to support clean integration of new queues that support both bounded and unbounded methods and fields (that will be used in the `UnboundedProxies`) we need to modify the interface architecture.

A bounded queue exposes the following methods:
- `bool enqueue(T item)`
- `bool dequeue(T& item)`
- `size_t capacity() const`
- `size_t size()`

While the same unbounded queue uses bounded queues in a linked list (or other memoization structures) and has to expose (other to the above methods) the following:
- `bool isOpen()`
- `bool isClosed()`
- `bool close()`
- `bool open()`

> The open/close mechanic control whether a segment is closed to further insertions, so a new one has to be allocated and linked to the list

All unbounded methods are only exposed to the specific `UnboundedProxy` that adds the logic to mantain the linked list, while calling internal `enqueue()/dequeue()` on the bounded segments. 

Other than this we also want unbounded segments to support an additional field (`next*`) that is a pointer to the logical next queue in the linked list.

We can achieve all this by using `concepts` to define bounded and unbounded segment behaviour, and use a wrapper calls to to encapsulate the `next*` field of an unbounded based segment

Instead still using interfaces we can do something like this:

```cpp

/*
IBoundedQueue<T> is interface support for virtual bounded and unbounded proxy methods
IUnboundedSegment<T> is interface support for virtual private unbounded segmentmethods
*/

template <typename T>
class concreteBoundedSegment: public IQueue<T> {
public:
    // bounded methods...
private:
    // maybe some private members only for bounded
};


template <typename T, typename UProxy> //CRTP pattern
class concreteUnboundedSegment : public IUnboundedSegment<T> public concreteBoundedSegment<T> {
    friend class UProxy;  // UProxy can access unbounded private members
private:
    // unbounded methods (open, close, isOpen, isClosed, next)
    // next pointer for linked list
};

```

### Unbounded Proxy Example
```cpp

//Unbounded_Proxy.hpp
template <typename T, template<typename, typename> class SegmentTemplate>
class UnboundedProxy {
public:
    using Segment = SegmentTemplate<T, UnboundedProxy<T, SegmentTemplate>>;
};

//main.cpp
int main(void) {
	using MyProxy = UnboundedProxy<int, MySegment>;
	MyProxy proxy;
}

```