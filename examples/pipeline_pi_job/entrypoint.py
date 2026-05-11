#!/usr/bin/env python3
"""Демо пайплайна split → map → reduce для координатора (учебный образ)."""
import json
import math
import random
import sys


def monte_carlo_pi(samples: int, seed: int) -> dict:
    random.seed(seed)
    inside = 0
    for _ in range(samples):
        x = random.random()
        y = random.random()
        if x * x + y * y <= 1.0:
            inside += 1
    estimate = 4.0 * inside / samples
    return {
        "ok": True,
        "algorithm": "monte_carlo",
        "samples": samples,
        "inside_circle": inside,
        "pi_estimate": estimate,
        "abs_error": abs(math.pi - estimate),
    }


def cmd_split(inp: dict) -> dict:
    """stdin: {\"input\": ...} от координатора."""
    big = inp.get("input") or {}
    chunks = int(big.get("chunks", big.get("n_chunks", 4)))
    samples = int(big.get("samples_per_chunk", big.get("samples", 200_000)))
    base_seed = int(big.get("base_seed", 1))
    if chunks <= 0 or chunks > 500:
        raise ValueError("chunks must be in 1..500")
    if samples <= 0:
        raise ValueError("samples_per_chunk must be > 0")
    subtasks = []
    for i in range(chunks):
        payload = {"samples": samples, "seed": base_seed + i}
        subtasks.append({"payload": payload})
    return {"subtasks": subtasks}


def cmd_map(payload: dict) -> dict:
    """stdin: один payload подзадачи (воркер; поле docker_image игнорируется)."""
    clean = {k: v for k, v in payload.items() if k != "docker_image"}
    samples = int(clean.get("samples", clean.get("n", 100_000)))
    seed = int(clean.get("seed", 42))
    if samples <= 0:
        raise ValueError("samples must be > 0")
    return monte_carlo_pi(samples, seed)


def cmd_reduce(inp: dict) -> dict:
    """stdin: {\"input\": ..., \"results\": [{\"task_id\", \"result\"}, ...]}."""
    parts = inp.get("results") or []
    if not parts:
        return {"ok": False, "error": "no results"}
    total_samples = 0
    weighted = 0.0
    for p in parts:
        r = p.get("result") or {}
        s = int(r.get("samples", 0))
        est = float(r.get("pi_estimate", 0.0))
        total_samples += s
        weighted += est * s
    if total_samples <= 0:
        return {"ok": False, "error": "zero total samples"}
    combined = weighted / total_samples
    return {
        "ok": True,
        "pi_estimate_combined": combined,
        "total_samples": total_samples,
        "chunks": len(parts),
        "abs_error": abs(math.pi - combined),
    }


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "map"
    raw = sys.stdin.read().strip()
    data = {}
    if raw:
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            print(json.dumps({"error": "stdin is not JSON"}), file=sys.stderr)
            return 1

    try:
        if cmd == "split":
            out = cmd_split(data)
        elif cmd == "map":
            out = cmd_map(data)
        elif cmd == "reduce":
            out = cmd_reduce(data)
        else:
            print(json.dumps({"error": f"unsupported command: {cmd}"}), file=sys.stderr)
            return 1
    except (ValueError, TypeError, KeyError) as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 1

    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
