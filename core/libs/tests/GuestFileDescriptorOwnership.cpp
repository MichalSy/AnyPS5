#include "prx/libc/include/GuestFileDescriptors.hpp"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace {
std::atomic<int> cleanupCount{0};
std::atomic<int> readableAtCleanup{0};

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Cleanup(int descriptor) noexcept {
    cleanupCount.fetch_add(1);
    if (::fcntl(descriptor, F_GETFD) != -1) readableAtCleanup.fetch_add(1);
    const auto standard = GuestFiles::Acquire_nid_no_patch(1);
    errno = 47;
}

int OwnedDescriptor() {
    const int descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    Require(descriptor >= 0, "host descriptor creation failed");
    return descriptor;
}

bool Closed(int descriptor) {
    errno = 0;
    return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

void CheckRollback() {
    int descriptor = OwnedDescriptor();
    Require(!GuestFiles::AdoptOwned_nid_no_patch(descriptor, 7, Cleanup) && errno == 22,
        "invalid adoption did not return guest EINVAL");
    Require(Closed(descriptor), "invalid adoption leaked its owned descriptor");
    Require(cleanupCount == 1 && readableAtCleanup == 1, "adoption cleanup was not exactly once before close");
    descriptor = OwnedDescriptor();
    auto pair = GuestFiles::AdoptPairOwned_nid_no_patch(-1, descriptor, 0, 1, Cleanup);
    Require(!pair[0] && !pair[1] && errno == 9, "invalid pair published an entry");
    Require(Closed(descriptor), "invalid pair leaked its valid descriptor");
    const int first = OwnedDescriptor(), second = OwnedDescriptor();
    pair = GuestFiles::AdoptPairOwned_nid_no_patch(first, second, 0, 9, Cleanup);
    Require(!pair[0] && !pair[1] && errno == 22, "invalid access pair published an entry");
    Require(Closed(first) && Closed(second), "pair rollback leaked a native descriptor");
    Require(cleanupCount == 4 && readableAtCleanup == 4, "pair rollback cleanup count mismatch");
}

void CheckPairAndIdentity() {
    auto pair = GuestFiles::AdoptPairOwned_nid_no_patch(OwnedDescriptor(), OwnedDescriptor(), 0, 1, Cleanup);
    Require(pair[0] && pair[1], "valid pair adoption failed");
    const int first = GuestFiles::LogicalDescriptor_nid_no_patch(pair[0]);
    const int second = GuestFiles::LogicalDescriptor_nid_no_patch(pair[1]);
    Require(first == 3 && second == 4, "pair did not publish the lowest distinct logical IDs");
    Require(GuestFiles::AccessMode_nid_no_patch(pair[0]) == 0 && GuestFiles::AccessMode_nid_no_patch(pair[1]) == 1,
        "pair access modes changed");
    GuestFiles::Identity previous = pair[0];
    const int native = GuestFiles::NativeDescriptor_nid_no_patch(pair[0]);
    auto pin = std::move(pair[0]);
    errno = 66;
    Require(GuestFiles::Close_nid_no_patch(first) == 0 && errno == 66, "pinned logical close failed");
    Require(::fcntl(native, F_GETFD) != -1, "logical close prematurely closed an IO lease");
    Require(!GuestFiles::Acquire_nid_no_patch(first) && errno == 9, "retired logical ID remained open");
    auto replacement = GuestFiles::AdoptOwned_nid_no_patch(OwnedDescriptor(), 2, Cleanup);
    Require(replacement && GuestFiles::LogicalDescriptor_nid_no_patch(replacement) == first,
        "released logical ID was not reused");
    Require(GuestFiles::CloseMatching_nid_no_patch(previous) == -1 && errno == 9,
        "stale identity closed a new entry");
    Require(GuestFiles::Matches_nid_no_patch(replacement), "stale close damaged replacement identity");
    const int before = cleanupCount;
    errno = 84;
    pin.reset();
    Require(errno == 84 && cleanupCount == before + 1, "deferred close did not preserve errno or clean once");
    Require(Closed(native), "released IO lease leaked its native descriptor");
    const auto replacementIdentity = GuestFiles::Identity(replacement);
    replacement.reset();
    Require(GuestFiles::CloseMatching_nid_no_patch(replacementIdentity) == 0, "matching close failed");
    const auto secondIdentity = GuestFiles::Identity(pair[1]);
    pair[1].reset();
    Require(GuestFiles::CloseMatching_nid_no_patch(secondIdentity) == 0, "second pair close failed");
}

void CheckReplacement() {
    auto initial = GuestFiles::AdoptOwned_nid_no_patch(OwnedDescriptor(), 2, Cleanup);
    Require(static_cast<bool>(initial), "initial adoption failed");
    const int logical = GuestFiles::LogicalDescriptor_nid_no_patch(initial);
    const auto identity = GuestFiles::Identity(initial);
    const int previousNative = GuestFiles::NativeDescriptor_nid_no_patch(initial);
    auto replacement = GuestFiles::ReplaceOwnedMatching_nid_no_patch(identity, OwnedDescriptor(), 1, Cleanup);
    Require(replacement && GuestFiles::LogicalDescriptor_nid_no_patch(replacement) == logical,
        "replacement changed logical ID");
    Require(!GuestFiles::Matches_nid_no_patch(identity) && errno == 9, "replacement retained old identity");
    Require(::fcntl(previousNative, F_GETFD) != -1, "replacement closed a pinned original descriptor");
    errno = 35;
    initial.reset();
    Require(errno == 35 && Closed(previousNative), "replacement retirement leaked or changed errno");
    const int rejected = OwnedDescriptor();
    Require(!GuestFiles::ReplaceOwnedMatching_nid_no_patch(identity, rejected, 2, Cleanup) && errno == 9,
        "stale replacement committed");
    Require(Closed(rejected), "stale replacement leaked its owned descriptor");
    Require(GuestFiles::Matches_nid_no_patch(replacement), "stale replacement damaged live entry");
    const auto current = GuestFiles::Identity(replacement);
    replacement.reset();
    Require(GuestFiles::CloseMatching_nid_no_patch(current) == 0, "replacement cleanup failed");
}
}

int main() {
    try {
        Require(GuestFiles::InitializeStandards_nid_no_patch() == 0, "standard snapshot failed");
        errno = 77;
        Require(GuestFiles::NativeError_nid_no_patch(EAGAIN) == 35 && errno == 77, "EAGAIN mapping changed errno");
        Require(GuestFiles::NativeError_nid_no_patch(EOVERFLOW) == 84, "EOVERFLOW mapping mismatch");
        CheckRollback();
        CheckPairAndIdentity();
        CheckReplacement();
        Require(cleanupCount == readableAtCleanup, "cleanup ran after native close");
        std::cout << "PASS: guest descriptor ownership, rollback, weak identity and deferred cleanup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
