"""Run with python -m unittest discover -s tests -p test_marsh_islands.py."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/terrain_lab"))
import marsh_islands as marsh


class MarshIslandsTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1]
        self.rule = json.loads((root / "content/config/terrain_core/props.json").read_text())["megaliths"]["marsh_islands"]

    def test_every_riser_keeps_minimum_and_total_height(self):
        rng = np.random.default_rng(11)
        for _ in range(100):
            profile = marsh.island_profile(rng, 350, self.rule)
            self.assertTrue(np.all(profile["risers"] >= self.rule["riser_min_m"]))
            self.assertGreaterEqual(profile["risers"].sum(), self.rule["top_m"][0])
            self.assertLessEqual(profile["risers"].sum(), self.rule["top_m"][1] + 1e-8)

    def test_island_has_dry_flat_top_and_leaves_outside_untouched(self):
        rule = dict(self.rule, tread_noise_m=0)
        profile = marsh.island_profile(np.random.default_rng(5), 300, rule)
        ground = np.full((256, 256), 12.0, np.float32)
        mask, _ = marsh.raise_island(ground, 8, 1024, 1024, 14, profile, rule)
        self.assertTrue(np.all(ground[~mask] == 12))
        self.assertTrue(np.all(ground[120:136, 120:136] > 14))
        self.assertLess(float(np.ptp(ground[120:136, 120:136])), 1e-4)

    def test_rerun_retains_original_base_but_new_generation_replaces_it(self):
        with tempfile.TemporaryDirectory() as directory:
            current, base = Path(directory) / "current", Path(directory) / "base"
            current.write_bytes(b"original")
            marsh.base_of(current, base, {}, "hash")
            current.write_bytes(b"islands")
            record = {"hash": marsh.digest(current)}
            self.assertEqual(marsh.base_of(current, base, record, "hash"), base)
            self.assertEqual(base.read_bytes(), b"original")
            current.write_bytes(b"new generation")
            marsh.base_of(current, base, record, "hash")
            self.assertEqual(base.read_bytes(), b"new generation")


if __name__ == "__main__":
    unittest.main()
