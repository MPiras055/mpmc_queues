#pragma once
/**
 * @file SequencedCell.hpp
 * @brief A payload word beside a sequence word, for the algorithms that version each slot.
 * @ingroup cell
 */


#include <atomic>
#include <util/align.hpp>
#include <util/specs.hpp>  // defines CACHE_LINE


namespace cell {
/**
 * @brief A sequenced cell storing a value and its sequence index.
 *
 * This struct is used in lock-free ring buffers and concurrent queues.
 * Each cell stores:
 *  - a value of type `T` (atomically accessed)
 *  - a sequence counter used for turn-based synchronization
 *
 * Padding behavior is controlled by the template parameter `PadToCacheLine`.
 *
 * @tparam T                 Type of the stored value.
 * @tparam PadToCacheLine    If true, the cell is cache-line-sized to avoid false sharing.
 */
template <typename T, bool PadToCacheLine>
struct SequencedCell;

// -----------------------------------------------------------------------------
// Specialization: cache-line padded
// -----------------------------------------------------------------------------

/**
 * @brief A cache-line padded sequenced cell.
 *
 * This specialization pads the structure to exactly one cache line, preventing
 * false sharing when many producers or consumers update adjacent cells.
 *
 * @tparam T Type of the stored value.
 */
template <typename T>
struct alignas(CACHE_LINE) SequencedCell<T, true> {
    std::atomic<T>        val;  ///< Stored value.
    std::atomic<uint64_t> seq;  ///< Sequence index.
    CACHE_PAD(std::atomic<T>,std::atomic<uint64_t>);
};

// -----------------------------------------------------------------------------
// Specialization: compact (no padding)
// -----------------------------------------------------------------------------

/**
 * @brief A compact sequenced cell with minimal alignment.
 *
 * This specialization avoids padding and minimizes memory usage. It should be
 * used when false sharing is not a concern or when memory usage is of the essence
 *
 * @tparam T Type of the stored value.
 */
template <typename T>
struct alignas(
    sizeof(std::atomic<T>) + sizeof(std::atomic_uint64_t)
) SequencedCell<T, false> {
    std::atomic<T>        val;  ///< Stored value.
    std::atomic<uint64_t> seq;  ///< Sequence index.
};

}   //namespace cell
