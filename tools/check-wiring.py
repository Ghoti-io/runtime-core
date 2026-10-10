#!/usr/bin/env python3
"""Fail if a gate exists but `make test` would not run it.

check-gates.sh proves each gate script fails on its planted defect. It cannot
see the Makefile: a gate that is dropped from TEST_GATES, or whose target no
longer calls its script, is a gate that has stopped running, and it looks
exactly like a gate that passes. So this reads the Makefile as text and asks,
for each required gate, that

  - it is named in TEST_GATES, which `test` depends on;
  - it has a target;
  - its recipe runs its script (check-edges: both modes).

Usage: check-wiring.py [Makefile]
"""

import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
path = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "Makefile"

# gate -> what its recipe must contain
REQUIRED = {
    "check-symbols": ["tools/check-symbols.sh"],
    "check-aliasing": ["-Wstrict-aliasing"],
    "check-stamps": ["tools/check-stamps.py"],
    "check-fp-contract": ["tools/check-fp-contract.sh"],
    "check-labels": ["tools/check-labels.sh"],
    "check-direction": ["tools/check-direction.sh"],
    "check-edges": ["tools/check-edges.sh --includes", "tools/check-edges.sh --links"],
    "check-gates": ["tools/check-gates.sh"],
    "check-version": ["tools/check-version.sh"],
    "check-wiring": ["tools/check-wiring.py"],
    "check-hook": ["tools/check-hook.sh"],
}

problems = []
text = re.sub(r"\\\n", " ", path.read_text())

m = re.search(r"^TEST_GATES\s*\??=([^\n]*)$", text, re.M)
if not m:
    print("check-wiring: no TEST_GATES in %s; this gate is measuring nothing" % path,
          file=sys.stderr)
    sys.exit(1)
listed = m.group(1).split()
if not re.search(r"^test:[^\n]*\$\(TEST_GATES\)", text, re.M):
    problems.append("the test target does not depend on $(TEST_GATES), so no gate runs")

for gate, needles in REQUIRED.items():
    if gate not in listed:
        problems.append("%s is not in TEST_GATES, so `make test` never runs it" % gate)
    # The recipe: the tab lines after the target line, comments and conditional
    # directives allowed between.
    rules = re.findall(
        r"^%s:[^\n]*\n((?:(?:#[^\n]*|[ \t]*|if(?:n?eq|n?def)[^\n]*|else[^\n]*|endif[^\n]*)\n|\t[^\n]*\n)*)" % re.escape(gate),
        text, re.M)
    if not rules:
        problems.append("%s has no target" % gate)
        continue
    body = "\n".join(rules)
    for needle in needles:
        if needle not in body:
            problems.append("the %s recipe never runs %s" % (gate, needle))

if problems:
    for problem in problems:
        print("check-wiring: %s" % problem, file=sys.stderr)
    sys.exit(1)
print("check-wiring: %d gates are in TEST_GATES, have a target and run their script"
      % len(REQUIRED))
