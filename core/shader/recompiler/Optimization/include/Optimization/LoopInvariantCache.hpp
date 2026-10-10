#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_LOOPINVARIANTCACHE_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_LOOPINVARIANTCACHE_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>

namespace ShaderRecompiler {

struct LoopInvariantCacheStats {
    std::uint32_t cachedSlices = 0;
    std::uint32_t cachedValues = 0;
    std::uint32_t cachedLoads = 0;
    std::uint32_t cachedInstructions = 0;
};

class LoopInvariantCache {
public:
    [[nodiscard]] LoopInvariantCacheStats Cache(IrProgram& program) const;
};

}

#endif
