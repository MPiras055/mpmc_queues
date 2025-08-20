#pragma once
#include <IStoragePolicy.hpp>

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
};