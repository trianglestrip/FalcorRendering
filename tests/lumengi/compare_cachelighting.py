"""LumenGI S3 Gate offline analysis: cache-lighting vs hit-lighting (pure python).

Role / purpose
--------------
Agent V (S3 Gate reference-compare asset, pure code/script round). Reads the
raw arrays dumped by tests/lumengi/run_cachelighting_reference.py and computes
the S3 gate metrics ("Surface Cache direct lighting 与 hit-lighting reference
在容差内", task.md 8 S3 gate bullet 1) offline:

  ref    = <tag>_diffuseRadianceHitDist.npy   screen-res (H, W, 4) float32
            RGB = unmodulated diffuse radiance, A = bounce-ray hit distance.
  cache  = <tag>_cacheDirectRadiance.npy      atlas-res (A, A, 4) float32
            RGB = direct radiance per cache texel (world-space).
  aligned= <tag>_cacheDirectResampled.npy     OPTIONAL S3_TODO screen-res dump
            produced by a future LumenGI resample output; when present the two
            channels are pixel-aligned and the REAL per-pixel gate metrics are
            computed. When absent the run is DISTRIBUTION/pre-flight only.

Relation to Agent L's S1 comparison (compare_reference2.py)
-----------------------------------------------------------
* The S1 protocol compares LumenGI.diffuseGI against PathTracer indirect at the
  same sample budget: direction via per-pixel RGB cosine on the joint nonzero
  mask (threshold >= 0.7), coverage via a ratio vs a sample-fair PT baseline
  (>= 0.9) plus an absolute floor (>= 0.15), and energy as CONTEXT (not gating)
  because 1-spp vs 1024-spp energy is a variance artifact.
* S3 reuses that shape but replaces the "reference" with LumenGI's own
  per-pixel hit-lighting channel (same engine, same samplers, same sample
  budget by construction, no PathTracer needed):
  - direction  = RGB cosine on the joint lit mask (>= 0.7, S1 convention);
  - coverage   = cache-lit coverage vs reference-lit coverage: ratio >= 0.9 and
    absolute joint coverage >= 0.15 (S1 convention);
  - energy     = CONTEXT only (mean/median ratios), matching S1's decision to
    keep Pearson/z and mean-energy out of gating;
  - NEW for S3: per-pixel relative RMSE and mean-abs error on the aligned joint
    mask, the literal "在容差内" numeric.
* The offline script can only compute the aligned metrics when the S3_TODO
  resample exists; until then the coverage ratio is replaced by a
  domain-by-domain occupancy pre-flight that is NOT gate-able.

S3 gate determination recommendation (comment only; thresholds S3_TODO freeze)
------------------------------------------------------------------------------
* Gate the ALIGNED mode only (cacheDirectResampled present):
      relRMSE <= REL_RMSE_MAX
      mean_abs_error / ref_mean <= MEAN_ABS_REL_MAX
      cosine_mean >= COSINE_MIN            (S1 direction protocol)
      joint_coverage / ref_coverage >= COVERAGE_RATIO_MIN   (S1 coverage protocol)
      joint_coverage >= COVERAGE_ABS_MIN
  All thresholds are S3_TODO PLACEHOLDERS below; freeze with root per task.md
  15.4 ("thresholds frozen at S0/S1, never loosened on failure").
* DISTRIBUTION mode is a pre-flight smoke (invariants + occupancy), never a
  gate verdict; a FAIL here means the capture itself is broken (e.g. atlas
  entirely unlit), not that cache-lighting differs from hit-lighting.
* Transport alignment is S3_TODO (see run_cachelighting_reference.py header):
  cacheDirectRadiance is the DIRECT term, diffuseRadianceHitDist is the
  one-bounce INDIRECT term. The metric machinery is transport-agnostic; root
  must fix which pair is the gate before freezing thresholds.

Usage
-----
    python tests\\lumengi\\compare_cachelighting.py [data_dir]
    (default data_dir artifacts/lumengi/S3/reference-compare; writes
     cachelighting.json + cachelighting_report.md into the same directory)

Requires only numpy. RUN-ONLY: prints VERDICT lines, never exits non-zero.
"""

import argparse
import json
import math
import os

import numpy as np

EPS_LUM = 0.0  # lit mask = strictly positive luminance (S1 convention)

