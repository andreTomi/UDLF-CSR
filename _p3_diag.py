#!/usr/bin/env python3
# _p3_diag.py <mode> -- Fase 0 do diagnostico S2.4: agrega s24/raw_<mode>/ e
# decompoe cada stage do driver em (loop | build | alloc) por braco, com gates
# de checksum e de deriva do canario.
#
# Semantica dos dados (verificada em fonte/bench_phase.cpp + SparseMatrix.hpp):
#   * rep i: linha REP,i seguida do bloco PHASEPROFILE do MESMO rep
#     (phprof::reset() no inicio do rep, report() no fim); rep0 descartado.
#   * DRVAGG: mediana por run sobre TODOS os reps (rep0 incluso -- caveat),
#     agregado aqui como mediana das medianas por run.
#   * fases seriais (x_CALL, ALLOC_*, V5_COMPACT, B3_PREFIX, TRANSPOSE):
#     wall_ms = span total confiavel.
#   * fases paralelas (x_LOOP, MULT_TOTAL, V4_ROWSORT): os stamps sao
#     sobrescritos por regiao, entao wall_ms cobre so a ULTIMA instancia;
#     usamos total_ms/T como estimativa de wall total do loop (T = PHASE_THREADS).
#   * B3_HIST/B5_FILL nao tem wall (rusage only): user_ms = CPU agregado.
#   * HWM build_calls e cumulativo por run: delta entre HWMs = builds/rep.
import os
import statistics as st
import sys

D = "/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR/Data/phase_profile/etapa_b/s24"
mode = sys.argv[1] if len(sys.argv) > 1 else None
if mode not in ("ab12", "ab1", "abcprr"):
    sys.exit("usage: _p3_diag.py <ab12|ab1|abcprr> [suffixo_raw]")
# sufixo opcional p/ campanhas extras sem sobrescrever a evidencia base
# (Fase 1: suffixo=_f1 le raw_<mode>_f1/ e escreve no stdout normalmente)
suffix = sys.argv[2] if len(sys.argv) > 2 else ""
RAD = "%s/raw_%s%s" % (D, mode, suffix)
T = 1 if mode == "ab1" else 12

CALL_PHASES = ["MP_CALL", "MERGE_CALL", "COMBINE_CALL", "BUILD_CALL",
               "ALLOC_SETUP", "ALLOC_TEARDOWN"]
LOOP_PHASES = ["MULT_TOTAL", "MERGE_LOOP", "COMBINE_LOOP"]
BUILD_SERIAL = ["V5_COMPACT", "B3_PREFIX", "TRANSPOSE"]
BUILD_PAR = ["V4_ROWSORT", "B3_HIST", "B5_FILL"]


def arm_of(tag):
    if tag.startswith("warm"):
        return None
    if tag.startswith("canary"):
        return "canary"
    if "_pre" in tag:
        return "pre"
    if "_new" in tag:
        return "new"
    return None


def read(tag):
    drv, reps, cks, blocks, hwm, done = {}, [], set(), [], [], ""
    path = "%s/%s.txt" % (RAD, tag)
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.rstrip("\n").split(",")
        if p[0] == "REP":
            reps.append(float(p[2]))
            cks.add(p[4])
            blocks.append({})
        elif p[0] == "DRVAGG":
            drv[p[1]] = float(p[2].split("=")[1])
        elif p[0] == "PHASEPROFILE" and len(p) > 2 and "=" in p[2]:
            kv = dict(x.split("=", 1) for x in p[2:] if "=" in x)
            if p[1] == "HWM":
                hwm.append(kv)
            elif blocks and p[1] not in ("THREAD", "CTX"):
                blocks[-1][p[1]] = kv
        elif p[0] == "DONE":
            done = line.strip()
    return dict(drv=drv, reps=reps, cks=cks, blocks=blocks, hwm=hwm, done=done)


tags = sorted(f[:-4] for f in os.listdir(RAD) if f.endswith(".txt"))
data = {t: read(t) for t in tags}
arms = {a: [t for t in tags if arm_of(t) == a] for a in ("pre", "new", "canary")}
print("== mode=%s  T=%d ==" % (mode, T))
print("runs: pre=%s" % arms["pre"])
print("      new=%s" % arms["new"])
print("      canary=%s" % arms["canary"])


