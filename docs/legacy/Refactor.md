> **Archived.** A working note requesting the extension guide and the Hazard/Pool/thread-ticket rework. All of it is done; the guide it asked for is docs/Extending.md.
>
> Kept for the design history; nothing here describes the current tree.

I need you to better document the project, I need a standard example way on how to add a new thing of everything:
- adding a new algorithm
- adding new policies
- reasoning with block constructions
- adding a new source

Moreover I need you to rework teh Hazard Pointers and the Pool, as of right now the pool doesnt work and I need to fix it (I will fix it). I also want to pack a variant metadata instead of padding of hazard pointers or the pool thread local storage metadata. I want to standardize this, so having custom metadata embedded as the std::vector in the linked proxy if no other selected but if the source provides a way to embed the metadata then use that, if not default to the std::vector.


I also want you to rework the ThreadTicket both static and dynamic, honestly I woudn't want to provide an acquire and release guard, I'd prefer a RAII guard which internally calls acquire or release. Thinking about it we could also have each thread publish its single writer location as linking it to a public linked list atomicly, each thread would keep access to its single writer location while keeping a pointer to the previous one, so not needing to have a double linked list. In this way we have truly dinamic parallelism. We also need a way for the thread to cleanup, which we could get with a raii guard. The only one which needs it is the linked proxy, i'd prefer to have the less changes in API between the linked proxy and the bounded algorithms as possible. This would be a basic building block for almost to all memory reclamation systems we could decide to implement.