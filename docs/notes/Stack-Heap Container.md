We want to both support stack allocated storage and heap allocated storage for all queues interfaces. 

Since all queues need an internal buffer (can be of plain cells or tagged with sequence numbers) we use an abstract class as storage policy that supports stack and heap allocated storages.

All queues constructors will receive a pointer to the specified storage policy that is initialized elsewhere.

## Storage Policy
```cpp
//IStoragePolicy.hpp
#pragma once
#include <cstddef>
template <typename T>
class IStoragePolicy {
public:
	virtual T* data() = 0;
	virtual size_t capacity() const = 0;
	virtual ~IStoragePolicy() = default;
}

//HeapStorage.hpp
#include "IStoragePolicy.hpp"
#include <cstddef>
template<typename T>
class HeapStorage: public IStoragePolicy<T> {
public:
	explicit HeapStorage(size_t capacity)
		: capacity_(capacity), buffer_(new T[capacity]){}
	
	~HeapStorage() override {
		delete[] buffer_;
	}
	
	T* data() override {
		return buffer_;
	}
	
	size_t capacity() const override {
		return capacity_;
	}
private:
	const size_t capacity_;
	T* buffer_;
}

//StackStorage.hpp
#pragma once
#include "IStoragePolicy.hpp"
#include <array>
#include <cstddef>

template <typename T, size_t N>
class StackStorage: public IStoragePolicy<T> {
public:
	StackStorage() = default;
	
	T* data() override {
		return buffer_.data();
	}
	
	size_t capacity() const override {
		return N;
	}
private:
	std::array<T, N> buffer_;
}
```

## Bounded Queue Constructor Example
```cpp
//BoundedQueue.hpp
#pragma once
#include "IStoragePolicy.hpp"
#include <memory>

template <typename T>
class BoundedQueue {
public:
	explicit BoundedQueue(std::unique_ptr<IStoragePolicy<T>> storage)
		:storage_(std::move(storage)){}
}

//main.cpp
#include "BoundedQueue.hpp"
#include "HeapStorage.hpp"
#include "StackStorage.hpp"

int main() {
	auto heapStorage = std::make_unique<HeapStorage<int>>(1024);
	BoundedQueue<int> queue(std::move(heapStorage));
	
	auto stackStorage = std::make_unique<StackStorage<int,1024>>;
	BoundedQueue<int> queue(std::move(stackStorage));
	//automatic cleanup
}
```