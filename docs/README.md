\page docsindex Documentation index

Start with the [README](../README.md) at the repository root — it is the architecture overview
and the front page of the generated documentation.

## Guides

| | |
| --- | --- |
| \ref extending "Extending.md" | how to add an algorithm, a policy, a source, or an object with co-allocated arrays. Read the relevant section plus the checklist at the end |
| \ref testing "Testing.md" | the suites, the four build configurations, and the technique used to find the concurrency bugs |

## Generated API documentation

```bash
cmake -S . -B build
cmake --build build --target docs      # -> build/docs/html/index.html
```

Needs `doxygen`; `graphviz` additionally enables the collaboration and include graphs, which are
the only place the assembled template stack is visible in one picture. The target is skipped
with a status message if Doxygen is absent, so it never blocks a build.

### The warning bar

`WARN_IF_UNDOCUMENTED` is **on**, so an entity with no documentation at all is reported. This is
deliberate — `EXTRACT_ALL` is off for the same reason, since it would turn every gap into a
confident-looking empty stub.

`WARN_NO_PARAMDOC` is **off**, and that is also deliberate rather than a concession: it demands a
`@param` for every argument of every documented function, so `@param item the item` on a dozen
`enqueue(T item)` overloads, and it makes adding a `@brief` without a full parameter list produce
*more* warnings than adding nothing. Parameters that carry non-obvious meaning — `closed_hint`,
`init_full`, `worth_waiting` — are documented where they occur.

The build currently reports **≈241 warnings**, down from 507 when the Doxyfile was first added.
What remains is a thin tail of individually-undocumented members spread across roughly twenty
files, concentrated in `proxy/Admission.hpp`, `util/threading/`, `registry/` and `mem/Handle.hpp`.
None is a missing *explanation* — the class-level prose covers the reasoning in every case — they
are members whose own one-line brief has not been written yet. Treat the number as a budget to
drive down, and do not let it grow.

## Notes

Design background and the current backlog. These describe the tree as it is.

| | |
| --- | --- |
| `notes/As Shipped.md` | the architecture that actually exists, and why each piece is shaped that way |
| `notes/Assessment.md` | code review, known defects, and the optimisation backlog — the live to-do list |
| `notes/Hazard.md` | hazard pointers from first principles |
| `notes/Epoch-Based Segment Recycler.md` | why the pooled source reclaims by epoch rather than by hazard pointer |
| `notes/Recycled Queue.md` | the linked-segment queue concept: fast path, slow path, closing |
| `MPMC Architecture.md` | the short project statement |

## Legacy

`legacy/` holds superseded designs, each with a header saying what replaced it. Nothing there
describes the current tree — it is kept because the reasoning that led somewhere is often worth
more than the destination. It is excluded from the Doxygen build.

Notably `Queue Interfaces.md` and `Queue Storage Policies.md` document the `IQueue<T>` /
`IStoragePolicy` virtual interfaces that the refactor removed; contracts are now C++20 concepts
in `include/core/`, and storage is single-block co-allocation.
