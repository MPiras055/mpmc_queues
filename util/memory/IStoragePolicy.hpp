#pragma once

/**
 * Basic interface that defines an abstract storage policy
 */
template <typename T>
class IStoragePolicy {
public:

    virtual T* data() = 0;

    virtual size_t capacity() const = 0;

    virtual ~IStoragePolicy() = default;

};