def usable(t):
    """REP totals com rep0 descartado."""
    r = data[t]["reps"]
    return r[1:] if len(r) > 1 else r


def phase_vals(arm, phase, metric):
    """Valores por rep util (rep>0) de todos os runs do braco."""
    out = []
    for t in arms[arm]:
        for i, blk in enumerate(data[t]["blocks"]):
            if i == 0:
                continue
            if phase in blk:
                out.append(float(blk[phase][metric]))
    return out


def med(arm, phase, metric):
    v = phase_vals(arm, phase, metric)
    return st.median(v) if v else 0.0


def drv_med(arm, label):
    v = [data[t]["drv"][label] for t in arms[arm] if label in data[t]["drv"]]
    return st.median(v) if v else None


# --------------------------------------------------------------------------
# 1) REP total + ABBA pareado por ciclo
# --------------------------------------------------------------------------
pre_r = [v for t in arms["pre"] for v in usable(t)]
new_r = [v for t in arms["new"] for v in usable(t)]
print("\n== REP total (rep0 descartado) ==")
for t in arms["pre"] + arms["new"]:
    u = usable(t)
    print("  %-10s n=%d med=%8.1f  vals=%s" %
          (t, len(u), st.median(u), ["%.0f" % x for x in u]))
print("  POOL pre: n=%d med=%.1f  |  new: n=%d med=%.1f  | ratio=%.4f" %
      (len(pre_r), st.median(pre_r), len(new_r), st.median(new_r),
       st.median(new_r) / st.median(pre_r)))
rep_delta = st.median(new_r) - st.median(pre_r)

print("\n== ABBA pareado por ciclo (mean(new) - mean(pre)) ==")
cycle_deltas = []
for c in sorted({t.split("_")[0] for t in arms["pre"]}):
    pre_c = [v for t in arms["pre"] if t.startswith(c + "_") for v in usable(t)]
    new_c = [v for t in arms["new"] if t.startswith(c + "_") for v in usable(t)]
    if pre_c and new_c:
        d = st.mean(new_c) - st.mean(pre_c)
        cycle_deltas.append(d)
        print("  %s: pre=%.1f  new=%.1f  delta=%+.1f ms" %
              (c, st.mean(pre_c), st.mean(new_c), d))
if cycle_deltas:
    print("  delta ABBA medio=%+.1f ms (std=%.1f)" %
          (st.mean(cycle_deltas),
           st.stdev(cycle_deltas) if len(cycle_deltas) > 1 else 0.0))

# --------------------------------------------------------------------------
# 2) DRVAGG (visao do driver por stage; rep0 incluso, caveat)
# --------------------------------------------------------------------------
labels = []
for t in arms["pre"] + arms["new"]:
    for k in data[t]["drv"]:
        if k not in labels:
            labels.append(k)
print("\n== DRVAGG por stage (mediana das medianas/run; rep0 incluso) ==")
print("  %-24s %9s %9s %9s %7s" % ("label", "pre(ms)", "new(ms)", "delta", "%"))
for k in labels:
    p, n = drv_med("pre", k), drv_med("new", k)
    if p is None or n is None:
        print("  %-24s %s" % (k, "MISSING pre" if p is None else "MISSING new"))
        continue
    print("  %-24s %9.1f %9.1f %+9.1f %6.1f%%" %
          (k, p, n, n - p, 100.0 * (n - p) / p if p else 0.0))

# --------------------------------------------------------------------------
# 3) Decomposicao por fases: loop vs build vs alloc
# --------------------------------------------------------------------------
print("\n== Fases seriais: wall_ms confiavel (medianas por rep util) ==")
print("  %-18s %9s %9s %9s %7s  %s" %
      ("fase", "pre(ms)", "new(ms)", "delta", "%", "nota"))
for ph in CALL_PHASES:
    p, n = med("pre", ph, "wall_ms"), med("new", ph, "wall_ms")
    calls = med("new", ph, "calls")
    print("  %-18s %9.1f %9.1f %+9.1f %6.1f%%  calls/rep=%.1f" %
          (ph, p, n, n - p, 100.0 * (n - p) / p if p else 0.0, calls))
