"""LumenGI S1 wrap-up: offline PathTracer vs LumenGI direction comparison.

Role / purpose
--------------
Reads the numpy arrays written by tests/lumengi/run_pathreference.py and the
LumenGI diffuseGI from the same run, and answers the S1 Gate question "间接光
颜色传播方向与 PathTracer 一致" (task.md 6) with direction metrics.

Semantic alignment (method choice, see report)
----------------------------------------------
* LumenGI.diffuseGI  = one-bounce INDIRECT diffuse only (already multiplied by
  primary diffuse albedo).
* PathTracer.color   = full image (direct + indirect). Comparing it directly
  against diffuseGI would be dominated by direct light, so instead:
      b_ind = PT(maxBounces=1) - PT(maxBounces=0)
  With useRussianRoulette=False and NEE+MIS the two runs consume IDENTICAL RNG
  sequences for every shared path segment (fixedSeed=1337), so the shared
  primary-direct / primary-emission / vertex-1-emission contributions cancel
  EXACTLY and b_ind is exactly the one-bounce indirect transport
  (vertex-1 NEE direct light + vertex-2 emissive scatter) - i.e. the same
  physical quantity LumenGI estimates.
* Chosen metric set (方案 B + A in the task brief):
  - per-pixel RGB cosine similarity of normalized colors on masked nonzero
    pixels (primary "direction" metric),
  - grayscale Pearson correlation + per-channel R/G/B correlations,
  - permutation null (pixel-shuffle) for statistical significance (z-score),
  - energy ratio and relRMSE as context (task.md 15.4 metrics, not gating),
  - PT full color vs LumenGI as context (direct-light dominated).

Verdict (provisional C2 thresholds, subject to S0 freeze, task.md 15.4):
  direction-consistent PASS iff at spp=1024:
      z-score >= 10 and Pearson r >= 0.5 and mean cosine >= 0.7
  and at spp=256 the same-direction sign holds (mean cosine >= 0.6).
  The overall S1-gate verdict additionally requires scene coverage: the
  fraction of pixels where LumenGI.diffuseGI carries a nonzero signal must
  be >= 0.5 (a GI that only lights 0.3% of the scene does not satisfy
  "间接光颜色传播方向与 PathTracer 一致", even if its direction is right).
  KNOWN S1 finding this script quantifies: LumenGI's active tracing path
  evaluates secondary-hit lighting ONLY as (hit-surface emission + analytic
  light sampling); emissive-triangle next-event sampling is not wired
  (LumenHitLighting.slang:75-95, matches todo.md 2.3). Cornell's only light
  is an emissive quad, so diffuseGI is nonzero only where the single bounce
  ray directly hits the light quad.

Usage
-----
    python tests\lumengi\compare_reference.py
    (reads artifacts/lumengi/S1/reference-compare/, writes metrics.json,
     report.md and PNGs into the same directory)

Requires only numpy (+ PIL for the optional PNGs); no EXR library.
"""

import argparse
import json
import math
import os

import numpy as np

EPS_LUM = 0.0  # mask = strictly positive luminance on BOTH sides
Z_THRESHOLD = 10.0
PEARSON_THRESHOLD = 0.5
COSINE_THRESHOLD_1024 = 0.7
COSINE_THRESHOLD_256 = 0.6
COVERAGE_THRESHOLD = 0.5
HIST_BINS = 21
NULL_PERMS = 200


def load_rgb(path):
    arr = np.load(path)
    if arr.ndim != 3 or arr.shape[2] < 3:
        raise ValueError("unexpected array shape %s in %s" % (arr.shape, path))
    return np.ascontiguousarray(arr[..., :3], dtype="float32")


