#pragma once
#include <atomic>
#include <cassert>
#include <bit.hpp>

namespace util::hazard {

/**
 * @brief A cell that stores an "active" epoch number with a single-writer assumption.
 *
 * This class encodes both an "active" flag and a 63-bit epoch value into a single
 * 64-bit atomic word. The most significant bit (MSB) is reserved for the "active"
 * flag, while the lower 63 bits store the epoch number.
 *
 * Layout of the 64-bit word:
 *
 *   [ MSB | lower 63 bits ]
 *   [ active flag | epoch ]
 *
 * - **active flag**: 1 if the cell is active, 0 if inactive.
 * - **epoch**: a 63-bit epoch value used for versioning or hazard tracking.
 *
 * This type is designed for scenarios where only a single writer updates the cell,
 * but multiple readers may concurrently read it.
 */
struct SingleWriterCell {
    /**
     * @brief Marks the cell as active and stores the given epoch value.
     *
     * @param epoch Epoch value to store (must have MSB clear).
     *
     * The MSB of @p epoch must be zero, otherwise an assertion will fail.
     * Internally, this method sets the MSB of the stored value to indicate
     * the active state.
     */
    void activate(uint64_t epoch) {
        assert(bit::getMSB64(epoch) == 0 &&
               "SingleWriterCell::setActiveEpoch - epoch parameter MSB cannot be set");
        epochActive.store(bit::setMSB64(epoch), std::memory_order_release);
    }

    /**
     * @brief Clears the active flag, leaving the epoch value unchanged.
     *
     * After calling this, the cell is considered inactive, though the epoch
     * value remains stored in the lower 63 bits.
     * 
     * @note this method is idempotent
     */
    void deactivate() noexcept {
        epochActive.fetch_and(bit::LSB63_MASK, std::memory_order_release);
    }

    /**
     * @brief Checks whether the cell is currently active.
     *
     * @return true if the MSB is set (active), false otherwise.
     */
    bool isActive() const noexcept {
        return bit::getMSB64(epochActive.load(std::memory_order_acquire)) != 0;
    }

    /**
     * @brief Reads both the active flag and epoch value in a single snapshot.
     *
     * @param[out] active Will be set to true if the cell is active, false otherwise.
     * @param[out] epoch  Will be set to the stored 63-bit epoch value.
     *
     * This method loads the 64-bit atomic once and decodes it into its
     * constituent parts.
     * 
     * @note this method is safe
     */
    void snapshot(bool& active, uint64_t& epoch) const noexcept {
        uint64_t snapshot = epochActive.load(std::memory_order_acquire);
        active = bit::getMSB64(snapshot) != 0;
        epoch  = bit::get63LSB(snapshot);
    }

private:
    /// Atomic word storing both active flag (MSB) and epoch (lower 63 bits).
    std::atomic<uint64_t> epochActive{0};
};

} // namespace util::hazard
