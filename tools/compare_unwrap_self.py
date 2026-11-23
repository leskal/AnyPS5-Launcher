"""Compare the launcher's C++ SELF unwrapper with AnyPS5's tools/unwrap_self.py.

Developer tool, not needed to use the launcher. It collects the containers AnyPS5's
tools/tests/test_unwrap_self.py builds, adds mutated variants, runs both
implementations on every one and fails on any difference: the ELF bytes must be
identical, and refusals must carry the same message.

    cmake --build build/debug --target unwrap_self_cli
    python3 tools/compare_unwrap_self.py --anyps5 C:/dev/AnyPS5 --cli build/debug/unwrap_self_cli.exe

Run it after AnyPS5 changes tools/unwrap_self.py, and port the change to
src/core/SelfUnwrap.cpp until it passes.
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

parser = argparse.ArgumentParser(description="compare the C++ SELF unwrapper with AnyPS5's unwrap_self.py")
parser.add_argument("--anyps5", required=True, help="AnyPS5 checkout")
parser.add_argument("--cli", required=True, help="path to the unwrap_self_cli build")
parser.add_argument("--mutants", type=int, default=300, help="mutated variants per seed container")
parser.add_argument("--seed", type=int, default=1234, help="random seed for the mutations")
arguments = parser.parse_args()
ANYPS5 = Path(arguments.anyps5)
CLI = arguments.cli
MUTANTS_PER_SEED = arguments.mutants
sys.path[:0] = [str(ANYPS5 / "tools" / "tests"), str(ANYPS5 / "tools")]
import unwrap_self  # noqa: E402

work = Path(tempfile.mkdtemp(prefix="unwrap-diff-"))
corpus = []
original = unwrap_self.unwrap


def capture(path, output, verbose=False):
    try:
        corpus.append(Path(path).read_bytes())
    except OSError:
        pass
    return original(path, output, verbose)


unwrap_self.unwrap = capture
import test_unwrap_self  # noqa: E402
suite = unittest.defaultTestLoader.loadTestsFromModule(test_unwrap_self)
result = unittest.TextTestRunner(stream=open(os.devnull, "w"), verbosity=0).run(suite)
unwrap_self.unwrap = original
print(f"AnyPS5 unit tests: {result.testsRun} run, {len(result.failures)} failures, {len(result.errors)} errors")
seeds = []
seen = set()
for data in corpus:
    if data not in seen and len(data) >= 0x400:
        seen.add(data)
        seeds.append(data)
print(f"captured {len(corpus)} inputs, {len(seeds)} distinct seeds")

rng = random.Random(arguments.seed)
INTERESTING = [0, 1, 2, 7, 8, 0x10, 0x38, 0x40, 0x350, 0x400, 0x800, 0x1000, 0x4000, 0x10000, 0x7FFF0000,
               0x80000000, 0xFFFFFFFF, 1 << 32, 1 << 63, (1 << 64) - 1, (1 << 64) - 0x1000]


def mutate(data):
    data = bytearray(data)
    inner = data.find(b"\x7fELF")
    for _ in range(rng.randint(1, 4)):
        choice = rng.random()
        if choice < 0.3:
            offset = 0x40 + rng.randrange(28) * 8
        elif choice < 0.7 and inner >= 0:
            program = inner + struct.unpack_from("<Q", data, inner + 0x20)[0] if inner + 0x28 <= len(data) else inner
            count = struct.unpack_from("<H", data, inner + 0x38)[0] if inner + 0x3A <= len(data) else 1
            offset = program + rng.randrange(max(count, 1)) * 56 + rng.choice([0, 4, 8, 16, 24, 32, 40, 48])
        elif choice < 0.8 and inner >= 0:
            offset = inner + rng.choice([4, 5, 6, 0x10, 0x12, 0x14, 0x20, 0x34, 0x36, 0x38])
        else:
            offset = rng.randrange(len(data))
        if offset >= len(data):
            continue
        kind = rng.random()
        if kind < 0.45:
            width = rng.choice([1, 2, 4, 8])
            value = rng.choice(INTERESTING)
            if rng.random() < 0.5:
                value = rng.randrange(0, 0x20000)
            value &= (1 << (width * 8)) - 1
            raw = value.to_bytes(width, "little")
        elif kind < 0.75:
            raw = bytes([data[offset] ^ (1 << rng.randrange(8))])
        elif kind < 0.85 and offset + 8 <= len(data):
            old = int.from_bytes(data[offset:offset + 8], "little")
            raw = ((old + rng.choice([-0x1000, -0x10, -8, -1, 1, 8, 0x10, 0x1000])) % (1 << 64)).to_bytes(8, "little")
        else:
            raw = os.urandom(rng.randint(1, 64))
        data[offset:offset + len(raw)] = raw[:max(0, len(data) - offset)]
    if rng.random() < 0.05:
        data = data[:rng.randrange(0x400, len(data) + 1)]
    if rng.random() < 0.05:
        data += os.urandom(rng.randint(1, 0x2000))
    return bytes(data)


cases = list(seeds)
for seed in seeds:
    for _ in range(MUTANTS_PER_SEED):
        cases.append(mutate(seed))
# Encrypted-looking loads and random containers exercise the entropy check.
for seed in seeds[:20]:
    data = bytearray(seed)
    if len(data) <= 0x800:
        continue
    data[0x800:] = os.urandom(len(data) - 0x800)
    cases.append(bytes(data))
for size in (0x3FF, 0x400, 0x1000, 0x20000):
    cases.append(struct.pack("<I", 0x1D3D154F) + os.urandom(size - 4))
print(f"{len(cases)} cases")

inputs = work / "in"
outputs = work / "out"
inputs.mkdir()
outputs.mkdir()
paths = []
for index, data in enumerate(cases):
    path = inputs / f"{index}.self"
    path.write_bytes(data)
    paths.append(path)
(work / "list.txt").write_text("\n".join(str(path) for path in paths) + "\n")
cpp = subprocess.run([CLI, "--batch", str(work / "list.txt"), str(outputs)], capture_output=True, text=True)
if cpp.returncode != 0 or cpp.stderr.strip():
    print("C++ batch failed:", cpp.returncode, cpp.stderr[-3000:])
    sys.exit(1)
lines = cpp.stdout.splitlines()
assert len(lines) == len(cases), (len(lines), len(cases))

stats = {"ok": 0, "refused": 0, "py-error": 0}
mismatches = []
for index, (path, line) in enumerate(zip(paths, lines)):
    target = work / "py.elf"
    if target.exists():
        target.unlink()
    try:
        original(str(path), str(target))
        python = ("OK", target.read_bytes())
    except unwrap_self.Refused as error:
        python = ("REFUSED", str(error))
    except Exception as error:  # noqa: BLE001 - the script lets these escape as tracebacks
        python = ("PYERROR", f"{type(error).__name__}: {error}")
    if python[0] == "OK":
        produced = (outputs / f"{index}.elf").read_bytes() if line.startswith("OK ") else None
        if produced != python[1]:
            mismatches.append((index, "python OK", line))
        else:
            stats["ok"] += 1
    elif python[0] == "REFUSED":
        if line != "REFUSED: " + python[1]:
            mismatches.append((index, "python REFUSED: " + python[1], line))
        else:
            stats["refused"] += 1
    else:
        if line.startswith("OK "):
            mismatches.append((index, "python " + python[1], line))
        else:
            stats["py-error"] += 1
            print(f"  case {index}: Python raised {python[1]!r}; C++ said {line!r}")
print("matched:", stats)
print("mismatches:", len(mismatches))
for item in mismatches[:25]:
    print("  ", item)
if mismatches:
    keep = Path("unwrap-mismatches")
    keep.mkdir(exist_ok=True)
    for index, *_ in mismatches[:25]:
        shutil.copy(paths[index], keep / f"{index}.self")
    print(f"the first mismatching inputs were copied to {keep.resolve()}")
shutil.rmtree(work)
sys.exit(1 if mismatches else 0)
