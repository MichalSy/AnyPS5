#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_REGISTEREDPREPARATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_REGISTEREDPREPARATION_HPP

#include "prx/libSceAgcDriver/Execution/include/PreparationPool.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <array>
#include <memory>
#include <stop_token>

namespace AgcDriver::DriverDetail {

std::array<std::shared_ptr<const ShaderRecompiler::SourceHandle>, 2> PrepareComputeTemplates(
    PreparationPool& pool, const ShaderRecompiler::RecompileRequest& request, std::stop_token token = {});

}

#endif
