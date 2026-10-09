#include "SceTypes.hpp"
#include <cstdlib>
#include <cstring>

extern "C" {
const char* APS5_VABI getprogname_nid_postfix();
extern const char* __progname_nid_postfix;
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    const auto* original = __progname_nid_postfix;
    const auto savedError = *__error_nid_postfix();
    *__error_nid_postfix() = 45;
    Require(getprogname_nid_postfix() == original);
    Require(std::strcmp(getprogname_nid_postfix(), "eboot.bin") == 0);
    char name[] = "guest-program";
    __progname_nid_postfix = name;
    Require(getprogname_nid_postfix() == name);
    name[0] = 'G';
    Require(std::strcmp(getprogname_nid_postfix(), "Guest-program") == 0);
    __progname_nid_postfix = "";
    Require(getprogname_nid_postfix() == __progname_nid_postfix);
    Require(*getprogname_nid_postfix() == '\0');
    Require(*__error_nid_postfix() == 45);
    __progname_nid_postfix = original;
    *__error_nid_postfix() = savedError;
}
