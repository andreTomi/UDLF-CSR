import sys

CRLF = chr(13) + chr(10)


def L(*lines):
    return CRLF.join(lines)


p = "/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR/Source/bench/bench_phase.cpp"
s = open(p, encoding="utf-8", newline="").read()

old = L("                omp_threads, omp_get_max_threads(), affinity_mask().c_str(),",
        "                g_affinity_at_entry.c_str(), env_or(\"PHASE_MASK\", \"unset\").c_str(),",
        "                sysconf(_SC_NPROCESSORS_ONLN),",
        "                (size_t)omp_get_max_threads(), (int)getpid());")
new = L("                omp_threads, affinity_mask().c_str(),",
        "                g_affinity_at_entry.c_str(), env_or(\"PHASE_MASK\", \"unset\").c_str(),",
        "                sysconf(_SC_NPROCESSORS_ONLN),",
        "                (size_t)omp_get_max_threads(), (int)getpid());")

if s.count(old) != 1:
    sys.exit("ABORT: print_tset arg block not found once (found %d)" % s.count(old))
s = s.replace(old, new)
open(p, "w", encoding="utf-8", newline="").write(s)
print("print_tset: stray omp_get_max_threads() argument removed")
