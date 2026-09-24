import sys

for p in ["Source/tests/test_csr_ops.cpp"]:
    L = open(p, encoding="utf-8", newline="").read().splitlines()
    print(p, "total lines:", len(L))
    for i in range(683, len(L)):
        print(i + 1, repr(L[i]))
    s = "\n".join(L)
    for k in ["C20", "build_from_partial_results_legacy", "int main", "g_checks"]:
        hits = [i + 1 for i, l in enumerate(L) if k in l]
        print("hits %-32s %s" % (k, hits))
