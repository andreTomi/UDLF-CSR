#!/usr/bin/env bash
# _maskprobe.sh -- what does taskset really do inside a restricted caller?
set -u
echo "caller affinity: $(grep Cpus_allowed_list /proc/self/status | cut -f2)"
echo "caller nproc:    $(nproc)   nproc --all: $(nproc --all)"
for m in 0 0-1 0-3 0-5 0-11 0,2,4; do
  out=$(taskset -c "$m" grep Cpus_allowed_list /proc/self/status 2>&1)
  rc=$?
  printf 'taskset -c %-5s -> rc=%s  nproc=%s  %s\n' "$m" "$rc" \
    "$(taskset -c "$m" nproc 2>/dev/null)" "$out"
done
echo "--- with the mask read from the child env (sched_getaffinity in C) ---"
cat > /tmp/aff.c <<'EOF'
#include <sched.h>
#include <stdio.h>
int main(void){cpu_set_t s; CPU_ZERO(&s);
 if(sched_getaffinity(0,sizeof s,&s)) return 1;
 for(int c=0;c<CPU_SETSIZE;c++) if(CPU_ISSET(c,&s)) printf("%d,",c);
 printf("\n"); return 0;}
EOF
gcc -O1 -o /tmp/aff /tmp/aff.c && echo "aff built"
for m in 0-3 0-5 0-11; do
  printf 'taskset -c %-5s -> ' "$m"; taskset -c "$m" /tmp/aff || echo "(failed rc=$?)"
done
