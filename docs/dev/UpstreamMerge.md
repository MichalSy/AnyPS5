# Upstream integration, 2026-10-13

Upstream `0f2683cb7` is integrated into the previously published `51c55dcf5`.
The pre-merge runtime including temporary Sackboy diagnostics is preserved on
`codex/pre-upstream-sackboy-snapshot` (`13121dfbb`). Temporary default frame dumps
and file-based pad input are not enabled on main.

## Replaced by upstream

- The queue-idle publication policy uses upstream's 50 ms quiet interval.
- Texture snapshots use upstream's layer validity and import-watch handling;
  the old scalar validity/maybe-written comparison optimization is removed.
  Sampled snapshots retain upstream's CPU comparison bytes; discarding them made
  empty byte comparisons incorrectly reuse stale GPU pixels.
- CMASK unknown-code handling follows upstream's expanded interpretation.
  Temporary invalid-code diagnostics are removed.
- Recursive ELF discovery uses upstream's signature checks and converter.
- Param JSON parsing uses upstream's complete metadata parser, which already
  includes user-defined parameters and download size.
- Vertex layer/clip/cull exports and pipeline-library interfaces follow upstream.
- Centered multisample color rasterization uses upstream behavior; the earlier
  attachment precheck and the sample-location accessor understand that path.

## Retained extensions

- Guest descriptor ownership, leases, stream identities and host isolation remain
  necessary. New low-numbered sockets share the logical descriptor allocator;
  fcntl, ioctl, close, select and terminal calls preserve guest boundaries.
- Read-only pass-through resummarize detection extends upstream's no-work case.
- POSIX thread masks and protection of pre-existing host signal handlers remain;
  upstream pending waits and timers are retained.
- FrameRift telemetry, explicit launcher paths and recursive module discovery.
- PlayGo's finite fallback chunk set and host physical-memory reporting in guest
  page units.
- Peer socket type mappings and the observed PS5 broadcast option.
- Canonical depth/stencil aliases, mip/sample support and two-dimensional array
  color slices are retained where upstream has narrower support. Upstream depth
  plane array/cube sampling and host D16/stencil format fallback are integrated.
- Queued CMASK/metadata ordering, mapping retirement and conditional descriptor
  validation remain in place. Zero-stride vertex bounds handling is retained.

A successful merge build does not establish that Sackboy's previously reported
neon textures, black surfaces or menu rendering are fixed. Game validation must
run through FrameRift with the rebuilt runtime.

## Additional PR integrations

- Pixel shaders load the perspective/linear per-sample interpolation pairs
  (PR #4797); tessellation control-point components use their within-slot offset
  (PR #4723). Existing triangle, quad and data-indexed tessellation stay intact.
- At flips, pending host-imported storage images up to 64 KiB publish their GPU
  results for CPU consumers such as exposure calculation (PR #4841). Larger
  resident targets retain their existing idle/read synchronization policy.
- Static compute/fragment ABI resolution runs on the background preparation pool
  (PR #2589). Draws await pending jobs; failed speculative preparations fall back
  to preparation at use.
- Pipelined release packets do not interrupt for INT_SEL 3; immediate DMA fills
  notify both HTILE and CMASK consumers in packet order (PRs #1201/#1202).
- Registered linear color targets use the resident-image path (PR #1193).
  Fragment descriptor snapshots reuse sets only while their layouts remain
  cached; owned layouts retain their explicit free path (PR #1202).
- Windows write-watch queries size the output capacity to the scanned region
  (PR #4875). Positional file reads return guest errors on failure (PR #4719);
  public descriptor hooks continue to preserve host/guest ownership.
- Host standard-stream isolation already covers the goal of PR #3295. The
  rejected APR retry/staging proposal #4413 is not included.
- The GT7 report's packed 11:11:10 workaround disables blending and is not a
  submitted PR. It is not part of these integrations; Sackboy's green tint needs
  separate validation instead of assuming the same cause.

## Validation

The full configured build succeeds with `ANYPS5_ENABLE_DEBUG_BRIDGE=ON`.
Targeted Linux regressions pass, including the complete graphics and submission
suites, per-sample interpolation, tessellation with 24-byte control points,
background/synchronous ABI preparation, small image publication at flips,
actual NID-patched libraries, descriptor isolation, canonical aliases, queued
CMASK, depth plane sampling/storage and module start arguments.

The complete `agc_driver_depth_mips` and
`agc_driver_depth_surface_last_writer` suites pass. Native MSAA depth sampling
uses matching image views; mismatched sample counts/extents remain rejected.
Plain depth arrays use the existing array storage path, while unsupported
combined depth/stencil array views retain upstream validation. Tests follow
upstream plane-cache ownership and byte-based snapshot validation, and check
GPU pixels, pending writes and plane release on cache teardown.

An additional general `guest_memory` check fails its application-area boundary
assertion in `CheckFixedMappingsReachTheApplicationAreaEnd`; this is outside these graphics
changes. Complete memory/rendering correctness and Sackboy rendering remain
unverified.
Linux was validated; a native Windows build and game smoke tests were not run.
