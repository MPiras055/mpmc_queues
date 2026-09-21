#pragma once
/**
 * @file Unbounded.hpp
 * @brief An unbounded SPSC queue: a list of spsc::Linear chunks, freed without reclamation.
 * @ingroup spsc
 */

#include <mem/SingleBlock.hpp>
#include <spsc/Buffer.hpp>
#include <atomic>
#include <cstddef>
#include <memory>

namespace spsc {

/**
 * @brief Unbounded single-producer / single-consumer queue built from the two bounded buffers.
 *
 * A list of spsc::Linear chunks joined by one `next` pointer each. The producer holds the tail
 * chunk and the consumer the head chunk, both privately. The producer writes the item into a new
 * chunk *before* publishing `next`, so the consumer never advances onto an empty chunk.
 *
 * ### Why freeing needs no hazard pointers or epochs
 *
 * The consumer frees a chunk only when its read index has reached the chunk's **capacity** and
 * `next` is non-null. A linear chunk never wraps, so a read index at capacity means the producer
 * wrote every cell, and a non-null `next` means it has already made its last access to the chunk
 * -- publishing `next` is that access, and the consumer's acquire load of it synchronises with
 * it. The producer provably cannot touch a chunk the consumer is freeing. That is the whole
 * reason this exists rather than reusing `LinkedProxy`.
 *
 * With one chunk, `next` is null, so it is never freed and the consumer simply reports empty.
 *
 * Drained chunks go back to the producer through a small spsc::Ring of chunk pointers, so malloc
 * stays off the hot path; when that ring is full the chunk is freed instead. The reverse channel
 * is itself an SPSC buffer -- this queue is made of nothing but the two flavours in Buffer.hpp.
 *
 * Same restriction as the buffers: `T{}` is the empty marker and cannot be enqueued.
 */
template <Storable T>
class Unbounded {
    struct Chunk : mem::SingleBlock<Chunk> {
        using cell_type = typename Linear<T>::cell_type;

        static constexpr auto plan(std::size_t n) noexcept {
            mem::LayoutBuilder b{sizeof(Chunk), alignof(Chunk)};
            mem::Plan<1> p{};
            p.regions[0] = b.add(n * sizeof(cell_type), alignof(cell_type));
            p.total = b.total();
            p.block_align = b.block_align();
            return p;
        }

        Chunk(std::size_t n, mem::Blocks blk) noexcept
            : buf{n, blk.template at<cell_type>(plan(n).regions[0])} {}

        Linear<T> buf;
        /// Written once, by the producer, as its last access to this chunk.
        CACHE_LINE_MEMBER(std::atomic<Chunk*>, next, {nullptr});
    };

    using Recycle = Ring<Chunk*>;

public:
    using value_type = T;

    /**
     * @param chunk_capacity items per chunk.
     * @param recycled       drained chunks kept for reuse rather than freed; rounded up to a
     *                       power of two.
     */
    explicit Unbounded(std::size_t chunk_capacity = 1024, std::size_t recycled = 8)
        : chunk_capacity_{Linear<T>::capacity_for(chunk_capacity)},
          recycle_cells_{new typename Recycle::cell_type[Recycle::capacity_for(recycled)]},
          recycle_{Recycle::capacity_for(recycled), recycle_cells_.get()} {
        tail_ = head_ = Chunk::create(chunk_capacity_);
    }

    Unbounded(const Unbounded&) = delete;
    Unbounded& operator=(const Unbounded&) = delete;

    ~Unbounded() {
        for (Chunk* c = head_; c;) {
            Chunk* n = c->next.load(std::memory_order_relaxed);
            Chunk::destroy(c);
            c = n;
        }
        for (Chunk* c = nullptr; recycle_.dequeue(c);) Chunk::destroy(c);
    }

    /// Never refuses. Allocates only when the tail chunk is full and none is waiting for reuse.
    bool enqueue(T v) {
        if (tail_->buf.enqueue(v)) return true;
        Chunk* n = nullptr;
        if (recycle_.dequeue(n)) {
            n->buf.reset();
            n->next.store(nullptr, std::memory_order_relaxed);
        } else {
            n = Chunk::create(chunk_capacity_);
        }
        n->buf.enqueue(v); // the item goes in before the chunk becomes reachable
        tail_->next.store(n, std::memory_order_release);
        tail_ = n;
        pushed_.store(pushed_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return true;
    }

    /// @return false only if the queue is empty. Leaves @p out untouched in that case.
    bool dequeue(T& out) noexcept {
        for (;;) {
            if (head_->buf.dequeue(out)) return true;
            if (!head_->buf.drained()) return false; // the producer has not got this far yet
            Chunk* n = head_->next.load(std::memory_order_acquire);
            if (!n) return false;
            Chunk* old = head_;
            head_ = n;
            if (!recycle_.enqueue(old)) Chunk::destroy(old);
            popped_.store(popped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }

    /// Items per chunk.
    std::size_t chunk_capacity() const noexcept { return chunk_capacity_; }

    /// Chunks the producer has moved onto since construction. For tests of growth.
    std::size_t chunks_linked() const noexcept { return pushed_.load(std::memory_order_relaxed); }

    /// Chunks the consumer has left behind since construction.
    std::size_t chunks_retired() const noexcept { return popped_.load(std::memory_order_relaxed); }

private:
    std::size_t chunk_capacity_;
    std::unique_ptr<typename Recycle::cell_type[]> recycle_cells_;
    Recycle recycle_;
    /// Producer-private.
    CACHE_ALIGN Chunk* tail_ = nullptr;
    std::atomic<std::size_t> pushed_{0};
    CACHE_PAD(Chunk*, std::atomic<std::size_t>);
    /// Consumer-private.
    CACHE_ALIGN Chunk* head_ = nullptr;
    std::atomic<std::size_t> popped_{0};
    CACHE_PAD(Chunk*, std::atomic<std::size_t>);
};

} // namespace spsc
