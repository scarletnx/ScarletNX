#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Measure offline compilation strategies using snx-bench-jit-latency.

Inputs are private snapshots, never game data committed to this repository.
Screening uses available CPUs in parallel. Finalists are measured serially in
random order so contended screening times cannot choose the production setting.
All guest compilation remains at O0. The replay never executes guest code.
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import random
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--aot", type=pathlib.Path, required=True)
    parser.add_argument("--roots", type=pathlib.Path, required=True)
    parser.add_argument("--validation-roots", type=pathlib.Path,
                        help="broader entry stream to check later-miss and total-work costs")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--compact", action="store_true",
                        help="compare the current adaptive setting, strict small regions and the previous setting")
    args = parser.parse_args()
    if args.rounds < 3:
        parser.error("at least three finalist rounds are required")
    args.out.mkdir(parents=True, exist_ok=True)
    log = args.out / "progress.log"
    cpus = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count() or 1
    jobs = min(cpus, args.jobs if args.jobs is not None else cpus)
    if jobs < 1:
        parser.error("--jobs must be positive")

    def say(message):
        print(message, flush=True)
        with log.open("a") as stream:
            stream.write(message + "\n")

    runs = []

    def measure(config, label, cache=None, roots=None):
        budget, batch, threads, mode, region = config
        command = [str(args.bench.resolve()), str(args.manifest.resolve()), str(args.aot.resolve()),
                   str((roots or args.roots).resolve()), str(budget), str(batch), str(threads), mode, str(region)]
        if cache is not None:
            command.append(str(cache.resolve()))
        started = time.time()
        result = subprocess.run(command, text=True, capture_output=True, timeout=180)
        if result.returncode:
            raise RuntimeError(f"{config}: exit {result.returncode}: {result.stderr[-2000:]}")
        row = json.loads(result.stdout)
        if row["missing"]:
            raise RuntimeError(f"{config}: failed to compile a requested entry")
        row.update(config=config, label=label, started=started, finished=time.time())
        return row

    say(f"CPU compiler tuner: {cpus} available CPUs, {jobs} parallel screening processes, O0 only.")
    budgets = [1024, 4096, 20000]
    # One active demand compile per replay: worker threads wait for work, rather than multiplying the CPU budget.
    adaptive = (64, 256, 4, "demand", "auto")
    baseline = (20000, 256, 4, "demand", 0)
    screen = [(64, 256, 1, "demand", 64), adaptive] if args.compact else [
        (budget, 256, 1, "demand", region)
        for budget in budgets for region in (64, 256, 512, 1024, 2048, 4096)] + [adaptive]
    say("Parallel screening checks all strategies; screening times are excluded from finalist ranking.")
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(jobs, len(screen))) as pool:
        for row in pool.map(lambda config: measure(config, "screen"), screen):
            runs.append(row)
            say(f"Checked budget {row['config'][0]}, region {row['config'][4]}: {row['instructions']} instructions, "
                f"{row['functions']} functions, all entries compiled")

    # Regions leave their external branches and fallthroughs on dispatch. A short first wait alone is insufficient:
    # the optional broader entry stream below checks the cost of further demand compiles.
    finalists = screen + [baseline]
    rng = random.Random(7950)
    for round_ in range(args.rounds):
        order = finalists[:]
        rng.shuffle(order)
        for config in order:
            row = measure(config, f"final-{round_}")
            runs.append(row)
            say(f"Round {round_ + 1}: budget={config[0]} region={config[4]} workers={config[2]}, "
                f"waits={[round(x, 3) for x in row['waits_ms']]} ms, total={row['elapsed_ms']:.3f} ms")
            (args.out / "runs.json").write_text(json.dumps(runs, indent=2))

    ranking = []
    for config in finalists:
        rows = [row for row in runs if tuple(row["config"]) == config and row["label"].startswith("final-")]
        maxima = [max(row["waits_ms"]) for row in rows]
        ranking.append({"config": config, "median_max_wait_ms": statistics.median(maxima),
                        "max_wait_range_ms": [min(maxima), max(maxima)],
                        "median_total_ms": statistics.median(row["elapsed_ms"] for row in rows),
                        "median_code_bytes": statistics.median(row["code_bytes"] for row in rows)})
    ranking.sort(key=lambda row: row["median_max_wait_ms"])
    say("Measured ranking by median worst blocking wait:")
    for row in ranking:
        say(f"  {row['config']}: {row['median_max_wait_ms']:.3f} ms, total {row['median_total_ms']:.3f} ms")

    validation = []
    if args.validation_roots:
        # Include a moderate-size region even when a tiny one wins the first two misses.
        choices = {tuple(row["config"]) for row in ranking[:3]}
        choices.update({adaptive, baseline})
        if not args.compact:
            choices.add((4096, 256, 1, "demand", 1024))
        for round_ in range(args.rounds):
            order = sorted(choices, key=str)
            rng.shuffle(order)
            for config in order:
                row = measure(config, f"coverage-{round_}", roots=args.validation_roots)
                validation.append(row)
                say(f"Coverage round {round_ + 1}, {config}: total {row['elapsed_ms']:.3f} ms, "
                    f"worst {max(row['waits_ms']):.3f} ms, {row['instructions']} instructions")
                (args.out / "validation-runs.json").write_text(json.dumps(validation, indent=2))

    # Cache reuse is measured independently from fresh compilation. Each writer has its own private file.
    cache_runs = []
    for name, config in [("baseline", baseline), ("adaptive", adaptive), ("latency", tuple(ranking[0]["config"]))]:
        cache = args.out / f"{name}.jitcache"
        cache.unlink(missing_ok=True)
        cache_runs.append(measure(config, name + "-cache-write", cache))
        for i in range(args.rounds):
            row = measure(config, name + "-cache-read", cache)
            cache_runs.append(row)
            say(f"Cache reuse {name} run {i + 1}: {row['elapsed_ms']:.3f} ms, "
                f"{row['cached']} cached units, {row['batches']} fresh batches")

    result = {"available_cpus": cpus, "screen_jobs": jobs, "ranking": ranking, "runs": runs,
              "cache_runs": cache_runs, "validation_runs": validation,
              "limitations": ["Offline cold-code replay is not a gameplay frame-time measurement.",
                              "Snapshot discovery lacks the original run's exact pending and compiled JIT sets.",
                              "Low discovery budgets can increase later misses; validate broader coverage before deployment.",
                              "No guest instructions were executed, so architectural tests remain required."]}
    (args.out / "results.json").write_text(json.dumps(result, indent=2))
    say("Tuning finished. Measurements and limitations saved to results.json.")


if __name__ == "__main__":
    main()
