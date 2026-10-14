# FrameRift integration

The runtime follows upstream. The local adapter adds shared-runtime launching and
an optional UDP connection to FrameRift; it does not change shader compilation,
resource tracking, rendering, or GPU synchronization.

Build `relinker` and `libs` with `ANYPS5_ENABLE_DEBUG_BRIDGE=ON`. FrameRift starts
games through `tools/run_game.py` and `tools/run_with_memory_guard.py` using
explicit original-game, generated-code, and writable working directories.
`ANYPS5_GAME_ROOT` maps `/app0` to original files; `ANYPS5_CODE_ROOT` resolves
converted modules separately. Saves remain beneath FrameRift's shared data root.

The bridge supports session identification, heartbeats, monitoring commands, and
completed main VideoOut frame samples. Shader/resource telemetry and GPU work
profiling are not part of this adapter. Runtime behavior without configured
launch paths and with the bridge disabled follows upstream.

Checks: `debug_bridge`, `guest_working_directory`, `guest_shared_runtime`,
`video_out_flip`, and `run_game`.
