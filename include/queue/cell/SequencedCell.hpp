#pragma once
#include <atomic>
#include <specs.hpp>

/**
 * @brief A sequenced cell storing a value and its sequence index.
 *
 * Used in lock-free ring buffers and similar data structures to
 * associate a stored value with a monotonically increasing sequence index.
 *
 * @tparam T Type of the stored value.
 */
template<typename T>
struct SequencedCell;

#ifndef NCELL_PAD

/**
 * @brief Cache-line padded sequenced cell to avoid false sharing.
 *
 * Each instance is padded to exactly one cache line.
 */
template<typename T>
struct alignas(CACHE_LINE) SequencedCell {
    std::atomic<T>        val;  ///< Stored value.
    std::atomic<uint64_t> seq;  ///< Sequence index.
    char __pad[CACHE_LINE - (sizeof(std::atomic<T>) + sizeof(std::atomic<uint64_t>))];
};

#else

/**
 * @brief Compact sequenced cell with minimal alignment.
 *
 * Aligned to 16 bytes, without cache-line padding.
 */
template<class T>
struct alignas(16) SequencedCell {
    std::atomic<T>        val;  ///< Stored value.
    std::atomic<uint64_t> seq;  ///< Sequence index.
};

#endif
