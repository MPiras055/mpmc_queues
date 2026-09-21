#pragma once
/**
 * @file AllToAll.hpp
 * @brief A P x C matrix of SPSC buffers in one allocation: all-to-all, farm emitter, collector.
 * @ingroup block
 *
 * Every queue elsewhere in this tree is MPMC and pays for it on a shared head and tail. When the
 * topology is known -- producer *i* only ever hands work to consumer *j* -- the contended ring can
 * be replaced by one SPSC buffer per (producer, consumer) pair, each with exactly one writer and
 * one reader, and no compare-and-swap anywhere on the hot path.
 *
 * ```cpp
 * auto* block = block::AllToAll<Item>::create(4096, 4, 4);   // total capacity, P, C
 *
 * auto p = block->producer(i);        // once per thread, i in [0, P)
 * while (...) p.enqueue(item);        // no id lookup, no shared cursor
 *
 * auto c = block->consumer(j);        // j in [0, C)
 * c.dequeue(out);
 * block::AllToAll<Item>::destroy(block);
 * ```
 *
 * The farm emitter is this block at `P = 1`, the collector at `C = 1`; block::FarmEmitter and
 * block::FarmCollector only pin that degree.
 *
 * ### This is not a core::Queue, on purpose
 *
 * A buffer with two writers is not an SPSC buffer, so "any thread, any operation" cannot hold.
 * The contract is the handle: **at most one live handle per row and per column**, each used by
 * one thread. queue::ShardQueue is the core::Queue bridge for the benchmark.
 *
 * @warning A consumer that stops early strands its column: no other thread owns it, so nothing
 *          else will drain it. That is inherent to the topology, and why these blocks stay out
 *          of the general test suites.
 *
 * ### Layout
 *
 * `mem::Plan<2>`: region 0 is `P*C` cache-line-aligned buffer headers, region 1 one contiguous
 * cell arena. Both are row-major (`[p][c]`), so a producer's C buffers are adjacent and its scan
 * walks forward through memory. Buffer *k*'s cells start at `arena + k * stride`, with the
 * stride rounded to a cache line so no two buffers' cells ever share one -- which for the packed
 * spsc::Linear cells is the only place padding is needed at all.
 *
 * Two regions rather than `2*P*C` because `Plan<N>` fixes N at compile time and the degree is a
 * runtime value. Not derived from mem::SingleBlock, and that matters: its variadic
 * `create(n, Args...)` satisfies `core::BlockAllocated` for any arity, so a shaped block deriving
 * from it would be classified as a plain block-allocated queue.
 */

#include <block/Strategy.hpp>
#include <mem/Layout.hpp>
#include <spsc/Buffer.hpp>
#include <util/align.hpp>
#include <util/bit.hpp>
#include <cassert>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <utility>

namespace block {

/**
 * @tparam T        item type; `T{}` is reserved as empty (see spsc::Ring).
 * @tparam Strategy block::RoundRobin or block::Sticky.
 * @tparam Buf      spsc::Ring (the default: reusable) or spsc::Linear (fills once).
 */
template <typename T, typename Strategy = RoundRobin, template <typename> class Buf = spsc::Ring>
class AllToAll {
public:
    using value_type = T;
    using buffer_type = Buf<T>;
    using cell_type = typename buffer_type::cell_type;
    using strategy = Strategy;

    /// Items per buffer: the total split across the matrix, floored at 2, a power of two.
    static constexpr std::size_t per_buffer_for(std::size_t capacity, std::size_t p,
                                                std::size_t c) noexcept {
        const std::size_t n = p * c;
        const std::size_t share = (capacity + n - 1) / n;
        return bit::next_pow2(share < 2 ? std::size_t{2} : share);
    }

    /// Bytes between the starts of two adjacent buffers' cell runs.
    static constexpr std::size_t stride_for(std::size_t per_buffer) noexcept {
        return align::align_up(per_buffer * sizeof(cell_type), CACHE_LINE);
    }

    static constexpr mem::Plan<2> plan(std::size_t capacity, std::size_t p, std::size_t c) noexcept {
        const std::size_t n = p * c;
        mem::LayoutBuilder b{sizeof(AllToAll), alignof(AllToAll)};
        mem::Plan<2> pl{};
        pl.regions[0] = b.add(n * sizeof(buffer_type), alignof(buffer_type));
        pl.regions[1] = b.add(n * stride_for(per_buffer_for(capacity, p, c)), CACHE_LINE);
        pl.total = b.total();
        pl.block_align = b.block_align();
        return pl;
    }

    /// @throws std::invalid_argument on a zero degree, std::bad_alloc on allocation failure.
    [[nodiscard]] static AllToAll* create(std::size_t capacity, std::size_t p, std::size_t c) {
        // The same compile-time guard mem::SingleBlock applies: a representative shape is
        // enough, since validity depends on the layout's shape and not its size.
        static_assert(plan(64, 2, 2).valid(sizeof(AllToAll)),
                      "AllToAll layout is invalid: a region overlaps the header or another "
                      "region, or runs past the end of the block");
        if (p == 0 || c == 0) throw std::invalid_argument("block::AllToAll: zero degree");
        const auto pl = plan(capacity, p, c);
        void* raw = std::aligned_alloc(pl.block_align, pl.total);
        if (!raw) throw std::bad_alloc();
        return ::new (raw) AllToAll(capacity, p, c, mem::Blocks{raw});
    }

    static void destroy(AllToAll* b) noexcept {
        if (!b) return;
        b->~AllToAll();
        std::free(b);
    }

