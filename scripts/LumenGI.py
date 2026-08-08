from falcor import *


def render_graph_LumenGI():
    g = RenderGraph("LumenGI")

    gbuffer = createPass("GBufferRT", {
        "samplePattern": "Center",
        "sampleCount": 1,
        "useDOF": False,
    })
    g.addPass(gbuffer, "GBufferRT")

    lumen = createPass("LumenGI", {
        "enabled": True,
        "traceMode": "HardwareRT",
        "qualityPreset": "High",
    })
    g.addPass(lumen, "LumenGI")

    for channel in [
        "vbuffer",
        "linearZ",
        "mvec",
        "mvecW",
        "normWRoughnessMaterialID",
        "viewW",
        "diffuseOpacity",
        "emissive",
    ]:
        g.addEdge(f"GBufferRT.{channel}", f"LumenGI.{channel}")

    g.markOutput("LumenGI.diffuseGI")
    g.markOutput("LumenGI.diffuseRadianceHitDist")
    g.markOutput("LumenGI.confidence")
    g.markOutput("LumenGI.debugOutput")
    return g


LumenGI = render_graph_LumenGI()
try:
    m.addGraph(LumenGI)
except NameError:
    pass

