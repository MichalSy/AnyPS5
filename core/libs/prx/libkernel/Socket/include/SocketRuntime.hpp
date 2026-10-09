#pragma once
namespace GuestSockets {
constexpr int FirstDescriptor = 0x10000000;
extern "C" {
int GuestSocketClose_nid_no_patch(int descriptor);
bool GuestSocketIsOpen_nid_no_patch(int descriptor);
}
int Family(int descriptor);
}
