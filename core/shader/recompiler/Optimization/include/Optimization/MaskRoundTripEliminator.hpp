#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_INCLUDE_OPTIMIZATION_MASKROUNDTRIPELIMINATOR_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_INCLUDE_OPTIMIZATION_MASKROUNDTRIPELIMINATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include <cstdint>

namespace ShaderRecompiler {

struct MaskRoundTripEliminationStats {
    std::uint32_t rewrittenBallotBits = 0;
    std::uint32_t rewrittenConstantBits = 0;
};

class MaskRoundTripEliminator {
public:
    [[nodiscard]] MaskRoundTripEliminationStats Eliminate(IrProgram& program) const;
};

}
#endif