for ph in BUILD_SERIAL:
    p, n = med("pre", ph, "wall_ms"), med("new", ph, "wall_ms")
    print("  %-18s %9.1f %9.1f %+9.1f %6.1f%%" %
          (ph, p, n, n - p, 100.0 * (n - p) / p if p else 0.0))

print("\n== Fases paralelas: wall estimado = total_ms/T (T=%d) ==" % T)
print("  %-18s %9s %9s %9s %7s  %s" %
      ("fase", "pre(ms)", "new(ms)", "delta", "%", "nota"))
for ph in LOOP_PHASES:
    p, n = med("pre", ph, "total_ms") / T, med("new", ph, "total_ms") / T
    print("  %-18s %9.1f %9.1f %+9.1f %6.1f%%  wall_stamp(pre/new)=%.1f/%.1f" %
          (ph, p, n, n - p, 100.0 * (n - p) / p if p else 0.0,
           med("pre", ph, "wall_ms"), med("new", ph, "wall_ms")))

print("\n== Fases de build paralelas: CPU (user_ms) + calls ==")
for ph in BUILD_PAR:
    pu, nu = med("pre", ph, "user_ms"), med("new", ph, "user_ms")
    pc, nc = med("pre", ph, "calls"), med("new", ph, "calls")
    print("  %-18s user pre=%8.1f new=%8.1f delta=%+8.1f | calls/rep pre=%.0f new=%.0f" %
          (ph, pu, nu, nu - pu, pc, nc))

bc = []
for arm in ("pre", "new"):
    ds = []
    for t in arms[arm]:
        h = data[t]["hwm"]
        for i in range(1, len(h)):
            ds.append(int(h[i]["build_calls"]) - int(h[i - 1]["build_calls"]))
    bc.append(int(st.median(ds)) if ds else 0)
print("  build_calls/rep (HWM delta): pre=%d new=%d" % tuple(bc))

# --------------------------------------------------------------------------
# 4) Decomposicao derivada por chamada: nao-loop = CALL_wall - loop_est
# --------------------------------------------------------------------------
def nonloop(call, loop):
    c = med("new", call, "wall_ms") - med("pre", call, "wall_ms")
    l = (med("new", loop, "total_ms") - med("pre", loop, "total_ms")) / T
    return c, l, c - l


print("\n== Atribuicao de delta (new-pre) por chamada ==")
print("  %-14s %9s %9s %9s   %s" %
      ("chamada", "dCALL", "dLOOP", "dREST", "REST = alloc + build interno"))
comb = nonloop("COMBINE_CALL", "COMBINE_LOOP")
mp = nonloop("MP_CALL", "MULT_TOTAL")
print("  %-14s %+9.1f %+9.1f %+9.1f   (combine)" %
      ("combine", comb[0], comb[1], comb[2]))
print("  %-14s %+9.1f %+9.1f %+9.1f   (mults)" %
      ("mult", mp[0], mp[1], mp[2]))
if mode == "abcprr":
    mg = nonloop("MERGE_CALL", "MERGE_LOOP")
    print("  %-14s %+9.1f %+9.1f %+9.1f   (merge)" %
          ("merge", mg[0], mg[1], mg[2]))
bc_all = med("new", "BUILD_CALL", "wall_ms") - med("pre", "BUILD_CALL", "wall_ms")
alloc = ((med("new", "ALLOC_SETUP", "wall_ms") +
          med("new", "ALLOC_TEARDOWN", "wall_ms")) -
         (med("pre", "ALLOC_SETUP", "wall_ms") +
          med("pre", "ALLOC_TEARDOWN", "wall_ms")))
print("  dBUILD_CALL(todos os builds)=%+.1f ms   dALLOC=%+.1f ms   dREP=%+.1f ms"
      % (bc_all, alloc, rep_delta))
loop_tot = ((med("new", "COMBINE_LOOP", "total_ms") -
             med("pre", "COMBINE_LOOP", "total_ms")) +
            (med("new", "MULT_TOTAL", "total_ms") -
             med("pre", "MULT_TOTAL", "total_ms")) +
            (med("new", "MERGE_LOOP", "total_ms") -
             med("pre", "MERGE_LOOP", "total_ms"))) / T
