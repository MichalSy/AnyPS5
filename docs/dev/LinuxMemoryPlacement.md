# Linux guest memory placement

Automatic kernel mappings with a null address use the virtual range
`0x200000000` to `0xfc00000000`, with the requested alignment. The existing
Windows guest arena's system-reserved interval
`[0x7ffffc000, 0x1000000000)` is excluded from automatic Linux placement too,
without reserving host memory for the interval. This applies to
direct memory, flexible memory and virtual reservations because these APIs share
`MapPlaced` in `libkernel/DirectMemory/DirectMemory.cpp`. Existing mappings are
preserved with `MAP_FIXED_NOREPLACE`; an exhausted range or unsupported placement
fails instead of returning a host address outside this range. Explicit fixed
addresses and non-null hints keep their existing placement behavior.

The lower placement range also matches the bounds used by the existing Windows
guest arena. It is a compatible subset, rather than a complete model of all PS5
virtual regions.

The bundled PS5 libc in Quake (`PPSA01880`) provides direct evidence for the
address restriction. Its `sceLibcMspaceCreate` export at RVA `0xf9a0` calls the
allocator initializer at RVA `0xd300`. With flags zero, the initializer permits
a lower range beginning at `0x400000` and ending no higher than `0xfc00000000`,
or an upper range beginning at `0x80000000000` and ending no higher than
`0xf0000000000`. These are checks in this supplied library, not measurements of
the console kernel's allocation policy.

A 2 GiB direct mapping at `0x7fff73200000` succeeds on the Linux host but is
rejected by the guest allocator at RVA `0xd3e0`, returning null. Quake then
returns 1 from its heap initializer and raises debug exception `0xa0020013`;
subsequent allocation during library initialization terminates with
`std::bad_alloc`. Automatic placement in the lower range addresses this concrete
failure without replacing the guest allocator or altering the supplied game.

`guest_memory_tests` checks automatic bounds, alignment, occupied host mappings,
direct-memory aliases, oversized requests, alignment outside the usable range
and preservation of explicit high address hints. A 32 GiB uncommitted virtual
reservation checks that an allocation which cannot fit before the reserved
interval skips to its far side without crossing the interval or committing RAM.
