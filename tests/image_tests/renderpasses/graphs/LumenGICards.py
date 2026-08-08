from falcor import *


def render_graph_LumenGICards():
    g = RenderGraph("LumenGICards")

    # Placeholder graph for the S2-C1 card capture image test. It mirrors
    # graphs/LumenGI.py exactly: the LumenCardCapture pass is not yet
    # registered by the host (root-owned integration, task.md section 7),
    # so this graph renders the same GBufferRT -> LumenGI path and the
    # capture list in test_LumenGICards.py exercises the graph loading,
    # shader compilation and scene update plumbing that the capture pass
    # will hook into. Once the host lands the capture pass, root will add
    # it here (per-pass viewport rendering into the Surface Cache atlases)
    # without renaming this file.
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


LumenGICards = render_graph_LumenGICards()
try:
    m.addGraph(LumenGICards)
except NameError:
    pass
