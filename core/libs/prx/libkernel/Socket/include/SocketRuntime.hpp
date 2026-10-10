#pragma once
#include <cstddef>
#include <cstdint>
namespace GuestSockets {
constexpr int FirstDescriptor = 0x10000000;
extern "C" {
int GuestSocketClose_nid_no_patch(int descriptor);
bool GuestSocketIsOpen_nid_no_patch(int descriptor);
}
int Family(int descriptor);
std::int64_t Read(int descriptor, void* buffer, std::size_t length);
std::int64_t Write(int descriptor, const void* buffer, std::size_t length);
}
