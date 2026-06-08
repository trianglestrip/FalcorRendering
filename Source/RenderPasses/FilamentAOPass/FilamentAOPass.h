/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 **************************************************************************/
#pragma once

#include "Falcor.h"
#include "RenderGraph/RenderPass.h"
#include "FilamentPostProcess.h"

using namespace Falcor;

class FilamentAOPass : public RenderPass
{
public:
    FALCOR_PLUGIN_CLASS(FilamentAOPass, "FilamentAOPass", "Filament-style SAO/GTAO ambient occlusion.");

    static ref<FilamentAOPass> create(ref<Device> pDevice, const Properties& props)
    {
        return make_ref<FilamentAOPass>(pDevice, props);
    }

    FilamentAOPass(ref<Device> pDevice, const Properties& props);

    Properties getProperties() const override;
    RenderPassReflection reflect(const CompileData& compileData) override;
    void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    void renderUI(Gui::Widgets& widget) override;
    void setScene(RenderContext* pRenderContext, const ref<Scene>& pScene) override;

private:
    void parseProperties(const Properties& props);
    void syncCameraSettings();

    ref<FilamentPostProcess> mpFilamentAO;
    ref<Scene> mpScene;

    FilamentPostProcess::FilamentSettings mSettings;
    bool mUseGBufferNormals = true;
};
