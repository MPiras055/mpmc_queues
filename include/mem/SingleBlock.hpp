#pragma once
#include <mem/Layout.hpp>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

namespace mem {

/**
 * @brief CRTP base giving a type single-block (header + trailing arrays) allocation.
 *
 * The deriving type provides:
 *   - `static constexpr Plan<N> plan(std::size_t n)` describing its layout, and
 *   - a constructor `Derived(std::size_t n, mem::Blocks blk, ...)`.
 *
 * This replaces seven hand-rolled copies of the same factory, five `owns_buffer` flags
 * and five duplicated `operator delete` pairs.
 *
 * @note There is no `operator delete`. The old segments each carried a pair of them
 *       that branched on a runtime `owns_buffer` flag — reading that flag *after* the
 *       destructor had run. The flag only existed because one class served two
 *       allocation strategies; a segment has exactly one, so construction is
 *       create-only and destruction goes through destroy().
 *
 * @note Deriving from this class *is* the opt-in to co-allocation. There is no tag to
 *       misspell — the previous `IsCoAllocated` / `isCoAllocated` split silently sent
 *       one segment down the plain-`new` path for as long as it existed. A type that
 *       does not derive simply has no create(), which is an error at the call site.
 */
template <typename Derived>
struct SingleBlock {
    /// Allocate one block, place the header, and hand the regions to the constructor.
    template <typename... Args>
    [[nodiscard]] static Derived* create(std::size_t n, Args&&... args) {
        // Permanent, generic guard against the offset-arithmetic bug class: a
        // representative capacity is enough, since validity is shape-dependent, not
        // size-dependent. Evaluated at compile time.
        static_assert(Derived::plan(kProbe).valid(sizeof(Derived)),
                      "single-block layout is invalid: a region overlaps the header or "
                      "another region, or runs past the end of the block");

        const auto p = Derived::plan(n);
        void* raw = std::aligned_alloc(p.block_align, p.total);
        if (!raw) throw std::bad_alloc();
        return new (raw) Derived(n, Blocks{raw}, std::forward<Args>(args)...);
    }

    static void destroy(Derived* d) noexcept {
        if (!d) return;
        d->~Derived();
        std::free(d);
    }

private:
    /// Capacity used only to validate the layout's shape at compile time.
    static constexpr std::size_t kProbe = 64;
};

/// Deleter so a co-allocated object can be owned by a unique_ptr.
template <typename T>
struct Destroy {
    void operator()(T* p) const noexcept { SingleBlock<T>::destroy(p); }
};

template <typename T>
using unique_block = std::unique_ptr<T, Destroy<T>>;

} // namespace mem
