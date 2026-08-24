> **Archived.** Diagnosis of the pre-refactor codebase and the proposal that followed. Superseded by docs/notes/As Shipped.md.
>
> Kept for the design history; nothing here describes the current tree.

# Architecture Patterns — Current State and Proposal

> **Historical.** Sections 0-3 diagnose the *pre-refactor* code and remain an accurate
> record of what was wrong; the file:line references point at code that has since been
> deleted. Section 4's proposals were superseded during implementation — most visibly
> §4.1's `AnyQueue`, which was dropped because a hand-rolled vtable is still a vtable. See
> **[[As Shipped]]** for the current design.

This note has two halves. The first names the patterns the codebase actually uses today, with
`file:line` evidence, and explains what each one costs. The second proposes replacements chosen for
three properties: **better encapsulation**, **harder to get wrong**, and **cheap to derive new
implementations from**.

Everything asserted about the current tree was checked by compiling, not by reading. The commands
are in [[#Appendix — how the claims were checked]].

Related notes: [[Queue Interfaces]] (superseded — it still describes `NotLinkedSegment<T>` and
`LinkedTag<T>`, neither of which exists any more), [[Recycled Queue]], [[Queue Architecture for
Bounded and Unbounded Segments]], [[Epoch-Based Segment Recycler]], [[Hazard]],
[[Queue Storage Policies]].

---

## 0. Where things stand

Compiling every header standalone, every unit test, and the benchmark:

| Target | Result |
| --- | --- |
| 34 of 40 headers | compile |
| `Recycler.hpp`, `BoundedMemProxy.hpp`, `queue_impl.hpp` | fail |
| `Buckets.hpp`, `EpochCell.hpp`, `VersionedIndex.hpp` | fail — and nothing includes them |
| 6 of 7 unit tests, plus `src/sim/benchmark.cpp` | compile |
| `src/test/unit/BoundedProxyTest.cpp` | fails |

The single root cause of the compiling failures is `Recycler.hpp:81,326`, which names
`queue::LFringSlab<size_t>`. That type does not exist; the compatibility layer introduced in commit
`41306ff` is called `LFring`. The same lines also reference an undeclared member `buckets_`. The
breakage propagates `Recycler.hpp` → `include/linked/BoundedMemProxy.hpp` →
`include/util/queue_impl.hpp` → `BoundedProxyTest.cpp`, which is the only test covering
`BoundedMemProxy`. That proxy is currently both uncompilable and untested.

Separately, three headers under `include/util/hazard/Recycler/` are orphaned: `Buckets.hpp`,
`EpochCell.hpp` and `VersionedIndex.hpp` still use the *pre-move* flat include paths
(`#include <specs.hpp>`, `<bit.hpp>`, `<OptionsPack.hpp>`, `<SequencedCell.hpp>`), so they cannot
compile, and no file in `include/` or `src/` includes any of them. `Recycler.hpp` re-defines its own
`EpochCell` inline at line 33 rather than including the file of that name. Alongside
`src/test/unit/SegmentRecyclerTest.cpp.errored_out`, this is roughly 600 lines of dead weight that
still looks live.

This is not the cause of the difficulty, though — it is a symptom. The rest of the note is about the
cause.

---

## 1. Layer map

```
include/
  meta/           OptionsPack                        — compile-time option packs
  cell/           SequencedCell, PlainCell           — the storage unit
  segment/base/   IQueue, ILinkedSegment, IProxy     — the contracts
  segment/queue/  Vyukov, VyukovNoABA, VyukovDCAS,   — standalone bounded queues
                  SCQueue, PSCQueue, LFring, MutexQueue
  segment/linked/ LinkedVyukov, LinkedPRQ,           — segments meant to be chained
                  LinkedFAAArray, LinkedSCQ, HQSegment
  linked/         UnboundedProxy, Bounded{Counter,   — proxies that chain segments
                  Chunk,Mem}Proxy                       into a whole queue
  util/           hazard/, memory/, threading/,      — reclamation, tickets, timing, bits
                  timing/, atomic/, bit, specs
src/
  test/unit/      7 gtest binaries
  sim/            benchmark.cpp
```

Two frictions are visible from the map alone.

**The directory split does not mean what it looks like.** `include/linked/` holds proxies;
`include/segment/linked/` holds segments. A reader looking for "the linked stuff" finds half of it.

**Five namespaces are in play for one library.** `queue::` (standalone queues, `LFring`),
`linked::` (most segments), `queue::segment::` (only `HQSegment`), `base::` (the contracts — though
both `ILinkedSegment.hpp:102` and `IProxy.hpp:70` close with `// namespace meta`, a leftover from an
earlier name), `util::hazard::` / `util::threading::` (utilities), and the **global namespace**,
which currently holds all four proxies, `BoundedMemProxyOpt`, `VersionedIndex`, `HeapOwner`,
`NullOwner`, `CoAlloc`, and `is_co_allocated_v`.

---

## 2. Current patterns

Each subsection: what the pattern is, where it lives, what it buys, and how it fails.

### 2.1 Dual abstract base

A linked segment inherits **two** unrelated abstract classes:

```cpp
// LinkedVyukov.hpp:27
class LinkedVyukov :
    public queue::VyukovBuffer<T, Opt, LinkedVyukov<...>>,   // which itself : public base::IQueue<T>
    public base::ILinkedSegment<T, Next>
```

`base::IQueue<T>` (`IQueue.hpp:17`) declares `enqueue(const T)`, `dequeue(T&)`, `capacity()`,
`size()`. `base::ILinkedSegment<T,NextT>` (`ILinkedSegment.hpp:21`) declares `enqueue(T, bool info)`,
`dequeue(T&, bool)`, `close()`, `open()`, `isClosed()`, `isOpened()`, `capacity()` — and holds a
protected data member `next`.

*What it buys:* runtime polymorphism, and a written-down contract.

*How it fails:*

- `capacity()` is declared pure in both bases, so every segment must override it once to satisfy
  both — see `LinkedVyukov.hpp:119`, which forwards to `BaseSegment::capacity()`.
- `enqueue` exists at two arities. Every linked segment writes a forwarding overload whose only job
  is to drop the `info` parameter: `LinkedVyukov.hpp:111-117`, `LinkedSCQ.hpp:82-88`.
- `size()` is in `IQueue` but *not* in `ILinkedSegment`. So `LinkedVyukov` inherits a virtual
  `size()`, while `LinkedPRQ.hpp:120` and `LinkedFAAArray.hpp:98` each declare a **private,
  non-virtual** `size()` of their own. The same expression means different things depending on
  which segment you instantiate, and `LinkedSCQ` has none at all.
- The `info` parameter has a different default in nearly every implementation — `true` in
  `LinkedVyukov.hpp:111` and `LinkedSCQ.hpp:82`, `false` in `LinkedPRQ.hpp:143` and
  `LinkedFAAArray.hpp:123`. A caller who omits the argument gets different behaviour per segment.

### 2.2 `virtual` and `FORCE_INLINE` on the same function

```cpp
// LinkedPRQ.hpp:143
FORCE_INLINE bool enqueue(T item, [[maybe_unused]] bool info = false) noexcept override {
```

The same combination appears at `LinkedVyukov.hpp:111,115`, `LinkedSCQ.hpp:82,86`,
`LinkedFAAArray.hpp:123,136`, `Vyukov.hpp:125,160`. `FORCE_INLINE` expands to
`inline __attribute__((always_inline))` (`specs.hpp:17`).

*What it buys:* nothing that survives. The two annotations pull in opposite directions — a call
through a base pointer cannot be inlined, and `always_inline` on a virtual only applies to the
statically-resolved case.

*How it fails:* devirtualization becomes contingent on `-flto=auto` (`CMakeLists.txt:29`) plus the
`final` keyword, neither of which is applied consistently. For a project whose stated purpose is
*measuring the cost of thread synchronization* ([[MPMC Architecture]]), leaving an unquantified
indirect call in the measured path undermines the measurement.

### 2.3 CRTP layered on top of virtual dispatch

`VyukovBuffer<T,Opt,Derived>` (`Vyukov.hpp:37`) is simultaneously an implementer of the abstract
`IQueue` **and** a CRTP base:

```cpp
// Vyukov.hpp:46-50
using Owner     = std::conditional_t<std::is_void_v<Derived>, HeapOwner<Cell>, NullOwner<Cell>>;
using Effective = std::conditional_t<std::is_void_v<Derived>, VyukovBuffer, Derived>;
static constexpr bool linked_derived = base::is_linked_segment_v<Effective>;
```

and calls back down into the derived type from inside the hot loop:

```cpp
// Vyukov.hpp:131 and :146
if (static_cast<Effective*>(this)->isClosed(t)) return false;
...
(void) static_cast<Effective*>(this)->close();
```

*What it buys:* one algorithm serves both the standalone queue and the linked segment, with the
close-on-full behaviour compiled out when it is not wanted.

*How it fails:* the class now has two distinct extension mechanisms with different rules, and the
`Derived = void` sentinel means the type is *always* instantiated twice — once as a standalone
queue, once as a segment base. Only `Vyukov` and `SCQueue` use the pattern; `LinkedPRQ`,
`LinkedFAAArray` and `LinkedHQ` do not, so there is no standalone PRQ or FAAArray even though the
algorithms would support one.

### 2.4 Template-template segment parameter with fixed arity

Every proxy takes the segment as a template template parameter of exactly four arguments:

```cpp
// UnboundedProxy.hpp:19-28
template <typename T,
          template<typename,typename,typename,typename> typename Seg,
          typename ProxyOpt   = meta::EmptyOptions,
          typename SegmentOpt = meta::EmptyOptions>
class UnboundedProxy : public base::IProxy<T, Seg<T,void,SegmentOpt,void>>
{
    using Segment = Seg<T, UnboundedProxy, SegmentOpt, void>;
```

*What it buys:* the proxy can name itself inside the segment's own parameter list, which is what
makes `friend Proxy` work (see 2.5).

*How it fails:*

- The arity, order and meaning of all four parameters are an unwritten contract. A segment with
  three or five parameters cannot be used at all.
- The base-class list instantiates a **second, different** specialization —
  `Seg<T,void,SegmentOpt,void>` — purely to run the `is_linked_segment_v` static assert in
  `IProxy.hpp:20`. Two full template instantiations of a large class per proxy, one of which is
  never otherwise used.
- The parameter order is easy to get wrong and nothing catches it.
  `BoundedMemProxy.hpp:37` passes `Seg<T,void,SegmentOpt,ProxyOpt>` to `IProxy` — options in the
  wrong slots — while line 42 defines the real one as
  `Seg<T,BoundedMemProxy,SegmentOpt,VersionedIndex>`. The base-class instantiation is a different
  type from the one the proxy actually uses, and compiles anyway.

### 2.5 `friend Proxy` as the encapsulation mechanism

Segments mark their operations `private` and befriend whichever type is passed in the second
template slot:

```cpp
// LinkedPRQ.hpp:29
friend Proxy;
```

Same at `LinkedVyukov.hpp:39`, `LinkedFAAArray.hpp:34`, `LinkedSCQ.hpp:23`, `HQSegment.hpp:28`.

*What it buys:* the README's stated goal — a linked segment cannot be used standalone, because using
one safely requires a reclamation scheme the segment does not own.

*How it fails:* access is granted by **naming the caller**, not by describing what the caller must
be. Two consequences:

- Tests cannot drive a segment without impersonating a proxy. `LinkedSegmentTest.cpp:36` therefore
  defines a `TestProxy` whose entire body exists to re-export the private methods. It is a
  production-shaped type that exists only so the friend declaration matches.
- The proxies reach *through* the interface into a protected data member —
  `UnboundedProxy.hpp:92` does `tail->next.load(...)` and `:117`
  `tail->next.compare_exchange_strong(...)`, where `next` is the protected member declared at
  `ILinkedSegment.hpp:27`. The linked-list invariant is maintained by two classes editing one
  variable, with no accessor in between. `HQSegment` names its member `next_` and exposes
  `getNext()` instead, which is exactly why it no longer composes (see 2.11).

### 2.6 `OptionsPack` and the tag proliferation

`meta::OptionsPack` (`OptionsPack.hpp:16`) is the strongest piece of design in the tree. It is a
variadic type list with `has<Opt>`, `get<Template, Default>`, `add`, `add_if` and `merge_pack`, all
compile-time, and `SCQueue.hpp:33` uses it well — deriving the `LFring` option pack from the SCQ
option pack conditionally.

The problem is not the mechanism, it is the absence of a namespace or a validation step for the
tags. Ten separate option structs exist: `VyukovOption` (`Vyukov.hpp:21`), `LinkedVyukovOption`
(`LinkedVyukov.hpp:13`, which does nothing but re-export the previous one), `LinkedPRQOption`
(`LinkedPRQ.hpp:12`), `LinkedFAAArrayOpt` (`LinkedFAAArray.hpp:11`), `SCQOption` (`SCQueue.hpp:15`),
`LinkedSCQOption` (`LinkedSCQ.hpp:9`, likewise a pure re-export), `LFringOption` (`LFring.hpp:14`),
`BoundedCounterProxyOpt`, `BoundedChunkProxyOpt`, `BoundedMemProxyOpt`, `RecyclerOpt`
(`Recycler.hpp:22`), `CacheOpt` (`Buckets.hpp:264`, in a dead file).

Several declare a tag with the same name and identical meaning — `no_cell_padding` appears in
`VyukovOption`, `SCQOption` and `LFringOption`; `force_pow2` in `VyukovOption` and
`LinkedPRQOption`. Meanwhile `LinkedFAAArrayOpt` inverts the convention with `force_cell_padding`,
so `LinkedFAAArray.hpp:41` defaults to *unpadded* cells while every other segment defaults to
padded.

*How it fails:* `has<QueryOpt>` returns `false` for any type not in the pack — including a
misspelled tag, a tag belonging to a different implementation, or a tag that was renamed. Passing
`OptionsPack<VyukovOption::force_pow2>` to `LinkedPRQ` compiles and silently does nothing.

### 2.7 Hand-rolled co-allocation, seven times

The single most costly pattern. Every segment carries its own copy of:

1. a `static Self* create(size_t)` that computes a header offset, calls `std::aligned_alloc`, and
   placement-news itself;
2. an injected-buffer constructor and a self-allocating constructor;
3. a `const bool owns_buffer` member;
4. a pair of `static void operator delete(void*)` / `operator delete(void*, std::align_val_t)`
   overloads that branch on that flag.

`LinkedVyukov.hpp:57-66,127-153`, `LinkedPRQ.hpp:83-92,255-281`, `LinkedFAAArray.hpp:69-78,158-184`,
`LinkedSCQ.hpp:37-44,98-124`, `HQSegment.hpp:100-126`, `SCQueue.hpp:70-93`, `LFring.hpp:92-101`.
Item 4 alone is 26 identical lines repeated five times.

`include/util/memory/CoAlloc.hpp` was written to own exactly this. It is used for nothing but its
trait: `HeapOwner`/`NullOwner` are referenced only by `Vyukov.hpp:46`, and `CoAlloc<Derived>` itself
is referenced by no one.

*How it fails:* a pattern that must be copied is a pattern that will diverge. Both live bugs below
are direct consequences.

**Bug A — the header-offset mask.** The correct align-up is `(n + a - 1) & ~(a - 1)`. Three files
write `& (~a - 1)` instead:

```cpp
// LinkedPRQ.hpp:85, LinkedFAAArray.hpp:71, LFring.hpp:101
constexpr size_t header_size = (sizeof(Self) + alignof(Cell) - 1) & (~alignof(Cell) - 1);
//                                                                   ^^^^^^^^^^^^^^^^^ wrong
```

`LinkedVyukov.hpp:59` and `CoAlloc.hpp:23` have it right. The wrong form does not align up at all —
it clears bit 0 and bit `log2(a)`. Evaluated as a constant expression for `sizeof(Self) = 384`,
`alignof(Cell) = 128`: the correct form gives **384**, the buggy form gives **382**. The
co-allocated buffer therefore begins *two bytes inside the object header*, and the first cells
overlap the segment's own `head`/`tail` counters.

This is live. `LinkedPRQ` declares `IsCoAllocated` (see Bug B) and so does take its `create()` path,
and `LinkedPRQ` is in the `UnboundedQueueTest` type list at `UnboundedQueueTest.cpp:23`.

**Bug B — two spellings of the opt-in tag.** The detector at `CoAlloc.hpp:50` matches
`typename T::IsCoAllocated`:

| Spelling | Declared in |
| --- | --- |
| `IsCoAllocated` ✅ | `LinkedVyukov.hpp:52`, `LinkedPRQ.hpp:82`, `LinkedSCQ.hpp:27` |
| `isCoAllocated` ❌ | `LinkedFAAArray.hpp:68`, `SCQueue.hpp:67`, `LFring.hpp:92` |

`is_co_allocated_v<LinkedFAAArray<...>>` is therefore `false`, so `UnboundedProxy.hpp:53` and `:107`
take the `new Segment(cap)` branch and `LinkedFAAArray::create()` is dead code. The
single-allocation optimisation that commit `9667614` was written to deliver does not happen for that
segment, and nothing reports it. An opt-in that *looks* type-safe but degrades silently on a typo is
worse than a runtime flag.

### 2.8 Three different mechanisms for "does this segment support X?"

**A `requires`-expression probe**, copy-pasted verbatim into **all four** proxies —
`UnboundedProxy.hpp:35-45`, `BoundedCounterProxy.hpp:31-40`, `BoundedChunkProxy.hpp:33-42`,
`BoundedMemProxy.hpp:58-67`:

```cpp
inline bool dequeueAfterNextLinked(Segment* lhead, T& out) {
    // This is a hack for LinkedSCQ.
    // See SCQ::prepareDequeueAfterNextLinked for details.
    if constexpr (requires(Segment s) { s.prepareDequeueAfterNextLinked(); }) {
        lhead->prepareDequeueAfterNextLinked();
    }
    return lhead->dequeue(out);
}
```

The comment points at documentation that does not exist. What it actually works around: `LinkedSCQ`
is built from two `LFring`s (`SCQueue.hpp:41-42`), and `LFring` uses a *threshold* counter to make
its empty-check cheap. Once a successor segment has been linked, the head segment must be drained
exhaustively before it can be unlinked, so the threshold has to be reset first, or the retry at
`UnboundedProxy.hpp:154` reports empty while items remain. `LinkedSCQ.hpp:58` forwards to
`LFring::reset_threshold` (`LFring.hpp:55`).

**A static boolean.** `ILinkedSegment.hpp:37` declares `static constexpr bool info_required = false`,
overridden to `true` only by `LinkedSCQ.hpp:26`, and read as
`static constexpr bool INFO_REQUIRED = Segment::info_required` by
`BoundedCounterProxy.hpp:25`, `BoundedChunkProxy.hpp:24`, `BoundedMemProxy.hpp:45`.
`UnboundedProxy` does not read it at all.

**`void_t` tag traits.** `is_linked_segment_v` (`ILinkedSegment.hpp:84`), `is_proxy_v`
(`IProxy.hpp:50`), `is_co_allocated_v` (`CoAlloc.hpp:46`).

*How it fails:* three mechanisms, three failure modes. The `requires` probe silently does nothing if
the method is renamed. The static boolean is inherited with a default, so a segment that *should*
set it and forgets simply gets the slow path — or, worse, the livelock the flag exists to prevent
(documented at `BoundedChunkProxy.hpp:235-241`). The `void_t` traits fail as shown in Bug B. None of
the three produces a diagnostic.

### 2.9 Four proxies, one algorithm

`UnboundedProxy` (250 lines), `BoundedCounterProxy` (292), `BoundedChunkProxy` (303),
`BoundedMemProxy` (288). Each independently implements:

- the Michael–Scott traversal for `enqueue` — load tail, check consistency, follow `next` if set,
  try the segment, otherwise allocate-link-swing;
- the traversal for `dequeue` — load head, try, follow `next`, retry, swing, retire;
- ticket acquisition via `get_ticket_()` — identical in all four;
- `size()` as a sum over per-thread `opCounter`s, with `recordEnqueue`/`recordDequeue` helpers —
  identical in all four;
- `acquire()` / `release()` forwarding to `DynamicThreadTicket` — identical in all four;
- a `safeEnqueue_` implementing the `info_required` hint.

The genuine axes of variation are only three:

| Axis | Unbounded | Counter | Chunk | Mem |
| --- | --- | --- | --- | --- |
| Admission | none | `pushed - popped < cap` (`BoundedCounterProxy.hpp:254`) | `tail_idx - head_idx + 1 < k` (`BoundedChunkProxy.hpp:275`) | pool exhaustion |
| Reclamation | `HazardVector` | `HazardVector` | `HazardVector` | `Recycler` |
| Segment handle | `Segment*` | `Segment*` | `Segment*` | `VersionedIndex` |

Even `safeEnqueue_`'s two variants differ only in the handle: `BoundedCounterProxy.hpp:236` caches a
`Segment*` as `lastSeen`, `BoundedChunkProxy.hpp:251` caches a `uint64_t` index. The bodies are
otherwise line-for-line identical.

*How it fails:* a fix to the traversal — or to the memory ordering, which is the part most likely to
need one — must be made and re-reasoned four times. `UnboundedProxy.hpp:125` reads
`tail = hazard_.protect(null, ticket)` where `null` was just consumed by a failed
`compare_exchange_strong` and now holds the winning segment; whether the other three proxies express
this correctly has to be checked one by one, because there is no shared code to check once.

### 2.10 Manual protect/clear pairing

The hazard protocol is manual. `UnboundedProxy::dequeue` has three exits, two of which must clear:

```cpp
// UnboundedProxy.hpp:150      hazard_.clear(ticket); return false;
// UnboundedProxy.hpp:168-170  hazard_.clear(ticket); recordDequeue(ticket); return true;
```

and `enqueue` clears once at `:128`. Correctness depends on every future exit path remembering.

The two reclamation schemes also have incompatible shapes. `HazardVector<T,Meta>` is pointer-based,
threads an explicit `ticket` through `protect`/`clear`/`retire`, and reclaims with `delete`.
`Recycler<T,Capacity,Opt,Meta>` (`Recycler.hpp:29`) is index-based (`decode(idx)` at `:145`),
epoch-protected (`protect_epoch()` at `:153`), takes the ticket implicitly from TLS, and owns a
fixed pool that `reclaim()` draws from. A proxy written against one cannot be moved to the other
without a rewrite — which is precisely why `BoundedMemProxy` is a separate 288-line file rather than
`BoundedCounterProxy` with a different template argument.

### 2.11 Interface drift with no diagnostic

`include/segment/linked/HQSegment.hpp` defines `LinkedHQ` in `namespace queue::segment` and
implements the **previous** `ILinkedSegment` contract:

| `LinkedHQ` has | Current `base::ILinkedSegment` has |
| --- | --- |
| `Next getNext()` (`:199`) | no such method; proxies read `next` directly |
| own `std::atomic<Next> next_` (`:67`) | protected `next` in the base (`:27`) |
| `bool close()` (`:203`) | `void close()` (`:54`) |
| `bool open()` (`:208`) | `void open()` (`:61`) |
| `static constexpr bool optimized_alloc` (`:34`) | replaced by `IsCoAllocated` |

It cannot be instantiated with any current proxy, and is excluded from both
`LinkedSegmentTest.cpp:58-62` and `UnboundedQueueTest.cpp:22-28`. It passes `-fsyntax-only` only
because a class template's member bodies are not checked until instantiation. Nothing in the build
reports that one of the six segments has been dead since commit `413ee19`.

`LinkedFAAArray` is half-dead in the same way: `open()` is `assert(false && "TODO")`
(`LinkedFAAArray.hpp:117`), so the segment cannot be recycled — yet it is in the
`UnboundedQueueTest` list at `:25`. It survives only because `UnboundedProxy` deletes drained
segments rather than reopening them.

### 2.12 Duplicated primitives

- `VersionedIndex` is defined twice: at global scope in `BoundedMemProxy.hpp:18`, and again in
  `util/hazard/Recycler/VersionedIndex.hpp` (which is dead — nothing includes it).
- `VyukovDCAS.hpp` re-implements `is_pow2` (`:36`) and `round_to_next_pow2` (`:40`), both of which
  are in `util/bit.hpp`; re-`#define`s `CACHE_LINE` (`:6`, as does `VyukovNoABA.hpp:9`, both as
  `128` where `specs.hpp:9` says `128ul`); and defines a `CAS2` macro (`:14-28`) that also lives in
  `util/atomic/cas2.hpp`.
- `VyukovDCAS` also hand-rolls the padded cell (`:50-54`) and manual `char __pad_[]` members
  (`:61,63`) that `CACHE_PAD_TYPES` (`specs.hpp:85`) exists to generate.

### 2.13 No registry

`src/sim/benchmark.cpp:215` dispatches on an integer:

```cpp
switch (queue_type) {
    case 0: run_benchmark<queue::VyukovBuffer>(argc, argv); break;
    case 1: run_benchmark<queue::VyukovNoABA>(argc, argv);  break;
    case 2: run_benchmark<queue::VyukovDCAS>(argc, argv);   break;
    case 3: run_benchmark<queue::PSCQueue>(argc, argv);     break;
    case 4: run_benchmark<queue::MutexQueue>(argc, argv);   break;
    default: std::cerr << "Invalid queue type\n"; return 1;
}
```

`Benchmark<Queue>` takes a `template<typename> typename` (`benchmark.cpp:16`) and constructs
`Queue<TestItem>` (`:42`) with a single size argument (`:52`). **No proxy can satisfy that
signature** — all
four take `(cap, maxThreads)` and a four-parameter segment. So the linked queues, which are the
entire point of the project per the README, are not benchmarkable at all. The benchmark measures
only the five standalone buffers.

Tests enumerate types by hand in `::testing::Types<...>`. `BoundedProxyTest.cpp:18-27` has ten
entries of which six are commented out, and those six name `queue::segment::LinkedPRQ`,
`queue::segment::LinkedHQ`, `queue::segment::LinkedCASLoop` — a namespace that has not held those
types since the move, and one type (`LinkedCASLoop`) that no longer exists under any name.

---

## 3. What it costs

| Pattern | Encapsulation | Error-prone | Friction to derive |
| --- | --- | --- | --- |
| Dual abstract base (2.1) | — | `size()` means three things | overload boilerplate per segment |
| virtual + FORCE_INLINE (2.2) | — | measurement contaminated | — |
| CRTP over virtual (2.3) | — | — | only 2 of 6 segments adopt it |
| Fixed-arity `Seg<>` (2.4) | phantom instantiation | **wrong slots compile** (`BoundedMemProxy.hpp:37`) | arity is an unwritten law |
| `friend Proxy` (2.5) | proxies edit `next` directly | linked-list invariant split in two | tests need a fake proxy |
| Option tags (2.6) | — | misspelled tag ignored silently | 12 option structs, 4 redundant |
| Hand-rolled co-alloc (2.7) | — | **Bug A** (buffer/header overlap), **Bug B** (dead `create`) | ~60 lines copied per segment |
| 3 capability mechanisms (2.8) | — | all three fail silently | which one applies is not discoverable |
| 4 proxies, 1 algorithm (2.9) | — | ordering fixes needed 4× | new policy ⇒ new 290-line file |
| Manual protect/clear (2.10) | — | one missed `clear` leaks forever | reclaimers not interchangeable |
| Interface drift (2.11) | — | **`LinkedHQ` dead since `413ee19`** | no diagnostic |
| Stale includes (2.12, §0) | — | 3 dead headers still look live | — |
| No registry (2.13) | — | test lists name types that don't exist | **proxies cannot be benchmarked** |

**Adding one segment today** means: write the segment; copy ~60 lines of allocation boilerplate;
pick a spelling of the co-allocation tag; add it to `UnboundedQueueTest.cpp`'s type list; add it to
`LinkedSegmentTest.cpp`'s type list *and* make sure it works with `TestProxy`; add it to
`BoundedProxyTest.cpp`'s list; add the include to `util/queue_impl.hpp`; and accept that it cannot
be benchmarked. Seven edits, one of which is a silent-failure hazard.

---

## 4. Proposed patterns

The goal for each: make the wrong thing not compile, and make the right thing the path of least
effort.

### 4.1 Contract as concepts; type erasure only at the edge

Replace `IQueue` / `ILinkedSegment` / `IProxy` with C++20 concepts.

```cpp
// include/core/Concepts.hpp
namespace core {

template<typename S, typename T>
concept SegmentLike =
    requires (S s, const S cs, T item, T& out) {
        { s.enqueue(item) } noexcept -> std::same_as<bool>;
        { s.dequeue(out)  } noexcept -> std::same_as<bool>;
        { cs.capacity()   } noexcept -> std::same_as<std::size_t>;
        { cs.size()       } noexcept -> std::same_as<std::size_t>;
    };

template<typename S, typename T>
concept LinkedSegmentLike =
    SegmentLike<S,T> &&
    requires (S s, const S cs, typename S::handle_type h) {
        typename S::handle_type;                                  // Segment* or VersionedIndex
        { s.close()        } noexcept -> std::same_as<void>;
        { cs.is_closed()   } noexcept -> std::same_as<bool>;
        { cs.next()        } noexcept -> std::same_as<typename S::handle_type>;
        { s.link_next(h)   } noexcept -> std::same_as<bool>;      // CAS null -> h
    };

} // namespace core
```

Three things change.

**The `next` pointer gets accessors.** `next()` and `link_next()` enter the contract, so
`UnboundedProxy.hpp:92,117` stop reaching into a protected member. This alone resolves the
`LinkedHQ` drift of 2.11 — `LinkedHQ` already has `getNext()`; it just spelled the contract
differently from the one the proxies assumed.

**`friend Proxy` disappears.** Encapsulation moves from "this named class may touch me" to
"a `LinkedSegmentLike` is not usable without a reclaimer, and the only thing that takes one is a
`LinkedProxy`". The segment's second template parameter goes away entirely, which also kills 2.4:
segments become `Seg<T, Opt>`, and `LinkedProxy` constrains rather than instantiates.

**Diagnostics become useful.** A segment missing `is_closed()` currently produces a page of
overload-resolution noise at the proxy's first use; with a concept it produces one line naming the
missing expression.

The cost is honest: no runtime polymorphism. Pay it back where it is actually wanted — the benchmark
and the registry — with a single type-erased wrapper:

```cpp
// include/core/AnyQueue.hpp
template<typename T>
class AnyQueue {
    struct VTable {
        bool  (*enqueue)(void*, T);
        bool  (*dequeue)(void*, T&);
        std::size_t (*size)(const void*);
        std::size_t (*capacity)(const void*);
        bool  (*acquire)(void*);
        void  (*release)(void*);
        void  (*destroy)(void*) noexcept;
    };
    const VTable* vt_;
    void* obj_;
public:
    template<typename Q, typename... Args> static AnyQueue make(Args&&...);
    bool enqueue(T x)       { return vt_->enqueue(obj_, x); }
    bool dequeue(T& o)      { return vt_->dequeue(obj_, o); }
    // ...
};
```

One indirect call, paid once per operation, only in the benchmark harness — where it is a constant
added to every implementation equally and therefore does not distort comparisons.

### 4.2 Allocation as a policy, owned in one place

Delete the seven hand-rolled copies. `util/memory/CoAlloc.hpp` becomes the only implementation:

```cpp
// include/util/memory/CoAlloc.hpp
namespace mem {

/// The only align-up in the tree. Bug A cannot recur.
constexpr std::size_t align_up(std::size_t n, std::size_t a) noexcept {
    return (n + a - 1) & ~(a - 1);
}

/// A segment declares its trailing array via `cell_type`; everything else is derived.
template<typename Derived>
struct SingleBlock {
    using Cell = typename Derived::cell_type;

    template<typename... Args>
    [[nodiscard]] static Derived* create(std::size_t cells, Args&&... args) {
        static_assert(alignof(Derived) <= CACHE_LINE);
        const std::size_t header = align_up(sizeof(Derived), alignof(Cell));
        const std::size_t bytes  = align_up(header + cells * sizeof(Cell), CACHE_LINE);
        void* raw = std::aligned_alloc(CACHE_LINE, bytes);
        if (!raw) throw std::bad_alloc();
        auto* cells_at = reinterpret_cast<Cell*>(static_cast<std::byte*>(raw) + header);
        return new (raw) Derived(cells, cells_at, std::forward<Args>(args)...);
    }

    static void destroy(Derived* p) noexcept {
        if (!p) return;
        p->~Derived();
        std::free(p);
    }
};

} // namespace mem
```

Consequences:

- **`owns_buffer` and both `operator delete` overloads disappear from every segment.** The flag
  exists only because one class serves two allocation strategies. A linked segment has exactly one:
  proxies always want the single block. The standalone-queue case is served by a thin wrapper that
  owns a `SingleBlock` product, not by a runtime branch inside the segment.
- **Bug A becomes unrepresentable** — there is one `align_up`, and it is correct.
- **Bug B becomes unrepresentable** — there is no opt-in tag to misspell. Deriving from
  `SingleBlock<Self>` *is* the opt-in, and a segment that does not derive from it has no `create()`
  to call, which is a compile error at the call site rather than a silent fallback.
- **Ownership is expressed in the type.** Proxies hold
  `std::unique_ptr<Segment, mem::Destroy<Segment>>` or the equivalent handle, so a segment cannot be
  freed by the wrong path.

Net: roughly 60 lines removed per segment, five times over.

### 4.3 Capabilities as one traits block

Collapse the three mechanisms of 2.8 into one, with **no primary template definition** so that a
missing specialization is a compile error rather than a silent default:

```cpp
// include/core/SegmentTraits.hpp
namespace core {

template<typename S> struct segment_traits;   // deliberately undefined

template<typename T, typename Opt>
struct segment_traits<seg::PRQ<T,Opt>> {
    static constexpr bool needs_close_hint      = false;  // was ILinkedSegment::info_required
    static constexpr bool needs_dequeue_prepare = false;  // was the requires(...) probe
    static constexpr bool recyclable            = true;   // has a working reopen()
    static constexpr bool pointer_payload_only  = true;   // T must be a pointer
};

template<typename T, typename Opt>
struct segment_traits<seg::SCQ<T,Opt>> {
    static constexpr bool needs_close_hint      = true;
    static constexpr bool needs_dequeue_prepare = true;   // LFring threshold reset — see 2.8
    static constexpr bool recyclable            = true;
    static constexpr bool pointer_payload_only  = false;
};

} // namespace core
```

The proxy consults exactly one place:

```cpp
if constexpr (core::segment_traits<Segment>::needs_dequeue_prepare)
    head->prepare_dequeue_after_link();
```

And the reason a capability exists gets written down next to the flag, instead of surviving as
"This is a hack for LinkedSCQ" in four files. `recyclable = false` is how `LinkedFAAArray`'s
unimplemented `open()` becomes visible to the type system rather than to a runtime `assert`.

### 4.4 One proxy, three policies

```cpp
// include/proxy/LinkedProxy.hpp
template<typename T,
         core::LinkedSegmentLike<T> Segment,
         core::AdmissionPolicy     Admit,
         core::Reclaimer           Reclaim>
class LinkedProxy {
    using H = typename Reclaim::handle;      // Segment* | VersionedIndex
    // the Michael-Scott traversal, written exactly once
};
```

The four existing proxies become four aliases:

```cpp
template<typename T, typename Seg>
using Unbounded    = LinkedProxy<T, Seg, admit::None,          reclaim::Hazard<Seg>>;

template<typename T, typename Seg>
using ItemBounded  = LinkedProxy<T, Seg, admit::ItemCount,     reclaim::Hazard<Seg>>;

template<typename T, typename Seg, std::size_t K>
using ChunkBounded = LinkedProxy<T, Seg, admit::SegmentCount<K>, reclaim::Hazard<Seg>>;

template<typename T, typename Seg, std::size_t K>
using MemBounded   = LinkedProxy<T, Seg, admit::PoolExhaustion, reclaim::Epoch<Seg,K>>;
```

An admission policy is small — the entire variation between `BoundedCounterProxy` and
`BoundedChunkProxy` is `capacity_respected_`:

```cpp
struct ItemCount {
    std::atomic<uint64_t> pushed{0}, popped{0};
    bool may_admit() const noexcept {
        return (pushed.load(relaxed) - popped.load(acquire)) < cap_;
    }
    void on_enqueue() noexcept { pushed.fetch_add(1, release); }
    void on_dequeue() noexcept { popped.fetch_add(1, release); }
};
```

`safeEnqueue_`'s two variants (2.9) collapse into the handle abstraction: `lastSeen` becomes
`typename Reclaim::handle`, which is `Segment*` for the hazard reclaimer and a `uint64_t` index for
the epoch one, and the body is written once.

**This is where the payoff concentrates.** ~1100 lines across four files become one traversal plus
four policy structs of 20–40 lines each. A memory-ordering fix is made once.

### 4.5 A `Reclaimer` concept with an RAII guard

```cpp
template<typename R, typename S>
concept Reclaimer =
    requires (R r, typename R::handle h) {
        typename R::handle;                                   // Segment* | VersionedIndex
        typename R::guard;                                    // RAII; clears on scope exit
        { r.pin()               } -> std::same_as<typename R::guard>;
        { r.deref(h)            } -> std::same_as<S*>;
        { r.protect(h)          } -> std::same_as<typename R::handle>;
        { r.retire(h)           } -> std::same_as<void>;
        { r.acquire_segment()   } -> std::same_as<std::optional<typename R::handle>>;
        { r.max_threads()       } -> std::same_as<std::size_t>;
    };
```

Both `HazardVector` and `Recycler` can satisfy this — the existing operations map over almost
directly; what changes is that the ticket becomes reclaimer state rather than a parameter threaded
through every call, and `pin()` returns a guard whose destructor does what `clear(ticket)` does
today. The three-exit `dequeue` of 2.10 stops depending on every future exit path remembering to
clear.

`acquire_segment()` is what unifies allocation across the two schemes: for the hazard reclaimer it
allocates; for the epoch reclaimer it draws from the fixed pool and may return `nullopt`, which is
exactly the "pool exhausted" signal `admit::PoolExhaustion` needs. That is the last thing forcing
`BoundedMemProxy` to be a separate file.

### 4.6 One registry, consumed by tests and benchmark alike

```cpp
// include/registry/Registry.hpp
namespace registry {

template<typename T>
using All = meta::TypeList<
    Entry<"vyukov",     queue::Vyukov<T>>,
    Entry<"scq",        queue::SCQ<T>>,
    Entry<"lprq",       Unbounded<T, seg::PRQ<T>>>,
    Entry<"lscq",       Unbounded<T, seg::SCQ<T>>>,
    Entry<"lprq-bmem",  MemBounded<T, seg::PRQ<T>, 4>>
    // one line per implementation
>;

} // namespace registry
```

Consumed two ways:

```cpp
// src/test/unit/QueueTest.cpp
using QueueTypes = registry::AsGtestTypes<TestItem*>;      // replaces every hand-written list

// src/sim/benchmark.cpp
auto q = registry::make<TestItem*>(argv[1], capacity, threads);   // AnyQueue<TestItem*>
if (!q) { std::cerr << "unknown queue: " << argv[1] << "\n"; return 1; }
```

This is what makes proxies benchmarkable for the first time (2.13): `AnyQueue` erases the
`(cap, maxThreads)` vs `(cap)` difference that `Benchmark<template<typename> typename Queue>`
currently cannot express. It also makes the benchmark's CLI self-documenting — a name instead of
`case 3:`.

### 4.7 Namespaces and directories

```
include/
  core/        Concepts.hpp, SegmentTraits.hpp, AnyQueue.hpp     namespace core
  cell/        SequencedCell, PlainCell                          namespace cell
  queue/       Vyukov, SCQ, LFring, Mutex, ...                   namespace queue
  segment/     PRQ, SCQ, FAAArray, HQ, Vyukov                    namespace seg
  proxy/       LinkedProxy, admission/, reclaim/                 namespace proxy
  registry/    Registry.hpp                                      namespace registry
  util/        bit, specs, memory/, threading/, timing/          namespace util
```

Rules: one directory ↔ one namespace; nothing at global scope; option tags live in the namespace of
the thing they configure (`seg::PRQOpt`, `proxy::MemOpt`), so `has<>` cannot silently accept a tag
from elsewhere. `include/linked/` and `include/segment/linked/` merge — proxies are `proxy/`,
segments are `segment/`, and "linked" stops being a directory name that means two things.

---

## 5. Worked example — deriving a new segment

The acceptance test for the proposal. Under the current scheme this file would carry ~60 lines of
allocation code, a `friend`, four `override`s, two `operator delete`s and a co-allocation tag.
Under the proposal:

```cpp
// include/segment/Ring.hpp
#pragma once
#include <core/SegmentTraits.hpp>
#include <util/memory/CoAlloc.hpp>
#include <cell/SequencedCell.hpp>

namespace seg {

struct RingOpt { struct no_cell_padding{}; };

template<typename T, typename Opt = meta::EmptyOptions>
class Ring : public mem::SingleBlock<Ring<T,Opt>> {
public:
    using cell_type   = cell::SequencedCell<T, !Opt::template has<RingOpt::no_cell_padding>>;
    using handle_type = Ring*;

    // Called only by SingleBlock::create. Cells are already carved out of our own block.
    Ring(std::size_t n, cell_type* cells) noexcept : cap_{n}, cells_{cells} {
        for (std::size_t i = 0; i < n; ++i) {
            cells_[i].val.store(T{}, std::memory_order_relaxed);
            cells_[i].seq.store(i,   std::memory_order_relaxed);
        }
    }

    bool enqueue(T item) noexcept { /* algorithm */ }
    bool dequeue(T& out) noexcept { /* algorithm */ }

    std::size_t size()     const noexcept { /* ... */ }
    std::size_t capacity() const noexcept { return cap_; }

    void close()            noexcept { tail_.fetch_or(bit::set_msb(0ull), release); }
    bool is_closed()  const noexcept { return bit::get_msb(tail_.load(acquire)); }
    handle_type next() const noexcept { return next_.load(acquire); }
    bool link_next(handle_type h) noexcept {
        handle_type expect = nullptr;
        return next_.compare_exchange_strong(expect, h);
    }

private:
    ALIGNED_CACHE std::atomic<uint64_t> head_{0};
    ALIGNED_CACHE std::atomic<uint64_t> tail_{0};
    ALIGNED_CACHE std::atomic<handle_type> next_{nullptr};
    const std::size_t cap_;
    cell_type* const  cells_;
};

} // namespace seg

// The capability declaration. Omitting it is a compile error, not a silent default.
template<typename T, typename Opt>
struct core::segment_traits<seg::Ring<T,Opt>> {
    static constexpr bool needs_close_hint      = false;
    static constexpr bool needs_dequeue_prepare = false;
    static constexpr bool recyclable            = false;   // no reopen() yet
    static constexpr bool pointer_payload_only  = false;
};
```

No allocation code. No `friend`. No `override`. No `operator delete`. No co-allocation tag. The
class is the algorithm plus the linked-list accessors, and the traits block states in five lines
what the proxy needs to know.

Registering it — the only other edit:

```cpp
// include/registry/Registry.hpp
Entry<"ring",      Unbounded<T, seg::Ring<T>>>,
Entry<"ring-bmem", MemBounded<T, seg::Ring<T>, 4>>,
```

Those two lines put the segment into every typed test suite and make it selectable by name in the
benchmark. **Two files touched instead of seven, with no copied boilerplate and no silent-failure
hazard.**

---

## 6. Migration path

Each step leaves the tree compiling and the tests runnable.

1. **Unbreak the build.** Fix `Recycler.hpp:81,326` (`LFringSlab` → the real `LFring` API, and the
   missing `buckets_` member) so `BoundedProxyTest` compiles and `BoundedMemProxy` is under test
   again. Delete the three orphaned headers (`Buckets.hpp`, `EpochCell.hpp`, `VersionedIndex.hpp`)
   and `SegmentRecyclerTest.cpp.errored_out`, or repair their include paths if any is wanted.
2. **Fix the two live bugs in place.** The header-offset mask in `LinkedPRQ.hpp:85`,
   `LinkedFAAArray.hpp:71`, `LFring.hpp:101`; the tag spelling in `LinkedFAAArray.hpp:68`,
   `SCQueue.hpp:67`, `LFring.hpp:92`. Both disappear permanently at step 4, but they are corrupting
   memory today and the fix is three characters each. Add a test that asserts the co-allocated
   buffer address is `>= (char*)self + sizeof(Self)` — this is the regression guard, and it will
   fail before the fix.
3. **Introduce `core/` alongside the existing bases.** Concepts and `segment_traits`, with
   `static_assert(SegmentLike<...>)` added to the existing segments. No behaviour changes; the
   assertions document which segments already conform and which do not. Expect `LinkedHQ` to fail
   loudly here for the first time.
4. **Move allocation into `mem::SingleBlock`,** one segment at a time, deleting the local
   `create`/`owns_buffer`/`operator delete` as each is ported. `LinkedVyukov` first — it is the one
   whose mask is already correct, so its behaviour should not change at all, which makes it the
   control.
5. **Collapse the four proxies onto `LinkedProxy`,** starting with `UnboundedProxy` +
   `admit::None` + `reclaim::Hazard`, verified against `UnboundedQueueTest` before the other three
   are folded in.
6. **Registry, then rewire** `benchmark.cpp` and the four test type-lists onto it. Proxies become
   benchmarkable at this step.
7. **Retire the stragglers.** Port `LinkedHQ` to the current contract and namespace, or delete it.
   Implement `LinkedFAAArray::open()` — the TODO at `:117-120` sketches the approach: alternating
   `EMPTY`/`SEEN` encodings keyed off a generation bit in `head`/`tail`, so a drained segment resets
   without rewriting every cell. Until then its `recyclable` trait stays `false`, which is now
   enforced rather than asserted at runtime.

Steps 1 and 2 are worth doing regardless of whether the rest is adopted.

---

## 7. Open decisions

Three calls to make before step 3. Each has a recommendation and the reason it might go the other
way.

**Dispatch: concepts, or keep the vtable?**
*Recommendation: concepts, with `AnyQueue` at the benchmark boundary (4.1).* It removes the
dual-base overlap, the `Seg<T,void,...>` phantom instantiation, the `friend`-based access control
and the indirect call in the measured path, all at once. *Against:* it is the largest single change
in the plan, every segment's signature moves, and if runtime selection turns out to be wanted
somewhere other than the benchmark, the type-erasure wrapper has to grow.

**Proxies: unify onto `LinkedProxy`, or share a base?**
*Recommendation: unify (4.4).* The three axes of variation are already identified and small, and
the duplication is where memory-ordering bugs will hide. *Against:* a policy-parameterized proxy is
harder to read in isolation than a 250-line file you can follow top to bottom — which matters if
these are ever to be presented or published as reference implementations. A middle path exists:
unify `Unbounded`/`Counter`/`Chunk` (which differ only in admission) and leave `Mem` separate until
the `Reclaimer` concept of 4.5 has proven itself.

**Migration: in place, or a parallel `core/` tree?**
*Recommendation: parallel, as written in §6* — new headers alongside old, port one implementation at
a time, delete each old header when its replacement passes the same tests. The tree stays green
throughout and each port is independently reviewable. *Against:* both trees exist at once for the
duration, and `queue_impl.hpp` (which includes everything) will need to include both until the last
port lands.

---

## Appendix — how the claims were checked

```bash
# every header, standalone
for h in $(find include -name '*.hpp' -o -name '*.h'); do
  printf "%-55s " "$h"
  g++ -std=c++20 -fsyntax-only -Iinclude -x c++ "$h" 2>/dev/null && echo OK || echo FAIL
done

# every unit test, plus the benchmark
for t in src/test/unit/*.cpp; do
  printf "%-45s " "$t"
  g++ -std=c++20 -fsyntax-only -Iinclude \
      -Icmake/extern/googletest/googletest/include "$t" 2>/dev/null && echo OK || echo FAIL
done
g++ -std=c++20 -fsyntax-only -Iinclude -DCORE_TOPOLOGY='"/tmp/x"' src/sim/benchmark.cpp

# Bug A — the two forms of align-up
grep -rn "alignof(Cell) - 1" include/

# Bug B — the two spellings of the co-allocation tag
grep -rn "IsCoAllocated\|isCoAllocated" include/

# orphaned headers: pre-move flat includes, and who includes them
grep -rn '#include <\(specs\|bit\|OptionsPack\|SequencedCell\)\.hpp>' include/
for h in Buckets EpochCell VersionedIndex; do
  printf "%-16s included by: " "$h"; grep -rl "Recycler/$h.hpp" include/ src/; echo
done
```

Bug A's arithmetic, as a standalone check:

```cpp
constexpr size_t bad (size_t n, size_t a) { return (n + a - 1) & (~a - 1); }
constexpr size_t good(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }
static_assert(good(384, 128) == 384);
static_assert(bad (384, 128) == 382);   // header is 384 -> buffer starts inside the object
```

Expected results: 34 of 40 headers OK; 6 of 7 tests and the benchmark OK, `BoundedProxyTest` failing
on `LFringSlab`; `Recycler.hpp`, `BoundedMemProxy.hpp`, `queue_impl.hpp` failing, plus the three
orphans (note `grep` for `IsCoAllocated` reports four files — three declaring sites plus the
detector in `CoAlloc.hpp`); three files
with the buggy mask against two with the correct one; three `IsCoAllocated` against three
`isCoAllocated`; and no includer for any of `Buckets.hpp`, `EpochCell.hpp`, `VersionedIndex.hpp`.
