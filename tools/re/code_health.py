# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""How readable the co-op runtime's functions are: complexity, size and nesting per function.

Runs clang-tidy's readability-function-cognitive-complexity and readability-function-size on the
co-op sources (emulator/src/core/bloodborne_*.cpp) with every threshold at 0, so every function is
reported, and reads its output into one row per function:

  cognitive   cognitive complexity (clang-tidy; 0 when it reports none)
  lines       lines including whitespace and comments
  statements, branches, parameters
  nesting     the deepest block inside the body: 0 straight-line code, 1 an `if {}` in the body,
              2 a loop in that `if`, ... clang-tidy only notes where nesting first goes over its
              NestingThreshold ("nesting level N starts here", the body being level 1), so it
              runs again with NestingThreshold 1, 2, ... until no function goes over: a
              function's nesting is the highest threshold it went over.

Lambdas are rows of their own (clang-tidy reports their complexity only), named after the
function they sit in: `RunAutoplayStep::lambda#2`.

  code_health.py -p <build dir>                      summary per file
  code_health.py -p <build dir> --write-baseline     also writes code_health_baseline.json
  code_health.py -p <build dir> --check              CI: fails when the code got worse
  code_health.py --from-output <clang-tidy output>   reads a saved run instead of running it

<build dir> holds a compile_commands.json (the port: `ninja -C out/gpu -t compdb >
out/compile_commands.json` in bbport). --check fails when a function that is not in the baseline
goes over a limit (cognitive 25, nesting 4, 120 lines), or a function that is goes over a limit
and got worse on it than its baseline. Functions are matched by file and name, then by name alone
(code moved to another file keeps its baseline).
"""
import argparse
import concurrent.futures
import json
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
from dataclasses import asdict, dataclass

REPO = pathlib.Path(__file__).resolve().parents[2]
BASELINE = pathlib.Path(__file__).resolve().parent / "code_health_baseline.json"
SOURCES = "emulator/src/core/bloodborne_*.cpp"
# Diagnostics in these files are kept (the sources and the co-op headers they include).
OWN_FILES = re.compile(r"^emulator/src/core/bloodborne_[\w]+\.(cpp|h)$")
CLANG_TIDY = shutil.which("clang-tidy") or r"C:\msys64\clang64\bin\clang-tidy.exe"
CHECKS = "-*,readability-function-cognitive-complexity,readability-function-size"
CONFIG = ("{CheckOptions: {"
          "readability-function-cognitive-complexity.Threshold: 0, "
          "readability-function-cognitive-complexity.DescribeBasicIncrements: false, "
          "readability-function-size.LineThreshold: 0, "
          "readability-function-size.StatementThreshold: 0, "
          "readability-function-size.BranchThreshold: 0, "
          "readability-function-size.ParameterThreshold: 0, "
          "readability-function-size.NestingThreshold: 0}}")
# The extra runs for nesting: only NestingThreshold can be exceeded.
NESTING_CONFIG = ("{{CheckOptions: {{"
                  "readability-function-size.LineThreshold: 100000000, "
                  "readability-function-size.StatementThreshold: 100000000, "
                  "readability-function-size.BranchThreshold: 100000000, "
                  "readability-function-size.ParameterThreshold: 100000000, "
                  "readability-function-size.NestingThreshold: {threshold}}}}}")
LIMITS = {"cognitive": 25, "nesting": 4, "lines": 120}
METRICS = ("cognitive", "lines", "statements", "branches", "parameters", "nesting")

DIAGNOSTIC = re.compile(r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): (?P<kind>warning|note): "
                        r"(?P<message>.*?)(?: \[(?P<check>[\w.,-]+)\])?$")
SIZE = re.compile(r"^function '(?P<name>.+)' exceeds recommended size/complexity thresholds$")
COGNITIVE = re.compile(r"^function '(?P<name>.+)' has cognitive complexity of (?P<value>\d+) ")
LAMBDA = re.compile(r"^lambda has cognitive complexity of (?P<value>\d+) ")
NOTES = {
    "lines": re.compile(r"^(\d+) lines including whitespace and comments "),
    "statements": re.compile(r"^(\d+) statements "),
    "branches": re.compile(r"^(\d+) branches "),
    "parameters": re.compile(r"^(\d+) parameters "),
}
NESTING = re.compile(r"^nesting level (\d+) starts here ")


@dataclass
class Function:
    file: str
    name: str
    line: int = 0
    cognitive: int = 0
    lines: int = 0
    statements: int = 0
    branches: int = 0
    parameters: int = 0
    nesting: int = 0

    def key(self):
        return (self.file, self.name)

    def metrics(self):
        return {m: getattr(self, m) for m in METRICS}


def relative(path: str, root: pathlib.Path) -> str:
    """`path` (as clang-tidy printed it, maybe with `..`) relative to the repo, with `/`."""
    resolved = pathlib.Path(path).resolve()
    try:
        return resolved.relative_to(root.resolve()).as_posix()
    except ValueError:
        return resolved.as_posix()


def parse(output: str, root: pathlib.Path = REPO, keep=OWN_FILES.match) -> list:
    """clang-tidy's output (one or more runs) as one Function per function, in file order.

    A function in a header that several runs include is kept once. Same-named functions in one
    file (overloads) get `#2`, `#3`, ... in the order they appear.
    """
    functions = {}  # (file, line, col) -> Function
    lambdas = []  # (file, line, col, cognitive)
    current = None
    nested = None  # (function, level) of the last nesting note, until we know it isn't a macro's
    for text in output.splitlines():
        match = DIAGNOSTIC.match(text.strip())
        if not match:
            continue
        if nested:
            # A block that a macro expands to (LOG_INFO's `do { if (...) {` ...) is followed by
            # "expanded from macro": not nesting anyone wrote.
            if not match["message"].startswith("expanded from macro"):
                nested[0].nesting = max(nested[0].nesting, nested[1] - 1)
            nested = None
        file = relative(match["path"], root)
        if not keep(file):
            if match["kind"] == "warning":
                current = None
            continue
        where = (file, int(match["line"]), int(match["col"]))
        message = match["message"]
        if match["kind"] == "warning":
            current = None
            if size := SIZE.match(message):
                current = functions.setdefault(where, Function(file, size["name"], where[1]))
            elif cognitive := COGNITIVE.match(message):
                function = functions.setdefault(where, Function(file, cognitive["name"], where[1]))
                function.cognitive = int(cognitive["value"])
            elif lam := LAMBDA.match(message):
                lambdas.append((*where, int(lam["value"])))
            continue
        if current is None or file != current.file:
            continue
        for metric, pattern in NOTES.items():
            if note := pattern.match(message):
                setattr(current, metric, int(note[1]))
        if nesting := NESTING.match(message):
            nested = (current, int(nesting[1]))
    if nested:
        nested[0].nesting = max(nested[0].nesting, nested[1] - 1)

    rows = sorted(functions.values(), key=lambda f: (f.file, f.line))
    count = {}
    for row in rows:
        count[row.key()] = count.get(row.key(), 0) + 1
        if count[row.key()] > 1:
            row.name = f"{row.name}#{count[row.key()]}"
    seen_lambdas = set()
    for file, line, col, cognitive in sorted(set(lambdas)):
        if (file, line, col) in seen_lambdas:
            continue
        seen_lambdas.add((file, line, col))
        outer = [f for f in rows if f.file == file and f.lines and
                 f.line <= line < f.line + f.lines]
        owner = (max(outer, key=lambda f: f.line).name + "::") if outer else ""
        index = sum(1 for r in rows if r.file == file and r.name.startswith(owner + "lambda#"))
        rows.append(Function(file, f"{owner}lambda#{index + 1}", line, cognitive=cognitive))
    return sorted(rows, key=lambda f: (f.file, f.line))


def run_clang_tidy(build_dir: pathlib.Path, sources: list, clang_tidy: str, jobs: int) -> str:
    """Every report on `sources`: one run at threshold 0, then the nesting runs."""
    def tidy(source, checks, config):
        result = subprocess.run(
            [clang_tidy, "-p", str(build_dir), f"--checks={checks}", f"--config={config}",
             "--quiet", str(source)], capture_output=True, text=True, errors="replace")
        if "error:" in result.stdout or (result.returncode != 0 and not result.stdout):
            sys.stderr.write(f"clang-tidy failed on {source}:\n{result.stdout[-2000:]}"
                             f"{result.stderr[-2000:]}\n")
            raise SystemExit(2)
        return result.stdout

    def nesting(source, threshold):
        return tidy(source, "-*,readability-function-size",
                    NESTING_CONFIG.format(threshold=threshold))

    first = 8  # thresholds run at once; deeper ones one by one while something still nests
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        runs = {source: [pool.submit(tidy, source, CHECKS, CONFIG)] +
                [pool.submit(nesting, source, t) for t in range(1, first + 1)]
                for source in sources}
        outputs = []
        for source, futures in runs.items():
            outputs += [future.result() for future in futures]
            threshold = first
            while "nesting level" in outputs[-1]:
                threshold += 1
                outputs.append(nesting(source, threshold))
    return "\n".join(outputs)


def compiled_sources(build_dir: pathlib.Path, root: pathlib.Path) -> list:
    """The co-op sources the compile database builds."""
    database = json.loads((build_dir / "compile_commands.json").read_text())
    built = {pathlib.Path(entry["file"]).resolve() for entry in database}
    return [path for path in sorted(root.glob(SOURCES)) if path.resolve() in built]


def percentile(values: list, fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0
    return ordered[min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))]


def summary(rows: list) -> str:
    """Per file: functions, median/p90 cognitive, functions over 25/50/100, the longest one."""
    lines = [f"{'file':<44}{'funcs':>6}{'med':>5}{'p90':>5}{'>25':>5}{'>50':>5}{'>100':>5}"
             f"  longest"]
    by_file = {}
    for row in rows:
        by_file.setdefault(row.file, []).append(row)
    for file, group in [*sorted(by_file.items()), ("total", rows)]:
        cognitive = [r.cognitive for r in group]
        longest = max(group, key=lambda r: r.lines)
        lines.append(f"{file.rsplit('/', 1)[-1]:<44}{len(group):>6}"
                     f"{statistics.median(cognitive):>5g}{percentile(cognitive, 0.9):>5g}"
                     f"{sum(c > 25 for c in cognitive):>5}{sum(c > 50 for c in cognitive):>5}"
                     f"{sum(c > 100 for c in cognitive):>5}  {longest.name} ({longest.lines})")
    total_lines = sum(r.lines for r in rows) or 1
    over = sum(r.lines for r in rows if r.cognitive > LIMITS["cognitive"])
    lines.append(f"{sum(r.cognitive > LIMITS['cognitive'] for r in rows)} functions over cognitive "
                 f"{LIMITS['cognitive']} hold {100 * over / total_lines:.0f}% of the function lines;"
                 f" {sum(r.nesting > LIMITS['nesting'] for r in rows)} nest deeper than "
                 f"{LIMITS['nesting']}, {sum(r.lines > LIMITS['lines'] for r in rows)} are over "
                 f"{LIMITS['lines']} lines")
    return "\n".join(lines)


def worst(rows: list, count: int) -> str:
    ranked = sorted(rows, key=lambda r: -r.cognitive)[:count]
    return "\n".join(f"{r.cognitive:>5} cog {r.lines:>5} lines nest {r.nesting}  {r.name} "
                     f"({r.file.rsplit('/', 1)[-1]}:{r.line})" for r in ranked)


def baseline_json(rows: list) -> dict:
    return {
        "about": "code_health.py --write-baseline: per co-op function, see tools/re/code_health.py",
        "limits": LIMITS,
        "functions": [{"file": r.file, "name": r.name, **r.metrics()} for r in rows],
    }


def check(rows: list, baseline: dict, limits: dict = LIMITS) -> list:
    """Problems that fail CI: a new function over a limit, or a known one over a limit and worse
    on it than its baseline."""
    known = {(b["file"], b["name"]): b for b in baseline["functions"]}
    by_name = {}
    for entry in baseline["functions"]:
        by_name.setdefault(entry["name"], []).append(entry)
    problems = []
    for row in rows:
        base = known.get(row.key())
        if base is None and len(by_name.get(row.name, [])) == 1:
            base = by_name[row.name][0]  # moved to another file
        where = f"{row.name} ({row.file}:{row.line})"
        for metric, limit in limits.items():
            value = getattr(row, metric)
            if value <= limit:
                continue
            if base is None:
                problems.append(f"new function {where}: {metric} {value} is over {limit}")
            elif value > base.get(metric, 0):
                problems.append(f"{where}: {metric} went from {base.get(metric, 0)} to {value} "
                                f"(over {limit})")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--build-dir", type=pathlib.Path,
                        help="directory with compile_commands.json")
    parser.add_argument("--from-output", type=pathlib.Path, nargs="+",
                        help="saved clang-tidy output instead of running it")
    parser.add_argument("--save-output", type=pathlib.Path, help="keep clang-tidy's raw output")
    parser.add_argument("--clang-tidy", default=CLANG_TIDY)
    parser.add_argument("--root", type=pathlib.Path, default=REPO, help="the repo")
    parser.add_argument("--baseline", type=pathlib.Path, default=BASELINE)
    parser.add_argument("--write-baseline", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", type=pathlib.Path, help="every row as JSON")
    parser.add_argument("--worst", type=int, default=15, help="list the N most complex")
    parser.add_argument("-j", "--jobs", type=int, default=8)
    args = parser.parse_args(argv)

    if args.from_output:
        output = "\n".join(p.read_text(errors="replace") for p in args.from_output)
    elif args.build_dir:
        sources = compiled_sources(args.build_dir, args.root)
        if not sources:
            print(f"no co-op sources in {args.build_dir / 'compile_commands.json'}")
            return 2
        output = run_clang_tidy(args.build_dir, sources, args.clang_tidy, args.jobs)
    else:
        parser.error("give -p <build dir> or --from-output")
    if args.save_output:
        args.save_output.write_text(output)
    rows = parse(output, args.root)
    print(summary(rows))
    if args.worst:
        print(f"\nmost complex:\n{worst(rows, args.worst)}")
    if args.json:
        args.json.write_text(json.dumps([asdict(r) for r in rows], indent=1) + "\n")
    if args.write_baseline:
        data = baseline_json(rows)
        # One function per line: a change to the baseline reads as the functions that changed.
        functions = ",\n".join("  " + json.dumps(f) for f in data["functions"])
        text = (f'{{\n "about": {json.dumps(data["about"])},\n "limits": '
                f'{json.dumps(data["limits"])},\n "functions": [\n{functions}\n ]\n}}\n')
        args.baseline.write_text(text, newline="\n")
        print(f"\nwrote {args.baseline}")
    if args.check:
        problems = check(rows, json.loads(args.baseline.read_text()))
        for problem in problems:
            print(f"CODE HEALTH: {problem}")
        print(f"\ncode health: {'FAILED' if problems else 'ok'} ({len(rows)} functions, limits "
              f"cognitive {LIMITS['cognitive']}, nesting {LIMITS['nesting']}, "
              f"{LIMITS['lines']} lines)")
        return 1 if problems else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
