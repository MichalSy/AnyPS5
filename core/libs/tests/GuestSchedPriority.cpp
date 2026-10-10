#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdlib>

extern "C" {
int APS5_VABI sched_get_priority_max_nid_postfix(int policy);
int APS5_VABI sched_get_priority_min_nid_postfix(int policy);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    constexpr int realtimePolicies[] = {1, 3};
    for (int policy : realtimePolicies) {
        *__error_nid_postfix() = 13;
        Require(sched_get_priority_max_nid_postfix(policy) == 256);
        Require(*__error_nid_postfix() == 13);
        Require(sched_get_priority_min_nid_postfix(policy) == 767);
        Require(*__error_nid_postfix() == 13);
    }
    *__error_nid_postfix() = 13;
    Require(sched_get_priority_max_nid_postfix(2) == 103);
    Require(*__error_nid_postfix() == 13);
    Require(sched_get_priority_min_nid_postfix(2) == 0);
    Require(*__error_nid_postfix() == 13);
    constexpr int invalidPolicies[] = {-1, 0, 4};
    for (int policy : invalidPolicies) {
        *__error_nid_postfix() = 13;
        Require(sched_get_priority_max_nid_postfix(policy) == -1);
        Require(*__error_nid_postfix() == 22);
        *__error_nid_postfix() = 13;
        Require(sched_get_priority_min_nid_postfix(policy) == -1);
        Require(*__error_nid_postfix() == 22);
    }
}
