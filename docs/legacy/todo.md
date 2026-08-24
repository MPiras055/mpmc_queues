> **Archived.** An old task list; every item is either done or superseded. The live backlog is docs/notes/Assessment.md.
>
> Kept for the design history; nothing here describes the current tree.

## implementations
- [x] add compliant c++11 interface for lfring
- [x] add compliant c++11 interface for scqueue (w indirection)
- [ ] add compliant c++11 interface for scqueue (direct storing CAS2 vs software-emulation)

> Remember to add direct struct storing ::create() method for scqueue with indirection as well
as lfring(size_t) 

## utilities
- [ ] rework hazard pointers
- [ ] rework unbounded proxy 

## test
- [ ] test difference in standard c lfring/scqueue and c++ versions