def luminance(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def finite_mask(*arrays):
    mask = np.isfinite(arrays[0])
    for arr in arrays[1:]:
        mask &= np.isfinite(arr)
    return mask.all(axis=-1)


def masked_metric_report(a, b, mask):
    """Direction metrics between two RGB images on masked pixels."""
    a_m = a[mask]
    b_m = b[mask]

    an = np.linalg.norm(a_m, axis=-1)
    bn = np.linalg.norm(b_m, axis=-1)
    cos = np.sum(a_m * b_m, axis=-1) / np.maximum(an * bn, 1e-12)
    cos = np.clip(cos, -1.0, 1.0)

    a_lum = luminance(a_m)
    b_lum = luminance(b_m)
    p = np.corrcoef(a_lum, b_lum)[0, 1]
    r_per_channel = [
        float(np.corrcoef(a_m[..., c], b_m[..., c])[0, 1]) for c in range(3)
    ]

    hist_counts, hist_edges = np.histogram(cos, bins=HIST_BINS, range=(-1.0, 1.0))
    pct = [0.05, 0.5, 0.95, 0.99]
    quantiles = np.quantile(cos, pct)
    return {
        "masked_pixels": int(mask.sum()),
        "cosine_mean": float(cos.mean()),
        "cosine_std": float(cos.std()),
        "cosine_p5": float(quantiles[0]),
        "cosine_p50": float(quantiles[1]),
        "cosine_p95": float(quantiles[2]),
        "cosine_p99": float(quantiles[3]),
        "cosine_hist_counts": [int(c) for c in hist_counts],
        "cosine_hist_edges": [float(e) for e in hist_edges],
        "pearson_gray": float(p),
        "pearson_rgb_channels": [float(v) for v in r_per_channel],
    }


def permutation_null(a_lum, b_lum, mask, rng, n=NULL_PERMS):
    """Null distribution of Pearson r under pixel-shuffle of b."""
    a_l = a_lum[mask]
    b_l = b_lum[mask]
    null = np.empty(n)
    for i in range(n):
        shuffled = b_l[rng.permutation(b_l.size)]
        null[i] = np.corrcoef(a_l, shuffled)[0, 1]
    return null


def percentile_summary(arr):
    p = np.quantile(arr, [0.05, 0.5, 0.95])
    return {"mean": float(arr.mean()), "std": float(arr.std()),
            "p5": float(p[0]), "p50": float(p[1]), "p95": float(p[2])}


def save_histogram_png(edges, counts, path):
    from PIL import Image, ImageDraw

    W, H = 720, 360
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    x0, x1, y0, y1 = 60, W - 20, 30, H - 40
    cmin, cmax = edges[0], edges[-1]
    n = len(counts)
    maxc = max(counts) or 1
    for i in range(n):
        xa = x0 + (edges[i] - cmin) / (cmax - cmin) * (x1 - x0)
        xb = x0 + (edges[i + 1] - cmin) / (cmax - cmin) * (x1 - x0)
        h = counts[i] / maxc * (y1 - y0)
        d.rectangle([xa, y1 - h, xb, y1], fill=(30, 90, 180))
    d.line([x0, y1, x1, y1], fill=0)
    d.text((x0, 6), "per-pixel RGB cosine similarity (masked pixels)", fill=0)
    img.save(path)


def save_heatmap_png(a, b, cos, mask, path):
    from PIL import Image

    def scale(rgb):
        v = np.clip(rgb, 0.0, None)
        m = v.max() or 1.0
        return np.clip(v / m * 255.0, 0, 255).astype("uint8")

    cos_img = np.zeros(cos.shape, dtype="uint8")
    cos_img[mask] = np.clip(cos[mask] * 0.5 + 0.5, 0.0, 1.0) * 255
    cos_img = cos_img.astype("uint8")
    panels = [scale(a), scale(b), np.stack([cos_img] * 3, axis=-1)]
    img = Image.new("RGB", (a.shape[1] * 3, a.shape[0]), (0, 0, 0))
    for i, panel in enumerate(panels):
        img.paste(Image.fromarray(panel, "RGB"), (i * a.shape[1], 0))
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir", nargs="?", default="artifacts/lumengi/S1/reference-compare")
    args = ap.parse_args()
    d = args.data_dir

    def npy(name):
        path = os.path.join(d, name)
        if not os.path.isfile(path):
            raise FileNotFoundError("missing " + path)
        return load_rgb(path)

    a = npy("lumengi_diffuseGI_f1.npy")
    pt = {}
    for spp in (256, 1024):
        pt[(spp, 4)] = npy("pt_%d_b4_f1.npy" % spp)
        pt[(spp, 1)] = npy("pt_%d_b1_f1.npy" % spp)
        pt[(spp, 0)] = npy("pt_%d_b0_f1.npy" % spp)

    a_lum = luminance(a)
    f_mask = finite_mask(a, *pt.values())
    a_lum = np.where(f_mask, a_lum, 0.0)
    a_lum = np.ascontiguousarray(a_lum)
    report = {}
    results = {}
    for spp in (256, 1024):
        b_ind = pt[(spp, 1)] - pt[(spp, 0)]
        b_ind_lum = luminance(b_ind)
        mask = f_mask & (a_lum > EPS_LUM) & (b_ind_lum > EPS_LUM)
        m = masked_metric_report(a, b_ind, mask)

        rng = np.random.default_rng(1337)
        null = permutation_null(a_lum, b_ind_lum, mask, rng)
        m["perm_null"] = percentile_summary(null)
        m["z_score"] = float((m["pearson_gray"] - m["perm_null"]["mean"]) / (m["perm_null"]["std"] or 1.0))

        a_m = a[mask]
        b_m = b_ind[mask]
        m["energy_ratio_mean_a_over_b"] = float(a_m.mean() / (b_m.mean() or 1e-12))
        denom = np.linalg.norm(b_m)
        m["relrmse_a_vs_b"] = float(np.linalg.norm(a_m - b_m) / (denom or 1e-12))

        b_full = pt[(spp, 4)]
        m_full = masked_metric_report(a, b_full, f_mask)
        m["context_full_color_cosine_mean"] = m_full["cosine_mean"]
        m["context_full_color_pearson_gray"] = m_full["pearson_gray"]
        m["coverage_lumens_nonzero_frac"] = float((a_lum > EPS_LUM).mean())
        m["coverage_pt_indirect_nonzero_frac"] = float((b_ind_lum > EPS_LUM).mean())
        m["coverage_compared_frac"] = float(mask.mean())
        results[str(spp)] = m

    report["spp"] = {k: v for k, v in sorted(results.items())}

    r1024 = results["1024"]
    r256 = results["256"]
    z = r1024["z_score"]
    pear = r1024["pearson_gray"]
    cos1024 = r1024["cosine_mean"]
    cos256 = r256["cosine_mean"]
    cov = r1024["coverage_lumens_nonzero_frac"]
    direction_pass = (z >= Z_THRESHOLD and pear >= PEARSON_THRESHOLD
                      and cos1024 >= COSINE_THRESHOLD_1024 and cos256 >= COSINE_THRESHOLD_256)
    coverage_pass = cov >= COVERAGE_THRESHOLD
    passed = direction_pass and coverage_pass
    report["verdict"] = {
        "direction_consistent_on_nonzero_region": bool(direction_pass),
        "coverage_sufficient": bool(coverage_pass),
        "gate_overall": bool(passed),
        "criterion": {
            "z_score_ge_10_1024": z,
            "pearson_ge_0.5_1024": pear,
            "cosine_ge_0.7_1024": cos1024,
            "cosine_ge_0.6_256": cos256,
            "lumens_nonzero_coverage_ge_0.5": cov,
        },
        "thresholds_provisional_note": "C2 proposed thresholds; freeze per task.md 15.4",
    }
    report["config"] = {
        "scene": "media/test_scenes/cornell_box.pyscene",
        "resolution": [640, 360],
        "frame": 1,
        "path_tracer": {"seed": 1337, "useRussianRoulette": False,
                        "indirect_variant": "PT(bounces=1) - PT(bounces=0), same seed -> exact cancellation of shared segments"},
        "tone_mapper": {"autoExposure": False, "exposureCompensation": 0.0,
                        "numeric_processing": "not used numerically; linear HDR compared (PT.color, LumenGI.diffuseGI)"},
        "mask": "pixels with strictly positive luminance in both LumenGI.diffuseGI and PT indirect",
    }

    with open(os.path.join(d, "metrics.json"), "w") as fh:
        json.dump(report, fh, indent=2)

    try:
        from PIL import Image as _PIL  # noqa: F401
        save_histogram_png(r1024["cosine_hist_edges"], r1024["cosine_hist_counts"],
                           os.path.join(d, "cosine_histogram_1024.png"))
        b_ind1024 = pt[(1024, 1)] - pt[(1024, 0)]
        mask1024 = f_mask & (a_lum > EPS_LUM) & (luminance(b_ind1024) > EPS_LUM)
        cos_full = np.zeros(a.shape[:2], dtype="float32")
        an = np.linalg.norm(a, axis=-1)
        bn = np.linalg.norm(b_ind1024, axis=-1)
        prod = np.sum(a * b_ind1024, axis=-1) / np.maximum(an * bn, 1e-12)
        cos_full[mask1024] = np.clip(prod[mask1024], -1.0, 1.0)
        save_heatmap_png(a, b_ind1024, cos_full, mask1024,
                         os.path.join(d, "panel_lumens_ptind_1024.png"))
    except ImportError:
        pass

    with open(os.path.join(d, "report.md"), "w") as fh:
        fh.write("# LumenGI S1 wrap-up: PathTracer direction comparison\n\n")
        fh.write("See metrics.json for full numbers; produced by tests/lumengi/compare_reference.py.\n\n")
        fh.write("## Verdict\n\n")
        fh.write("gate_overall: `%s` | direction_consistent_on_nonzero_region: `%s` | coverage_sufficient: `%s`\n\n" % (
            "PASS" if passed else "FAIL",
            "PASS" if direction_pass else "FAIL",
            "PASS" if coverage_pass else "FAIL"))
        fh.write("1024 spp: z=%.1f pearson=%.3f cosine_mean=%.3f | 256 spp cosine_mean=%.3f | lumens coverage=%.4f\n" % (z, pear, cos1024, cos256, cov))
        fh.write("\nRoot cause (S1 finding): LumenGI's active tracing path evaluates secondary-hit\n")
        fh.write("lighting only as hit-surface emission + analytic lights; emissive-triangle NEE\n")
        fh.write("is not wired (LumenHitLighting.slang:75-95, todo.md 2.3). Cornell's only light is\n")
        fh.write("an emissive quad, so diffuseGI is nonzero only where the single bounce ray\n")
        fh.write("directly hits the light quad.\n")
        for spp in ("256", "1024"):
            r = results[spp]
            fh.write("\n## spp %s\n\n" % spp)
            fh.write("| metric | value |\n|---|---|\n")
            fh.write("| masked pixels | %d |\n" % r["masked_pixels"])
            fh.write("| cosine mean | %.4f |\n" % r["cosine_mean"])
            fh.write("| cosine std | %.4f |\n" % r["cosine_std"])
            fh.write("| cosine p50 | %.4f |\n" % r["cosine_p50"])
            fh.write("| cosine p95 | %.4f |\n" % r["cosine_p95"])
            fh.write("| pearson gray | %.4f |\n" % r["pearson_gray"])
            fh.write("| pearson R/G/B | %.4f / %.4f / %.4f |\n" % tuple(r["pearson_rgb_channels"]))
            fh.write("| z-score (permutation null) | %.2f |\n" % r["z_score"])
            fh.write("| energy ratio mean(a/b) | %.4f |\n" % r["energy_ratio_mean_a_over_b"])
            fh.write("| relRMSE | %.4f |\n" % r["relrmse_a_vs_b"])
            fh.write("| coverage lumens nonzero | %.4f |\n" % r["coverage_lumens_nonzero_frac"])
            fh.write("| coverage PT indirect nonzero | %.4f |\n" % r["coverage_pt_indirect_nonzero_frac"])
            fh.write("| compared fraction | %.4f |\n" % r["coverage_compared_frac"])
            fh.write("| context: PT full color cosine mean | %.4f |\n" % r["context_full_color_cosine_mean"])
            fh.write("| context: PT full color pearson | %.4f |\n" % r["context_full_color_pearson_gray"])
    print("COMPARE gate_overall:", "PASS" if passed else "FAIL",
          "| direction:", "PASS" if direction_pass else "FAIL",
          "| coverage:", "PASS" if coverage_pass else "FAIL",
          "z", round(z, 1), "pear", round(pear, 3),
          "cos1024", round(cos1024, 3), "cos256", round(cos256, 3), "cov", round(cov, 4))


if __name__ == "__main__":
    main()
