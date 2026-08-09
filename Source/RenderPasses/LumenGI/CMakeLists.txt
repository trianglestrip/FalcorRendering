add_plugin(LumenGI)

target_sources(LumenGI PRIVATE
    LumenGI.cpp
    LumenGI.h
    LumenGIData.slang
    LumenGIDebug.cs.slang
    LumenGIStats.h
    Lighting/LumenGILighting.slang
    Lighting/LumenGILightSampling.slang
    Lighting/LumenSurfaceCacheLighting.cs.slang
    Lighting/LumenSurfaceCacheLightingData.slang
    Tracing/LumenHardwareTrace.rt.slang
    Tracing/LumenHitLighting.slang
    Cards/LumenCardScene.h
    Cards/LumenCardScene.cpp
    Cards/LumenCardPlacement.cs.slang
    SurfaceCache/LumenSurfaceCache.h
    Capture/LumenCardCaptureData.slang
    Capture/LumenCardCapture.3d.slang
    ScreenTrace/LumenScreenTraceData.slang
    ScreenTrace/LumenScreenTrace.cs.slang
    ScreenTrace/LumenHZB.h
    ScreenTrace/LumenHZBBuild.cs.slang
    ScreenProbe/LumenScreenProbe.h
    ScreenProbe/LumenScreenProbeData.slang
    ScreenProbe/LumenScreenProbeTrace.cs.slang
    ScreenProbe/LumenScreenProbeIntegrate.cs.slang
    ScreenProbe/LumenScreenProbeInterpolate.cs.slang
    Temporal/LumenTemporalFilterData.slang
    Temporal/LumenTemporalFilter.cs.slang
)

target_copy_shaders(LumenGI RenderPasses/LumenGI)

target_source_group(LumenGI "RenderPasses")