    AllToAll(const AllToAll&) = delete;
    AllToAll& operator=(const AllToAll&) = delete;

private:
    /// A handle's private copies of its D buffers' indices, on lines of their own.
    class Cursors {
    public:
        Cursors() noexcept = default;
        explicit Cursors(std::size_t n)
            : v_{static_cast<std::size_t*>(
                  std::aligned_alloc(CACHE_LINE, align::align_up(n * sizeof(std::size_t), CACHE_LINE)))} {
            if (!v_) throw std::bad_alloc();
        }
        Cursors(Cursors&& o) noexcept : v_{std::exchange(o.v_, nullptr)} {}
        Cursors& operator=(Cursors&& o) noexcept {
            std::swap(v_, o.v_);
            return *this;
        }
        ~Cursors() { std::free(v_); }
        std::size_t& operator[](std::size_t i) noexcept { return v_[i]; }

    private:
        std::size_t* v_ = nullptr;
    };

    /**
     * @brief A row (producer) or a column (consumer): D buffers, `stride` headers apart.
     *
     * Carries everything the hot path needs -- base, stride, degree, the private indices and the
     * strategy cursor -- so an operation reads nothing shared but the buffer headers and cells.
     */
    template <bool IsProducer>
    class Handle {
    public:
        Handle() noexcept = default;

        /// @return false only if every buffer in the row refused.
        bool enqueue(T v) noexcept
            requires IsProducer
        {
            return Strategy::run(cursor_, degree_, [&](std::size_t k) noexcept {
                return base_[k * stride_].put(idx_[k], v);
            });
        }

        /// @return false only if every buffer in the column was empty; @p out is then untouched.
        bool dequeue(T& out) noexcept
            requires(!IsProducer)
        {
            return Strategy::run(cursor_, degree_, [&](std::size_t k) noexcept {
                return base_[k * stride_].take(idx_[k], out);
            });
        }

        std::size_t degree() const noexcept { return degree_; }
        explicit operator bool() const noexcept { return base_ != nullptr; }

    private:
        friend class AllToAll;
        Handle(buffer_type* base, std::size_t stride, std::size_t degree)
            : base_{base}, stride_{stride}, degree_{degree}, idx_{degree} {
            // Resume from the published indices, so a handle re-taken later is still correct.
            for (std::size_t k = 0; k < degree; ++k)
                idx_[k] = IsProducer ? base[k * stride].write_index() : base[k * stride].read_index();
        }

        buffer_type* base_ = nullptr;
        std::size_t stride_ = 0;
        std::size_t degree_ = 0;
        std::size_t cursor_ = 0;
        Cursors idx_{};
    };

public:
    using Producer = Handle<true>;
    using Consumer = Handle<false>;

    /// Row @p i. @pre at most one live Producer per row, used by one thread.
    Producer producer(std::size_t i) {
        assert(i < p_ && "block::AllToAll: producer index out of range");
        return Producer{buffers_ + i * c_, 1, c_};
    }

    /// Column @p j. @pre at most one live Consumer per column, used by one thread.
    Consumer consumer(std::size_t j) {
        assert(j < c_ && "block::AllToAll: consumer index out of range");
        return Consumer{buffers_ + j, c_, p_};
    }

    std::size_t producers() const noexcept { return p_; }
    std::size_t consumers() const noexcept { return c_; }
    std::size_t per_buffer() const noexcept { return per_buffer_; }
    std::size_t stride() const noexcept { return stride_; }

    /// The buffer producer @p i shares with consumer @p j.
    const buffer_type& buffer(std::size_t i, std::size_t j) const noexcept {
        return buffers_[i * c_ + j];
    }

    /// Total items the block holds: every buffer's capacity, which may exceed what was asked.
    std::size_t capacity() const noexcept { return p_ * c_ * per_buffer_; }

    /// Sum of the buffers' sizes. Approximate under concurrency.
    std::size_t size() const noexcept {
        std::size_t s = 0;
        for (std::size_t k = 0; k < p_ * c_; ++k) s += buffers_[k].size();
        return s;
    }

private:
    AllToAll(std::size_t capacity, std::size_t p, std::size_t c, mem::Blocks blk)
        : p_{p}, c_{c}, per_buffer_{per_buffer_for(capacity, p, c)},
          stride_{stride_for(per_buffer_)} {
        const auto pl = plan(capacity, p, c);
        buffers_ = blk.template at<buffer_type>(pl.regions[0]);
        auto* arena = blk.template at<std::byte>(pl.regions[1]);
        for (std::size_t k = 0; k < p * c; ++k)
            ::new (static_cast<void*>(buffers_ + k))
                buffer_type(per_buffer_, reinterpret_cast<cell_type*>(arena + k * stride_));
    }

    ~AllToAll() {
        for (std::size_t k = 0; k < p_ * c_; ++k) buffers_[k].~buffer_type();
    }

    std::size_t p_, c_, per_buffer_, stride_;
    buffer_type* buffers_ = nullptr;
};

/// One producer fanning out to @p consumers: AllToAll at `P = 1`.
template <typename T, typename Strategy = RoundRobin, template <typename> class Buf = spsc::Ring>
struct FarmEmitter {
    using type = AllToAll<T, Strategy, Buf>;
    [[nodiscard]] static type* create(std::size_t capacity, std::size_t consumers) {
        return type::create(capacity, 1, consumers);
    }
};

/// @p producers funnelling into one consumer: AllToAll at `C = 1`.
template <typename T, typename Strategy = RoundRobin, template <typename> class Buf = spsc::Ring>
struct FarmCollector {
    using type = AllToAll<T, Strategy, Buf>;
    [[nodiscard]] static type* create(std::size_t capacity, std::size_t producers) {
        return type::create(capacity, producers, 1);
    }
};

} // namespace block
