import json
import tempfile
import unittest
from pathlib import Path

from benchmark_manifest import build_manifest, percentile, summarize_profiler_capture, write_json


class BenchmarkManifestTests(unittest.TestCase):
    def test_percentile_interpolates_and_handles_empty_input(self):
        self.assertIsNone(percentile([], 0.95))
        self.assertEqual(percentile([4.0], 0.95), 4.0)
        self.assertEqual(percentile([0.0, 10.0], 0.5), 5.0)
        with self.assertRaises(ValueError):
            percentile([1.0], 1.1)

    def test_profiler_summary_has_stable_statistics(self):
        capture = {
            "frame_count": 4,
            "events": {
                "LumenGI/gpu_time": {
                    "records": [1.0, 2.0, 3.0, 4.0],
                    "stats": {"mean": 2.5},
                }
            },
        }
        summary = summarize_profiler_capture(capture)
        event = summary["events"]["LumenGI/gpu_time"]
        self.assertEqual(summary["frame_count"], 4)
        self.assertEqual(event["sample_count"], 4)
        self.assertEqual(event["mean_ms"], 2.5)
        self.assertEqual(event["p50_ms"], 2.5)
        self.assertAlmostEqual(event["p95_ms"], 3.85)

    def test_manifest_and_json_are_serializable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            manifest = build_manifest(
                root,
                {"width": 1280, "height": 720},
                status="completed",
                resource_stats={"total_memory_bytes": 512},
            )
            output = root / "manifest.json"
            write_json(output, manifest)
            loaded = json.loads(output.read_text(encoding="utf-8"))

        self.assertEqual(loaded["schema_version"], 1)
        self.assertEqual(loaded["benchmark"], "LumenGI")
        self.assertEqual(loaded["configuration"]["width"], 1280)
        self.assertEqual(loaded["lumen_gi"]["resource_stats"]["total_memory_bytes"], 512)
        self.assertFalse(output.with_name(output.name + ".tmp").exists())


if __name__ == "__main__":
    unittest.main()
