p = "Source/tests/test_csr_ops.cpp"
L = open(p, encoding="utf-8", newline="").read().splitlines()
for k in ["C5:", "C12:", "C20", "set_v2_row_sort_mode"]:
    print(k, [i + 1 for i, l in enumerate(L) if k in l])
print("printf blocks:", [i + 1 for i, l in enumerate(L) if "checks, %d failures" in l])
print("returns:", [i + 1 for i, l in enumerate(L) if l.strip().startswith("return")])
