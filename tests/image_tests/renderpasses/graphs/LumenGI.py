from falcor import *


def render_graph_LumenGI():
    g = RenderGraph("LumenGI")

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

    g.markOutput("LumenGI.diffuseGI")
    g.markOutput("LumenGI.diffuseRadianceHitDist")
    g.markOutput("LumenGI.confidence")
    g.markOutput("LumenGI.bentNormal")
    g.markOutput("LumenGI.debugOutput")

    return g


LumenGI = render_graph_LumenGI()
try:
    m.addGraph(LumenGI)
except NameError:
    pass
