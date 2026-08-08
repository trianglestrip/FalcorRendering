from falcor import *


def render_graph_LumenGILighting():
    g = RenderGraph("LumenGILighting")

    gbuffer = createPass(
        "GBufferRT",
        {
            "samplePattern": "Center",
            "sampleCount": 1,
            "useAlphaTest": True,
        },
    )
    g.addPass(gbuffer, "GBufferRT")
    g.addPass(createPass("LumenGI"), "LumenGI")

    tone_mapper = createPass(
        "ToneMapper",
        {
            "autoExposure": False,
            "exposureCompensation": 0.0,
        },
    )
    g.addPass(tone_mapper, "ToneMapper")

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
    g.addEdge("LumenGI.diffuseGI", "ToneMapper.src")

    g.markOutput("LumenGI.diffuseGI")
    g.markOutput("LumenGI.confidence")
    g.markOutput("LumenGI.debugOutput")
    g.markOutput("ToneMapper.dst")
    return g


LumenGILighting = render_graph_LumenGILighting()
try:
    m.addGraph(LumenGILighting)
except NameError:
    pass
