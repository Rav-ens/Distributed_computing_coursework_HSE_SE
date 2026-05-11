#!/usr/bin/env python3
import json
import math
import random
import sys


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "map"
    if cmd != "map":
        print(json.dumps({"error": f"unsupported command: {cmd}"}), file=sys.stderr)
        return 1

    raw = sys.stdin.read().strip()
    payload = {}
    if raw:
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            payload = {"raw": raw}

    samples = int(payload.get("samples", payload.get("n", 100000)))
    seed = int(payload.get("seed", 42))
    if samples <= 0:
        print(json.dumps({"error": "samples must be > 0"}), file=sys.stderr)
        return 1

    random.seed(seed)
    inside = 0
    for _ in range(samples):
        x = random.random()
        y = random.random()
        if x * x + y * y <= 1.0:
            inside += 1

    pi_estimate = 4.0 * inside / samples
    out = {
        "ok": True,
        "algorithm": "monte_carlo",
        "samples": samples,
        "inside_circle": inside,
        "pi_estimate": pi_estimate,
        "abs_error": abs(math.pi - pi_estimate),
    }
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