print("  dLOOP estimado (todas as loops, total/T)=%+.1f ms" % loop_tot)

# --------------------------------------------------------------------------
# 5) Gates: checksum, nnz, deriva do canario
# --------------------------------------------------------------------------
print("\n== Gates ==")
ck = {c for t in arms["pre"] + arms["new"] for c in data[t]["cks"]}
print("  GATE checksum (1 valor distinto, warm/canary excluidos): %s %s" %
      ("PASS" if len(ck) == 1 else "FAIL", sorted(ck)))
nnz_set = set()
for t in arms["pre"] + arms["new"]:
    for line in open("%s/%s.txt" % (RAD, t), encoding="utf-8", errors="replace"):
        if line.startswith("REP,"):
            nnz_set.add(line.split(",")[3])
print("  GATE nnz identico: %s %s" %
      ("PASS" if len(nnz_set) == 1 else "FAIL", sorted(nnz_set)))
can = [v for t in arms["canary"] for v in usable(t)]
mid = [v for t in arms["canary"] if t not in ("canary_0",) for v in usable(t)]
# Piso S1-bis (canary.sh, workload w3): rep >= 1000 ms e checksum correto.
# Um workload curto (<1 s) nao separa deriva de sessao de ruido amostral --
# foi assim que o `mult w1` herdado regrediu 3x (H3). O piso e mecanico.
CANARY_MIN_MS = 1000.0
CANARY_CKSUM = "8273.428589"
if can:
    floor_ok = all(v >= CANARY_MIN_MS for v in can)
    ck_ok = all(data[t]["cks"] <= {CANARY_CKSUM} for t in arms["canary"])
    print("  GATE piso canario (REP >= %.0f ms): %s" %
          (CANARY_MIN_MS, "PASS" if floor_ok else "FAIL"))
    print("  GATE checksum canario (%s): %s" %
          (CANARY_CKSUM, "PASS" if ck_ok else "FAIL"))
if can:
    # canary_0 roda logo apos os warm-ups (cold-start do caminho mult w1);
    # a deriva mid-sessao (canary_1..N) e o que caracteriza instabilidade
    # entre os blocos medidos.
    drift_all = (max(can) - min(can)) / min(can) * 100.0
    drift_mid = ((max(mid) - min(mid)) / min(mid) * 100.0) if len(mid) > 1 else 0.0
    print("  canary vals (todos=%s) (mid-sessao=%s)" %
          (["%.0f" % x for x in can], ["%.0f" % x for x in mid]))
    print("  GATE deriva mid-sessao canary_1..N (<=5%%): %s  drift=%.2f%%" %
          ("PASS" if drift_mid <= 5.0 else "FAIL", drift_mid))
    print("  (referencia: drift completo incl. canary_0 = %.2f%%)" % drift_all)

# --------------------------------------------------------------------------
# 6) Heuristica de veredito H1/H2/H3
# --------------------------------------------------------------------------
print("\n== Heuristica de veredito ==")
abs_loop, abs_build = abs(loop_tot), abs(bc_all)
share_loop = 100.0 * abs_loop / abs(rep_delta) if rep_delta else 0.0
share_build = 100.0 * abs_build / abs(rep_delta) if rep_delta else 0.0
print("  |dLOOP|/|dREP| = %.0f%%   |dBUILD_CALL|/|dREP| = %.0f%%" %
      (share_loop, share_build))
drift_ok = (not mid) or ((max(mid) - min(mid)) / min(mid) * 100.0 <= 5.0)
if not drift_ok:
    print("  -> H3 suspeita: canario derivou; nao confiar nos deltas desta sessao.")
elif share_build >= 50.0 and share_loop <= 30.0:
    print("  -> H1: delta concentrado no BUILD_CALL (work extra do builder).")
elif share_loop >= 50.0:
    print("  -> H2: delta nas LOOPS (loop nao alterado -> layout/inlining/ruído).")
else:
    print("  -> Mistura: ver linha REST por chamada (alloc vs build interno).")
print("done.")
