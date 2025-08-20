#pragma once
#include <cstddef>
#include <IStoragePolicy.hpp>

template<typename T, size_t N>

class StackStorage: public IStoragePolicy<T> {
public:
	StackStorage() = default;
	
	T* data() override {
		return buffer_;
	}
	
	size_t capacity() const override {
		return N;
	}

    ~StackStorage() override = default;

private:
	T buffer_[N];
};