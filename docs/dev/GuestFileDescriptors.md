# Guest file descriptors

Guest file descriptors belong to the table in `libc`'s
`GuestFileDescriptors.cpp`. They are independent of native descriptors used by
Vulkan, telemetry and other host code. Guest standard descriptors own duplicates
of the host standard streams, so closing guest stdin does not close host stdin.
Closing a guest descriptor removes its table entry; outstanding leases keep the
native resource alive until their operations complete. Replacement descriptors
have a new identity, preventing stale streams from accessing a reused entry.

## API hooks

`libkernel/File/include/FileDescriptorHooks.def` lists the file API boundaries.
`DescriptorHooks.cmake` compiles upstream implementations under private
`_native_nid_no_patch` names. `FileDescriptorHooks.cpp` exports the original guest
entry points, acquires a lease, translates the descriptor and calls the native
implementation. Calls between upstream implementations remain native and do not
translate twice. Private functions are excluded from NID patching.

The upstream bodies in `Open.cpp`, `Stdio.cpp`, `Terminal.cpp`,
`SharedMemory.cpp` and `Socket.cpp` stay unchanged. Creation, closing and
duplication use explicit hooks to transfer ownership. `GuestSelect.cpp` holds
leases for all selected files while retaining the existing virtual socket
namespace. AIO retains a lease for each native read/write. Small hooks also cover
WebKit `isatty`, kernel-sys `fchdir` and directory descriptor lookup.

The libc stream implementation shares the descriptor table. `fopen`, `fdopen`,
`freopen`, formatting and standard streams must use this ownership layer; raw
native `FILE*` handles cannot represent guest stream lifetime and descriptor
reuse safely.

## Integration gaps

The descriptor table and leases remain necessary: upstream file APIs still use
native descriptor numbers, and upstream streams still expose native ownership.
Keep those operation bodies behind the guest boundary rather than replacing the
ownership layer with them.

The POSIX `fcntl_nid_postfix` file branch in `Socket/src/Udp.cpp` currently calls
`File::Fcntl` with the guest number without translation. It can modify a host-only
file descriptor. The kernel `sceKernelFcntl` hook also only forwards the argument
for command 4; the new integer, pointer and duplication commands need explicit
translation and ownership handling. The `guest_dup` shared-status-flags check
exposes the POSIX translation gap.

The standard-stream mode fixture still passes a native `dup(fileno(...))` result
to guest `fdopen`; adapt that fixture to use a guest-owned descriptor. Host-only
descriptors must continue to be rejected.

## Upstream updates and verification

Add changed or new file entry points to the manifest. CMake rejects unlisted
definitions in the four hooked file sources whose first parameter is an `int`
or `std::int32_t` named `d`, `fd`, `descriptor` or `from`. Changes to those sources
trigger this check again. New entry points with other signatures and calls in
other libraries still require review for native descriptor access.

Build the patched runtime with `cmake --build build --target relinker libs`
while no game is running. Tests cover host isolation, stream identity, descriptor
ownership, concurrent close/read, pipe rollback, select, duplication, AIO and
POSIX/kernel errors. `guest_patched_file_runtime` additionally checks the actual
NID-patched shared libraries used by games. Run the relevant checks with:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(guest_file_.*|guest_standard_streams|file_streams|guest_stdio_errno|guest_select|guest_dup|guest_aio|guest_posix_.*|guest_kernel_sys|guest_terminal|guest_patched_file_runtime)$'
```

Use FrameRift to launch Bomberman for the native smoke check. Menu rendering and
frame delivery verify the descriptor regression; arena performance is a separate
measurement.
