#ifndef CORE_SHADER_RECOMPILER_SPIRVBACKEND_INDEPENDENTCOMPUTEHALVES_HPP
#define CORE_SHADER_RECOMPILER_SPIRVBACKEND_INDEPENDENTCOMPUTEHALVES_HPP
#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
namespace ShaderRecompiler {
[[nodiscard]] const IrValue* IndependentComputeHalfStore(const IrProgram& program, const ShaderComputeInputInfo& input);
}
#endif
