#ifndef CORE_SHADER_RECOMPILER_UNRESOLVEDSCALARCALLREQUIREMENT_HPP
#define CORE_SHADER_RECOMPILER_UNRESOLVEDSCALARCALLREQUIREMENT_HPP

#include <cstdint>

namespace ShaderRecompiler {

struct UnresolvedScalarCallRequirement {
    std::uint32_t programCounter;
    std::uint32_t targetRegister;
    std::uint32_t linkRegister;
};

}

#endif
