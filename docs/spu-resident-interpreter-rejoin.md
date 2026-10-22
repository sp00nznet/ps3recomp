# Resident code and interpreter rejoining

The interpreter must use the runtime ownership order already used by indirect
SPU dispatch: bounded resident code, task, overlay, primary image. A bounded
resident-code interval is authoritative even if no lifted function exists at the
requested address. Such a miss must continue interpreting live local-store
bytes rather than execute a stale primary-image lift at the same address.
Task and unbounded-overlay lookup misses retain their existing fallback order.

`spu_lifted_lookup` now owns this shared lookup, and indirect dispatch delegates
to it after its existing task identity selection. This change does not register
images, infer ownership from bytes, alter loads or invalidation, change image
identities, or enable any title-specific lift. NULL contexts retain wildcard
lookup compatibility. Region end is exclusive, using subtraction after checking
the lower bound to avoid addition overflow.

The production thread-start fixture adds overlapping-image resolution and real
interpreter/dispatcher execution. It covers an authoritative region hole, an
exclusive end, task/overlay holes, continuation DMA, conflicting DMA invalidation,
and clearing residency. The unchanged parent fails eleven of these checks.
Build and run `runtime/spu/tests/test_spu_lifted_start.c` using the full command
in its header; the candidate has 112 checks. Both optimized and undefined-behavior
sanitized builds pass. This is a resolver correctness regression; runtime benefit
requires separately validated module registration and routing evidence.
