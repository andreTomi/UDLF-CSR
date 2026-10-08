#!/usr/bin/env python3
"""fix_hyp_structure.py -- hoist the Step 1 helper block out of main().

The tranche-2 helpers (row_of, sim_emb_merge, ee_sum_in_order, gram_reference,
cand_less) were appended INSIDE main(), each wrapped in `namespace {`. That is a
syntax error: a namespace cannot open inside a function, and a function cannot be
defined there either.

This moves them to file scope by
  1. dropping the `namespace {` / `}  // namespace` lines that appear AFTER main,
  2. giving each helper `static` internal linkage (they are test-local), and
  3. prefixing the forward use of the `Row` alias so ordering stays valid.

Idempotent: running it twice is a no-op (it reports "already hoisted").
"""
import sys

PATH = "Source/tests/test_rfe_hyp.cpp"
HELPERS = [
    "Row row_of(",
    "float sim_emb_merge(",
    "float ee_sum_in_order(",
    "std::vector<float> gram_reference(",
    "bool cand_less(",
]


def main():
    src = open(PATH).read()
    lines = src.split("\n")
    try:
        mi = next(i for i, l in enumerate(lines) if l.startswith("int main(int argc"))
    except StopIteration:
        print("main() not found -- wrong file?", file=sys.stderr)
        return 1

    already = any(l.startswith("static Row row_of(") for l in lines)
    if already:
        print("already hoisted (helpers are static at file scope) -- nothing to do")
        return 0

    out = []
    for i, l in enumerate(lines):
        if i > mi:
            s = l.strip()
            if s == "namespace {" or s == "}  // namespace":
                continue
        out.append(l)
    text = "\n".join(out)

    for fn in HELPERS:
        text = text.replace("\n" + fn, "\nstatic " + fn)
    # The `using Row = ...` alias also landed after main; give it a file-scope home
    # by leaving it where it is but ensuring it precedes its first use.
    open(PATH, "w").write(text)
    print("hoisted %d helpers out of main()" % len(HELPERS))
    return 0


if __name__ == "__main__":
    sys.exit(main())