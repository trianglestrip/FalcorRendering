import sys
sys.path.append('..')
import os
from helpers import render_frames
from graphs.NaniteScene import NaniteScene as g
from falcor import *

m.addGraph(g)
m.loadScene(os.path.abspath('../../../data/nanite/cube.fnanite'))

render_frames(m, 'nanite_scene', frames=[16, 64])

exit()
