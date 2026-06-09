from falcor import *

def render_graph_NaniteScene():
    g = RenderGraph('NaniteSceneGraph')
    GBufferRaster = createPass('GBufferRaster', {'samplePattern': 'Center', 'forceCullMode': True, 'cull': 'Back'})
    NaniteRaster = createPass('NaniteRaster')
    Composite = createPass('NaniteGBufferComposite')
    g.addPass(GBufferRaster, 'GBufferRaster')
    g.addPass(NaniteRaster, 'NaniteRaster')
    g.addPass(Composite, 'NaniteGBufferComposite')
    g.addEdge('GBufferRaster.depth', 'NaniteGBufferComposite.sceneDepth')
    g.addEdge('GBufferRaster.posW', 'NaniteGBufferComposite.scenePosW')
    g.addEdge('GBufferRaster.normW', 'NaniteGBufferComposite.sceneNormW')
    g.addEdge('GBufferRaster.faceNormalW', 'NaniteGBufferComposite.sceneFaceNormalW')
    g.addEdge('GBufferRaster.texC', 'NaniteGBufferComposite.sceneTexC')
    g.addEdge('GBufferRaster.mtlData', 'NaniteGBufferComposite.sceneMtlData')
    g.addEdge('NaniteRaster.depth', 'NaniteGBufferComposite.naniteDepth')
    g.addEdge('NaniteRaster.position', 'NaniteGBufferComposite.nanitePosition')
    g.addEdge('NaniteRaster.normal', 'NaniteGBufferComposite.naniteNormal')
    g.addEdge('NaniteRaster.texCoord', 'NaniteGBufferComposite.naniteTexCoord')
    g.addEdge('NaniteRaster.baseColor', 'NaniteGBufferComposite.naniteBaseColor')
    g.markOutput('NaniteGBufferComposite.output')
    g.markOutput('NaniteGBufferComposite.depth')
    g.markOutput('NaniteGBufferComposite.normW')
    return g

NaniteScene = render_graph_NaniteScene()
try: m.addGraph(NaniteScene)
except NameError: None
