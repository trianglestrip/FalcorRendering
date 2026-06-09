#pragma once
#include "Falcor.h"
#include "RenderGraph/RenderPass.h"

using namespace Falcor;

/** Merge Nanite material-resolve outputs with GBuffer geometry using depth testing. */
class NaniteGBufferComposite : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(NaniteGBufferComposite, "NaniteGBufferComposite", "Merge Nanite and GBuffer outputs.");

    static ref<NaniteGBufferComposite> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<NaniteGBufferComposite>(pDevice, props);
    }

    NaniteGBufferComposite(ref<Device> pDevice, const Properties& props);

    virtual Properties getProperties() const override { return {}; }
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pRenderContext, const CompileData& compileData) override;
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;

private:
    ref<ComputePass> mpCompositePass;
    uint2 mFrameDim = {0, 0};
};
