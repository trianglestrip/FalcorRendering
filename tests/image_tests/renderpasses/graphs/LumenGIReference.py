from falcor import *


def render_graph_LumenGIReference():
    g = RenderGraph("LumenGIReference")

    gbuffer = createPass(
        "GBufferRT",
        {
            "samplePattern": "Center",
            "sampleCount": 1,
            "useAlphaTest": True,
        },
    )
    g.addPass(gbuffer, "GBufferRT")

    lumen_gi = createPass("LumenGI")
    g.addPass(lumen_gi, "LumenGI")

    # A fixed seed and multi-sample dispatch make the reference repeatable
    # without relying on temporal accumulation across test frames.
    path_tracer = createPass(
        "PathTracer",
        {
            "samplesPerPixel": 64,
            "maxSurfaceBounces": 4,
            "useRussianRoulette": False,
            "fixedSeed": 1337,
        },
    )
    g.addPass(path_tracer, "PathTracer")

    tone_mapper_lumen = createPass(
        "ToneMapper",
        {
            "autoExposure": False,
            "exposureCompensation": 0.0,
        },
    )
    g.addPass(tone_mapper_lumen, "ToneMapperLumen")

    tone_mapper_reference = createPass(
        "ToneMapper",
        {
            "autoExposure": False,
            "exposureCompensation": 0.0,
        },
    )
    g.addPass(tone_mapper_reference, "ToneMapperReference")

    g.addEdge("GBufferRT.vbuffer", "LumenGI.vbuffer")
    g.addEdge("GBufferRT.linearZ", "LumenGI.linearZ")
    g.addEdge("GBufferRT.mvec", "LumenGI.mvec")
    g.addEdge("GBufferRT.mvecW", "LumenGI.mvecW")
    g.addEdge(
        "GBufferRT.normWRoughnessMaterialID",
        "LumenGI.normWRoughnessMaterialID",
    )
    g.addEdge("GBufferRT.viewW", "LumenGI.viewW")
    g.addEdge("GBufferRT.diffuseOpacity", "LumenGI.diffuseOpacity")
    g.addEdge("GBufferRT.emissive", "LumenGI.emissive")

    # Both renderers use the same primary visibility and view direction.
    g.addEdge("GBufferRT.vbuffer", "PathTracer.vbuffer")
    g.addEdge("GBufferRT.mvec", "PathTracer.mvec")
    g.addEdge("GBufferRT.viewW", "PathTracer.viewW")

    g.addEdge("LumenGI.diffuseGI", "ToneMapperLumen.src")
    g.addEdge("PathTracer.color", "ToneMapperReference.src")

    # Linear HDR outputs are intended for numeric/FLIP preprocessing.
    g.markOutput("LumenGI.diffuseGI")
    g.markOutput("LumenGI.diffuseRadianceHitDist")
    g.markOutput("LumenGI.confidence")
    g.markOutput("LumenGI.debugOutput")
    g.markOutput("PathTracer.color")
    g.markOutput("PathTracer.nrdDiffuseRadianceHitDist")

    # Identically tone-mapped outputs are convenient for display-space FLIP.
    g.markOutput("ToneMapperLumen.dst")
    g.markOutput("ToneMapperReference.dst")

    return g


LumenGIReference = render_graph_LumenGIReference()
try:
    m.addGraph(LumenGIReference)
except NameError:
    pass
