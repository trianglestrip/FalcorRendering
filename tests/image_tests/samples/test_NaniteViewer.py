IMAGE_TEST = {
    "executable": "NaniteViewer",
    "device_types": ["d3d12"],
    "width": 512,
    "height": 512,
    "args": [
        "--fnanite", "data/nanite/cube.fnanite",
        "--screenshot", "nanite_cube.png",
        "--csv", "nanite_perf.csv",
    ],
}

# Standalone executable test; image generation is handled by run_image_tests.py.
exit()