# --- S3_TODO PLACEHOLDER thresholds; freeze with root (task.md 15.4) ---
REL_RMSE_MAX = 0.30        # S3_TODO placeholder: ||cache-ref||_2 / ||ref||_2
MEAN_ABS_REL_MAX = 0.25    # S3_TODO placeholder: mean(|cache-ref|) / ref_mean
COSINE_MIN = 0.7           # S1 direction convention (compare_reference2.py)
COVERAGE_RATIO_MIN = 0.9   # S1 coverage convention
COVERAGE_ABS_MIN = 0.15    # S1 coverage convention

QUANTILES = [0.05, 0.25, 0.5, 0.75, 0.95]

REF_SUFFIX = "_diffuseRadianceHitDist.npy"
CACHE_SUFFIX = "_cacheDirectRadiance.npy"
ALIGNED_SUFFIX = "_cacheDirectResampled.npy"


def lum(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def qsummary(values, name):
    qs = np.quantile(values, QUANTILES)
    return {
        "n": int(values.size),
        "mean": float(values.mean()),
        "p5": float(qs[0]),
        "p25": float(qs[1]),
        "p50": float(qs[2]),
        "p75": float(qs[3]),
        "p95": float(qs[4]),
        "min": float(values.min()) if values.size else None,
        "max": float(values.max()) if values.size else None,
    }


def safe_div(a, b, fallback=None):
    if b is None or b == 0.0 or not math.isfinite(b):
        return fallback
    return float(a / b)


def load_rgb(path, label):
    arr = np.load(path)
    if arr.ndim != 3 or arr.shape[2] < 3:
        raise ValueError("%s: unexpected shape %s" % (label, arr.shape))
    return np.ascontiguousarray(arr[..., :3], dtype="float32")


def invariants(arr, label):
    finite = bool(np.isfinite(arr).all())
    nonneg = bool(float(arr.min()) >= 0.0)
    return {"finite": finite, "nonnegative": nonneg, "label": label}


def distribution_stats(rgb, alpha=None):
    """Domain-by-domain occupancy / energy stats (NOT spatially aligned)."""
    l = lum(rgb)
    lit = (l > EPS_LUM) & np.isfinite(l)
    n = int(lit.sum())
    lit_vals = l[lit]
    energy = qsummary(lit_vals, "energy") if n else None
    out = {
        "coverage": safe_div(float(n), float(lit.size), 0.0),
        "lit_texels": n,
        "total_texels": int(lit.size),
        "lit_energy": energy,
    }
    if alpha is not None:
        a = np.ascontiguousarray(alpha, dtype="float32")
        out["alpha"] = qsummary(a, "alpha")
    return out


def cosine_metrics(a, b, mask):
    """Per-pixel RGB cosine on the joint mask (Agent L S1 protocol)."""
    a_m, b_m = a[mask], b[mask]
    an = np.linalg.norm(a_m, axis=-1)
    bn = np.linalg.norm(b_m, axis=-1)
    c = np.clip(np.sum(a_m * b_m, axis=-1) / np.maximum(an * bn, 1e-12), -1.0, 1.0)
    return {
        "mean": float(c.mean()),
        "median": float(np.median(c)),
        "p5": float(np.quantile(c, 0.05)),
        "p95": float(np.quantile(c, 0.95)),
    }


def aligned_metrics(cache, ref):
    """Per-pixel (pixel-aligned) gate metrics on the joint lit mask.
    cache/ref are equal-shape RGB float32 arrays (H, W, 3). Returns a dict
    with metric values, or None when no joint pixels exist."""
    cl, rl = lum(cache), lum(ref)
    finite = np.isfinite(cl) & np.isfinite(rl)
    joint = finite & (cl > EPS_LUM) & (rl > EPS_LUM)
    n = int(joint.sum())
    if n == 0:
        return None

    c_m, r_m = cache[joint], ref[joint]
    diff = c_m - r_m
    abs_err = np.sqrt((diff * diff).sum(axis=-1))       # per-pixel RGB distance
    ref_norm = np.sqrt((r_m * r_m).sum(axis=-1))         # per-pixel ref RGB norm
    denom = float(np.sqrt((r_m * r_m).sum()))            # global L2 of the ref
    rel_rmse = safe_div(float(np.sqrt((diff * diff).sum())), denom)
    mean_abs = float(abs_err.mean())
    # Consistent norms for the "mean abs within tolerance" ratio: both sides use
    # the per-pixel RGB vector norm, so a scalar-multiple (cache=k*ref) gives
    # exactly |1-k| rather than a norm-mixing artifact.
    mean_abs_over_ref = safe_div(mean_abs, float(ref_norm.mean()))
    rel_abs = qsummary(abs_err / np.maximum(ref_norm, 1e-12), "relative_abs")

    ref_lit = float((finite & (rl > EPS_LUM)).mean())
    cache_lit = float((finite & (cl > EPS_LUM)).mean())
    joint_frac = float(joint.mean())

    # Energy context uses luminance (Agent L S1 convention), CONTEXT only.
    cl_m = lum(c_m)
    rl_m = lum(r_m)
    mean_ratio = safe_div(float(cl_m.mean()), float(rl_m.mean()))
    median_ratio = safe_div(float(np.median(cl_m)), float(np.median(rl_m)))

    metrics = {
        "joint_pixels": n,
        "rel_rmse": rel_rmse,
        "mean_abs_error": mean_abs,
        "mean_abs_error_over_ref_mean": mean_abs_over_ref,
        "abs_error_quantiles": qsummary(abs_err, "abs_error"),
        "relative_abs_quantiles": rel_abs,
        "coverage_ref": ref_lit,
        "coverage_cache": cache_lit,
        "coverage_joint": joint_frac,
        "coverage_ratio_cache_over_ref": safe_div(cache_lit, ref_lit, None),
        "coverage_joint_over_ref": safe_div(joint_frac, ref_lit, None),
        "cosine": cosine_metrics(cache, ref, joint),
        "energy_context": {
            "mean_cache_lum": float(cl_m.mean()),
            "mean_ref_lum": float(rl_m.mean()),
            "mean_ratio_cache_over_ref": mean_ratio,
            "median_cache_lum": float(np.median(cl_m)),
            "median_ref_lum": float(np.median(rl_m)),
            "median_ratio_cache_over_ref": median_ratio,
            "note": "energy is CONTEXT only (S1 convention, compare_reference2.py); "
            "transport alignment is S3_TODO",
        },
    }

    # S3_TODO placeholder verdict (freeze thresholds with root).
    checks = {
        "rel_rmse_le": rel_rmse is not None and rel_rmse <= REL_RMSE_MAX,
        "mean_abs_rel_le": metrics["mean_abs_error_over_ref_mean"] is not None
        and metrics["mean_abs_error_over_ref_mean"] <= MEAN_ABS_REL_MAX,
        "cosine_ge": metrics["cosine"]["mean"] >= COSINE_MIN,
        "coverage_ratio_ge": metrics["coverage_joint_over_ref"] is not None
        and metrics["coverage_joint_over_ref"] >= COVERAGE_RATIO_MIN,
        "coverage_abs_ge": joint_frac >= COVERAGE_ABS_MIN,
    }
    metrics["verdict_placeholder"] = {
        "checks": checks,
        "pass_placeholder": bool(all(checks.values())),
        "note": "S3_TODO: freeze thresholds with root; transport alignment (direct-vs-direct or "
        "indirect-vs-indirect) must be confirmed before this is gating",
    }
    return metrics


def analyze_scene(d, tag):
    ref_path = os.path.join(d, tag + REF_SUFFIX)
    cache_path = os.path.join(d, tag + CACHE_SUFFIX)
    aligned_path = os.path.join(d, tag + ALIGNED_SUFFIX)
    if not os.path.isfile(ref_path) or not os.path.isfile(cache_path):
        print("CACHECMP skip", tag, "(missing ref or cache npy)")
        return None

    ref = load_rgb(ref_path, "ref")
    cache = load_rgb(cache_path, "cache")
    ref_a = np.load(ref_path)[..., 3].astype("float32") if np.load(ref_path).ndim == 3 and np.load(ref_path).shape[2] >= 4 else None

    rec = {
        "ref_shape": list(ref.shape),
        "cache_shape": list(cache.shape),
        "invariants": {
            "ref": invariants(ref, "ref"),
            "cache": invariants(cache, "cache"),
        },
        "distribution": {
            "ref_screen": distribution_stats(ref, alpha=ref_a),
            "cache_atlas": distribution_stats(cache),
        },
    }

    aligned = None
    if os.path.isfile(aligned_path):
        aligned_arr = load_rgb(aligned_path, "aligned")
        if aligned_arr.shape != ref.shape:
            print(
                "CACHECMP warn", tag, "aligned shape", aligned_arr.shape,
                "!= ref shape", ref.shape, "-> treating as distribution-only"
            )
        else:
            aligned = aligned_metrics(aligned_arr, ref)
    if aligned is None:
        rec["mode"] = "distribution"
        rec["aligned"] = None
        rec["verdict"] = {
            "gateable": False,
            "reason": "cacheDirectResampled absent (S3_TODO resample) -> "
            "distribution/pre-flight only, NOT gateable",
        }
        print(
            "CACHECMP", tag, "mode distribution | ref cov %.4f | cache atlas cov %.4f"
            % (rec["distribution"]["ref_screen"]["coverage"],
               rec["distribution"]["cache_atlas"]["coverage"])
        )
    else:
        rec["mode"] = "aligned"
        rec["aligned"] = aligned
        vp = aligned["verdict_placeholder"]
        rec["verdict"] = {
            "gateable": True,
            "pass_placeholder": bool(vp["pass_placeholder"]),
            "reason": "S3_TODO thresholds; transport alignment S3_TODO; treat as placeholder",
        }
        print(
            "CACHECMP", tag, "mode aligned | relRMSE %.4f | meanAbs/ref %.4f | cosine %.4f | "
            "joint cov %.4f / ref cov %.4f | placeholder %s"
            % (aligned["rel_rmse"], aligned["mean_abs_error_over_ref_mean"],
               aligned["cosine"]["mean"], aligned["coverage_joint"],
               aligned["coverage_ref"], "PASS" if vp["pass_placeholder"] else "FAIL")
        )
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir", nargs="?", default="artifacts/lumengi/S3/reference-compare")
    args = ap.parse_args()
    d = args.data_dir
    os.makedirs(d, exist_ok=True)

    tags = []
    if os.path.isdir(d):
        for fn in sorted(os.listdir(d)):
            if fn.endswith(REF_SUFFIX):
                tags.append(fn[: -len(REF_SUFFIX)])

    scenes = {}
    for tag in tags:
        rec = analyze_scene(d, tag)
        if rec is not None:
            scenes[tag] = rec

    any_aligned = any(s.get("mode") == "aligned" for s in scenes.values())
    report = {
        "script": "compare_cachelighting.py",
        "overall_mode": "aligned" if any_aligned else "distribution",
        "channels": {
            "cache": "cacheDirectRadiance (atlas-res, RGB = direct radiance, world-space)",
            "ref": "diffuseRadianceHitDist (screen-res, RGB = unmodulated diffuse radiance, A = bounce hit distance)",
            "aligned": "cacheDirectResampled (screen-res S3_TODO resample; absent -> distribution-only)",
            "transport_caveat": "cacheDirectRadiance carries the DIRECT term, diffuseRadianceHitDist the "
            "one-bounce INDIRECT term; gate is only meaningful after S3_TODO transport alignment",
        },
        "relation_to_s1": {
            "protocol": "mirrors Agent L compare_reference2.py: direction = RGB cosine on joint mask "
            "(>=0.7), coverage = ratio (>=0.9) + absolute (>=0.15) vs the reference's own lit coverage, "
            "energy = context only; Pearson/z stay out of gating",
            "new_for_s3": "per-pixel relative RMSE + mean abs error on the aligned joint mask = the literal "
            "S3 '在容差内' numeric",
        },
        "thresholds_placeholder": {
            "rel_rmse_max": REL_RMSE_MAX,
            "mean_abs_rel_max": MEAN_ABS_REL_MAX,
            "cosine_min": COSINE_MIN,
            "coverage_ratio_min": COVERAGE_RATIO_MIN,
            "coverage_abs_min": COVERAGE_ABS_MIN,
            "freeze": "S3_TODO: freeze with root per task.md 15.4 once S3-B1 and the transport "
            "alignment land",
        },
        "scenes": scenes,
    }

    out_json = os.path.join(d, "cachelighting.json")
    temp = out_json + ".tmp"
    with open(temp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(report, f, indent=2, sort_keys=True, allow_nan=False)
        f.write("\n")
    os.replace(temp, out_json)

    md = []
    md.append("# LumenGI S3 gate comparison: cache-lighting vs hit-lighting\n\n")
    md.append("Produced by tests/lumengi/compare_cachelighting.py; full numbers in cachelighting.json.\n\n")
    md.append("Overall mode: **%s** | scenes analyzed: %d\n\n" % (report["overall_mode"], len(scenes)))
    md.append("Thresholds are S3_TODO placeholders (freeze with root, task.md 15.4): "
              "relRMSE<=%.2f, meanAbs/ref<=%.2f, cosine>=%.2f, coverage ratio>=%.2f, "
              "coverage abs>=%.2f.\n\n" % (REL_RMSE_MAX, MEAN_ABS_REL_MAX, COSINE_MIN,
                                           COVERAGE_RATIO_MIN, COVERAGE_ABS_MIN))
    md.append("Transport caveat (S3_TODO): `cacheDirectRadiance` = DIRECT term, "
              "`diffuseRadianceHitDist` = one-bounce INDIRECT term. The metric machinery is "
              "transport-agnostic; root must fix which pair is the gate.\n\n")
    md.append("Relation to S1 (Agent L compare_reference2.py): same protocol shape - direction = RGB "
              "cosine on the joint mask, coverage = ratio + absolute vs the reference's own lit "
              "coverage, energy = context only; S3 adds per-pixel relRMSE / mean-abs as the literal "
              "'在容差内' numeric.\n\n")

    for tag in sorted(scenes):
        s = scenes[tag]
        md.append("## %s\n\n" % tag)
        inv = s["invariants"]
        md.append("ref (screen %s): finite=%s nonneg=%s | cache (atlas %s): finite=%s nonneg=%s\n\n" % (
            s["ref_shape"], inv["ref"]["finite"], inv["ref"]["nonnegative"],
            s["cache_shape"], inv["cache"]["finite"], inv["cache"]["nonnegative"]))
        dist_ref = s["distribution"]["ref_screen"]
        dist_cache = s["distribution"]["cache_atlas"]
        md.append("distribution (pre-flight, NOT gateable): ref coverage %.4f (mean %.4f) | "
                  "cache atlas coverage %.4f (mean %.4f)\n\n" % (
                      dist_ref["coverage"],
                      dist_ref["lit_energy"]["mean"] if dist_ref["lit_energy"] else 0.0,
                      dist_cache["coverage"],
                      dist_cache["lit_energy"]["mean"] if dist_cache["lit_energy"] else 0.0))
        if s["mode"] == "aligned":
            a = s["aligned"]
            v = s["verdict"]
            md.append("| metric | value | gate |\n|---|---|---|\n")
            md.append("| relRMSE | %.4f | <= %.2f %s |\n" % (
                a["rel_rmse"], REL_RMSE_MAX,
                "PASS" if a["rel_rmse"] <= REL_RMSE_MAX else "FAIL"))
            md.append("| mean abs / ref mean | %.4f | <= %.2f %s |\n" % (
                a["mean_abs_error_over_ref_mean"], MEAN_ABS_REL_MAX,
                "PASS" if a["mean_abs_error_over_ref_mean"] <= MEAN_ABS_REL_MAX else "FAIL"))
            md.append("| cosine mean | %.4f | >= %.2f %s |\n" % (
                a["cosine"]["mean"], COSINE_MIN,
                "PASS" if a["cosine"]["mean"] >= COSINE_MIN else "FAIL"))
            md.append("| joint cov / ref cov | %.4f | >= %.2f %s |\n" % (
                a["coverage_joint_over_ref"] or 0.0, COVERAGE_RATIO_MIN,
                "PASS" if (a["coverage_joint_over_ref"] or 0.0) >= COVERAGE_RATIO_MIN else "FAIL"))
            md.append("| joint coverage | %.4f | >= %.2f %s |\n" % (
                a["coverage_joint"], COVERAGE_ABS_MIN,
                "PASS" if a["coverage_joint"] >= COVERAGE_ABS_MIN else "FAIL"))
            md.append("\nVerdict (placeholder, S3_TODO): `%s` | joint pixels %d\n\n" % (
                "PASS" if v["pass_placeholder"] else "FAIL", a["joint_pixels"]))
        else:
            md.append("Verdict: **not gateable** (S3_TODO resample absent); distribution is pre-flight "
                      "only. If cache atlas coverage is ~0 while ref coverage is healthy, the capture "
                      "itself is broken (S3-B1 not wired / atlas never lit).\n\n")

    with open(os.path.join(d, "cachelighting_report.md"), "w", encoding="utf-8") as f:
        f.write("".join(md))

    n_aligned = sum(1 for s in scenes.values() if s.get("mode") == "aligned")
    print("CACHECMP wrote", os.path.abspath(out_json))
    print("CACHECMP scenes", len(scenes), "| aligned", n_aligned, "| overall_mode", report["overall_mode"])


if __name__ == "__main__":
    main()
