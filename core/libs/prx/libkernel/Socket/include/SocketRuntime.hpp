#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace GuestSockets {
constexpr int FirstDescriptor = 0x10000000;
extern "C" {
int GuestSocketClose_nid_no_patch(int descriptor);
bool GuestSocketIsOpen_nid_no_patch(int descriptor);
}
struct Iovec {
    void* base;
    std::size_t length;
};
int Duplicate(int descriptor);
int DuplicateTo(int descriptor, int target);
int Family(int descriptor);
std::int64_t Read(int descriptor, void* buffer, std::size_t length);
std::int64_t Write(int descriptor, const void* buffer, std::size_t length);
bool Ready(int descriptor, bool write, std::int64_t* data, bool* eof);
struct Interest {
    int descriptor;
    bool write;
};
std::uintptr_t CurrentWaker();
void Wake(std::uintptr_t waker);
bool WaitAny(const std::vector<Interest>& interests, std::uint64_t deadlineNanos);
std::int64_t Readv(int descriptor, const Iovec* iov, int iovcnt);
std::int64_t Writev(int descriptor, const Iovec* iov, int iovcnt);
}
