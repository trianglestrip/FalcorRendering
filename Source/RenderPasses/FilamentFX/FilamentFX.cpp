#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "FilamentPostProcess.h"

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, FilamentPostProcess>();
}