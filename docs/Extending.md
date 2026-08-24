\page extending Extending the library

How to add each kind of thing, with a worked example for each. Read the section you need; they
do not depend on each other.

| I want to add… | section |
| --- | --- |
| a queue algorithm | [A new algorithm](#a-new-algorithm) |
| an object with co-allocated arrays | [Block construction](#block-construction) |
| a tagging / linkage / admission policy, or an option | [A new policy](#a-new-policy) |
| a place segments come from | [A new source](#a-new-source) |

Every section ends with the same [checklist](#the-checklist). Skipping it is how the two worst
bugs in this tree shipped.

---

## A new algorithm {#a-new-algorithm}

An algorithm is a bounded queue. Written once, it becomes both a standalone queue and a linked
segment, because the successor pointer is a policy rather than a member.

### The shape

```cpp
#pragma once
/**
 * @file Example.hpp
 * @brief A minimal bounded ring, to show the shape.
 * @ingroup algo
 */
#include <cell/PlainCell.hpp>
#include <core/SegmentTraits.hpp>
#include <linkage/Linkage.hpp>
#include <mem/SingleBlock.hpp>
#include <meta/OptionsPack.hpp>

namespace algo {

struct ExampleOpt {
    struct no_cell_padding {};              // a flag
    template <auto N> struct patience {};   // a value option -- note `auto`, not `size_t`
};

template <typename T, typename Opt = meta::EmptyOptions, typename Link = linkage::None>
    requires meta::AcceptsOnly<Opt, typename ExampleOpt::no_cell_padding,
                               meta::ValueOption<ExampleOpt::patience>>
class Example : public mem::SingleBlock<Example<T, Opt, Link>> {
    using Self = Example<T, Opt, Link>;

    static constexpr bool pad_cells = !Opt::template has<typename ExampleOpt::no_cell_padding>;
    static constexpr std::size_t kPatience =
        static_cast<std::size_t>(Opt::template get<ExampleOpt::patience, std::size_t{2}>);

public:
    using cell_type   = cell::PlainCell<uintptr_t, pad_cells>;
    using link_state  = typename Link::template state<Self>;
    using handle_type = typename link_state::handle;

    /// Read back the tuning that was resolved. See OptionsTest for why this is public.
    static constexpr std::size_t patience = kPatience;

    static constexpr auto plan(std::size_t n) noexcept;      // see Block construction
    Example(std::size_t n, mem::Blocks blk) noexcept;

    bool enqueue(T item) noexcept;
    bool dequeue(T& out) noexcept;
    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;

    // Only when linked. `requires` rather than #if, so the standalone type simply has no
    // such member and nothing can call it by mistake.
    void close()  noexcept requires(Link::is_linked);
    bool is_closed() const noexcept requires(Link::is_linked);
    bool reopen() noexcept requires(Link::is_linked);
    handle_type next() const noexcept requires(Link::is_linked);
    /// `current` receives whichever successor ends up installed, so a loser
    /// need not re-read next(). See linkage::Node::state::link_next.
    bool link_next(handle_type h, handle_type& current) noexcept requires(Link::is_linked);

private:
    [[no_unique_address]] link_state link_{};   // empty under linkage::None
    const std::size_t capacity_;
    cell_type* const cells_;
};

} // namespace algo
```

### Then the traits, which are mandatory

```cpp
/// @brief Capabilities of algo::Example as a linked segment.
template <typename T, typename Opt, typename Link>
struct core::segment_traits<algo::Example<T, Opt, Link>> {
    static constexpr bool needs_close_hint      = false;
    static constexpr bool needs_dequeue_prepare = false;
    static constexpr bool needs_inflight_drain  = false;
    static constexpr bool recyclable            = true;
    static constexpr bool can_store_null        = true;
};
MPMC_ASSERT_SEGMENT_TRAITS(algo::Example<int*, meta::EmptyOptions, linkage::Node<mem::PtrHandle>>);
```

`core::segment_traits` **has no primary definition**. Omitting a field is a compile error, not a
silently wrong default — which is the point. What each field means:

| field | say `true` when |
| --- | --- |
| `needs_close_hint` | re-entering `enqueue` on a closed segment is harmful, not merely wasteful. PRQ sets this: without it, consumers are driven down the unsafe-cell path and a bounded proxy livelocks (4 of 12 runs stalled without it, 0 of 42 with it) |
| `needs_dequeue_prepare` | the segment needs telling that a successor now exists before it can be drained exhaustively |
| `needs_inflight_drain` | insertion is not a single atomic step, so the proxy must not unlink while a producer is mid-insert. Requires `has_inflight()` |
| `recyclable` | `reopen()` genuinely restores the segment. A pooled source refuses a segment without it |
| `can_store_null` | the tagging scheme leaves a representation for a null payload |

### Finally the aliases

```cpp
namespace queue {  // standalone
template <typename T, typename Opt = meta::EmptyOptions>
using Example = algo::Example<T, Opt, linkage::None>;
}
namespace seg {    // linked segment
template <typename T, typename Opt = meta::EmptyOptions, typename HP = mem::PtrHandle>
using Example = algo::Example<T, Opt, linkage::Node<HP>>;
}
```

Omit whichever makes no sense. FAAArray and HQ have no `queue::` alias and are constrained on
`linkage::Linked`, because their indices only advance: standalone they accept exactly one
fill/drain cycle and refuse everything after. Expressing that as a constraint means the unsound
configuration cannot be *named*, let alone instantiated.

### The obligations no concept can state

This is where care is needed. `core::LinkedSegment` checks that the right functions exist. It
cannot check what they must *do*, and every one of these is a real bug that shipped:

- **A linked segment must close itself permanently when full.** Returning `false` is not enough:
  a producer that read `next() == nil` just before another linked a successor will enqueue into
  a segment that consumers have since drained and unlinked, and the item is lost. `algo::Mutex`
  did exactly this. Tested by `SegmentLifecycleTest.FillingToCapacityClosesTheSegment`.
- **`reopen()` must leave no payload behind.** A stale cell in a recycled segment is delivered a
  second time. SCQ measured 20 004 items consumed against 20 000 produced before its `reopen()`
  rebuilt both rings.
- **A destructive dequeue must be necessary.** If a consumer overwrites a cell a producer has
  claimed, it must be to break head-of-line blocking, not impatience — HQ burned a cell whenever
  a producer was two loads behind, costing up to 47 of 1024 slots.

---

## Block construction {#block-construction}

A segment is **one allocation**: the object and every array it owns, in a single block. Two
allocations would mean two cache-miss domains and two failure paths.

### How it fits together

`mem::SingleBlock<Derived>` is a CRTP base providing `create()` and `destroy()`. It asks
`Derived::plan(n)` where things go, allocates once with `std::aligned_alloc`, then placement-news
`Derived` with a `mem::Blocks` describing the regions.

```cpp
static constexpr auto plan(std::size_t n) noexcept {
    mem::LayoutBuilder b{sizeof(Self), alignof(Self)};   // start past the header
    mem::Plan<1> p{};                                    // 1 == number of regions
    p.regions[0]  = b.add(n * sizeof(cell_type), alignof(cell_type));
    p.total       = b.total();
    p.block_align = b.block_align();
    return p;
}

Example(std::size_t n, mem::Blocks blk) noexcept
    : capacity_{n}, cells_{blk.template at<cell_type>(plan(n).regions[0])} {}
```

Rules:

- `plan()` is `static constexpr` and **must be callable before the object exists** — it is how
  the allocation is sized. It may only depend on `n`.
- Call `b.add()` once per region, in the order the regions are laid out. Each returns the offset
  to store in `p.regions[i]`; `Plan<N>`'s `N` is the region count.
- `b.block_align()` is the strictest alignment any region needs, and is what
  `std::aligned_alloc` is given.
- Recompute `plan(n)` in the constructor rather than passing offsets — it is `constexpr` and
  folds away.

More than one region is ordinary: `algo::SCQ` co-allocates two index rings and a data buffer in
one block. `MemoryLayoutTest` includes a region-overlap guard, because an arithmetic slip here
produces two regions that alias and a corruption that looks like an algorithm bug.

### Keeping contended members apart

`util/align.hpp` holds everything about placement: the cache-line size, `align::align_up`, and
the macros. A head and a tail written by different threads must not share a line, and the usual
case is one member alone on its own:

```cpp
CACHE_LINE_MEMBER(std::atomic<uint64_t>, head_, {0});   // aligned to the start of a line,
CACHE_LINE_MEMBER(std::atomic<uint64_t>, tail_, {0});   // padded to the end of it
```

The two primitives are there when that shape does not fit — several members sharing one line, or
a type whose name contains a top-level comma, which the preprocessor would split:

```cpp
CACHE_ALIGN std::atomic<T> val;
std::atomic<uint64_t> seq;
CACHE_PAD(std::atomic<T>, std::atomic<uint64_t>);   // fill the rest of the shared line
```

`CACHE_PAD` costs nothing when the members already fill the line: `align::Padding<0>` is empty
and `[[no_unique_address]]` removes it.

---

## A new policy {#a-new-policy}

Four kinds, all resolved at compile time, all costing nothing when empty.

### Tagging — how a cell word encodes its state

Model `cell::Tagging<Tag, T>`: `empty()`, `consumed()`, the `is_*` predicates, `encode`/`decode`,
and `can_store_null`. Add `claim()` / `is_claim()` to model `cell::ClaimingTag`, which the
three-step insert in PRQ and PSCQ requires.

The states must be **mutually exclusive and exhaustive**. The reason is on the record: three
segments previously hand-rolled three schemes, two of which spelled "reserved" identically and
meant different things — one including the empty word, one excluding it. Unifying on a
`reserved` predicate would have silently broken one of them.

### Linkage — whether a successor handle exists

`linkage::None` and `linkage::Node<HandlePolicy>`. A new one is unlikely; a new *handle* policy
is plausible — see `mem::PtrHandle` and `mem::IndexHandle<N>`, which are three lines each.

### Admission — what stops a proxy admitting an item

Model `core::AdmissionPolicy`: a `Config`, a static `config(segment, chunks)`, `bounded`,
`try_admit()` / `cancel_admit()`, the four `on_*` hooks, `bound()` and `capacity(segment)`.

**Say where you want to be asked.** `admit_point` is the field that makes two policies
genuinely different rather than differently parameterised:

| `core::AdmitPoint` | when | for |
| --- | --- | --- |
| `Enqueue` | top of every enqueue, before the traversal | policies that **reserve** — the claim must be taken before anything commits, or concurrent producers each pass a test and all commit past the ceiling. `admit::ItemCount` fetch-adds a ticket and rolls back on refusal |
| `SegmentLink` | only once the tail has refused and a segment is about to be acquired | policies that count **segments** — whether an enqueue links one is not known until then, and most link nothing |

Getting this wrong is not cosmetic. `admit::SegmentCount` was asked at `Enqueue`, so it refused
while the tail still had free slots: on 8-slot segments a 4-chunk queue held 25 of an advertised
32, and a **one-chunk queue held zero**, because one segment's worth of bound left no room to
link even the first successor.

Reserve rather than test where you can. If you cannot — because the answer depends on something
the proxy has not decided yet — use `SegmentLink` and accept a ceiling that is approximate to
within the number of producers acting concurrently.

If the policy is unbounded, make it an **empty struct** — `admit::None` is, so under
`[[no_unique_address]]` an unbounded proxy carries no counter and no cache line.

### Options — flags and values

```cpp
struct ThingOpt {
    struct fast_path {};                 // flag: presence is the whole meaning
    template <auto N> struct budget {};  // value: `auto`, always
};

// In the requires-clause -- flags by name, values wrapped:
requires meta::AcceptsOnly<Opt, typename ThingOpt::fast_path,
                           meta::ValueOption<ThingOpt::budget>>

// Reading them back:
static constexpr bool kFast = Opt::template has<typename ThingOpt::fast_path>;
static constexpr std::size_t kBudget =
    static_cast<std::size_t>(Opt::template get<ThingOpt::budget, std::size_t{64}>);
```

Three traps:

1. **`template <auto N>`, not `template <std::size_t N>`.** `get` takes a `template <auto> class`
   parameter and a less general template will not bind to it.
2. **Cast the result.** `get` returns the *option's* type when one is present and the default's
   type when it is not, so `budget<64>` arrives as `int`. The value is right and only the type is
   wrong, which is how it stays hidden until a narrowing or a sign extension.
3. **Add it to `AcceptsOnly`.** Otherwise `has<>`/`get<>` answer "not requested" for a misspelled
   or foreign option and it silently does nothing — which is exactly what `AcceptsOnly` exists to
   prevent.

Default every option to today's behaviour and assert it in `OptionsTest`, so making something
configurable cannot quietly change it.

---

## A new source {#a-new-source}

A source decides where segments come from, when they may be reused, and how a thread is
protected while it holds one. Model `core::SegmentSource`:

```cpp
using handle;          // S* for an allocating source, VersionedIndex<N> for a pooled one
using guard;           // RAII protection scope
using session;         // RAII thread registration
using thread_payload;  // the *caller's* per-thread state, carried in this source's node

static constexpr bool recycles;
static constexpr handle nil() noexcept;
static constexpr std::size_t live_segments() noexcept;  // 0 == unbounded

guard  pin() noexcept;
handle protect(guard&, handle) noexcept;
bool   renew(guard&) noexcept;   // true == protection moved, prior handles are void
S*     deref(handle) const noexcept;

std::optional<handle> acquire();                 // nullopt == the memory bound
template <typename Retry>
std::optional<handle> acquire(Retry&& worth_waiting);
void   discard(handle) noexcept;                 // never published: reclaim now
void   retire(handle) noexcept;                  // was reachable: defer
session join();
```

Points that are easy to get wrong:

- **`protect()` must never invalidate anything, and `renew()` is where the moving happens.**
  That split is what keeps sources composable. `protect(g, h)` adds `h` to what is protected
  and takes nothing away, so a caller holding an earlier handle stays safe. `renew(g)` says the
  opposite: *"I hold nothing I obtained before now — move me forward."* An epoch source
  republishes its pin there; a per-handle one like `Hazard` does nothing, because it has no
  epoch to hold back.

  The precondition is the caller's to keep, and it is sharper than it looks: after renewing,
  anything retired under the old epoch may be freed, **including the segment a stale local
  still names**. So the pattern at every call site is renew → re-read a shared anchor →
  protect, never renew → reuse a local. `LinkedProxy` does exactly that on each of its retry
  paths, and the one site where it deliberately does *not* renew (after losing a `link_next`
  race, so it can use the winner the CAS handed back) says so in a comment.

  **`renew()` returns whether it actually moved**, and the bool is not decoration. False means
  the call was a no-op, so the obligation above does not apply and the caller's existing handles
  are still good. `Hazard` returns a constant false; `Pool` returns false when the thread was
  already at the current stage. `LinkedProxy::enqueue` uses it to tell two situations apart that
  the source cannot distinguish for it: an empty pool because this thread was itself blocking the
  rotation (renew moved → worth retrying) versus an empty pool because the memory bound is
  genuinely reached (renew was a no-op → refuse). Without the gate it retried on both, measured
  at 73k pointless re-traversals per 2M items.

  > **Breaking change.** `renew()` previously returned `void`. An out-of-tree source will now
  > fail `core::SegmentSource` rather than compile with the wrong signature.

- **`live_segments()`** returns 0 unless the source caps how many segments can be outstanding.
  A pooled one answers with its slot count, and `LinkedProxy` uses it as the divisor when
  splitting a total capacity across the segments that will exist.

- **`discard` and `retire` are different operations**, and the distinction is load-bearing. A
  segment that lost the `link_next` race was never visible to anyone, so it needs no grace
  period; one that was linked does. Collapsing them either leaks latency or frees a live
  segment.
- **`thread_payload` is the caller's, not yours.** The source stores it in the same registry
  node as its own per-thread state, so `guard::payload()` reaches it with the thread-local
  lookup `pin()` already did. Build on `util::threading::ThreadRegistry`, which both existing
  sources use, rather than a side array.
- **`recycles == true` obliges the segment to be `recyclable`**, and the proxy static-asserts it.
  It also means `acquire()` must return a segment that is genuinely reset — the reopen belongs
  here, in the source that knows the segment is dirty, not in the proxy.
- **The `Retry` overload may ignore its argument.** `Hazard` does: it allocates, so it can never
  run dry and has nothing to wait for. `Pool` spins on it. Whether waiting makes sense is the
  caller's question, which is why the caller supplies the predicate.
- **If you spin while holding protection, bound it.** In `Pool` the spin runs while pinned, and
  two threads pinned a stage apart can each wait for the other; giving up is what drops the pin
  the other is blocked on. An unbounded spin there is a livelock, not a slowdown.

---

## The checklist {#the-checklist}

Whatever you added:

1. **Register it** — one `Entry<"name", Type>` line in `include/registry/Registry.hpp` reaches
   `benchmark --list`, `RegistryConformanceTest` and `ConcurrencyTest` at once.
2. **Add it to the typed suites.** For a segment that means `SegmentLifecycleTest`'s
   `SegmentTypes` list. This is not optional bookkeeping: `seg::Mutex` was missing from it, and
   that is precisely why "a linked segment must close itself when full" shipped broken — the
   suite already contained a test for that exact obligation, and it simply never ran against
   Mutex.
3. **Write down the obligations your concepts cannot express**, and add a test for each.
4. **Assert the defaults** in `OptionsTest` if you added options.
5. **Build all four configurations** and run the suites — see \ref testing.
