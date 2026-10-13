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

The upstream file operation bodies stay behind these hooks. Creation, closing and
duplication transfer ownership explicitly. Files, POSIX sockets and libSceNet
socket handles allocate logical numbers from the same guest table; socket
registries retain their transport objects. `GuestSelect.cpp` holds file leases
and polls both socket registries. AIO retains leases for native read/write.
WebKit `isatty`, kernel-sys `fchdir` and directory lookup also translate handles.
A native descriptor scope prevents a translated host number from being mistaken
for an unrelated logical socket with the same integer value.

The libc stream implementation shares the descriptor table. `fopen`, `fdopen`,
`freopen`, formatting and standard streams must use this ownership layer; raw
native `FILE*` handles cannot represent guest stream lifetime and descriptor
reuse safely.

## Integration gaps

The descriptor table and leases remain necessary: upstream file APIs still use
native descriptor numbers, and upstream streams still expose native ownership.
Keep those operation bodies behind the guest boundary rather than replacing the
ownership layer with them.

POSIX and kernel `fcntl` share a guest wrapper with typed arguments and logical
duplication. Socket `ioctl` translates file handles through leases. Guest close
dispatches libSceNet and POSIX sockets before files and notifies equeues using the
logical descriptor. These boundaries remain necessary with the new upstream
socket implementation.

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
NID-patched shared libraries used by games, including LibcInternal eager loading
and its getopt warning path. Cross-library stream access uses the C helpers
`GuestFileStreamNative_nid_no_patch` and `GuestFileStreamSyncStatus_nid_no_patch`;
internal C++ method names must not become unresolved imports after NID patching.
Run the relevant checks with:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(guest_file_.*|guest_standard_streams|file_streams|guest_stdio_errno|guest_select|guest_dup|guest_aio|guest_posix_.*|guest_kernel_sys|guest_terminal|guest_patched_file_runtime)$'
```

Use FrameRift to launch Bomberman for the native smoke check. Menu rendering and
frame delivery verify the descriptor regression; arena performance is a separate
measurement.
