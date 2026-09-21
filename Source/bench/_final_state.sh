#!/usr/bin/env bash
# _final_state.sh -- end-of-Etapa-A state snapshot.
cd /mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR || exit 1
out=Data/phase_profile/etapa_a/final_state.txt
{
  echo "branch=$(git rev-parse --abbrev-ref HEAD) commit=$(git rev-parse --short HEAD)"
  echo "--- status (should be empty):"
  git status --short
  echo "--- etapa_a files:"
  ls Data/phase_profile/etapa_a | sort
  echo "raw_v2=$(ls Data/phase_profile/etapa_a/raw_v2 | wc -l) raw_v1=$(ls Data/phase_profile/etapa_a/raw | wc -l)"
  echo "--- recent commits:"
  git --no-pager log --oneline -7
} > "$out" 2>&1
echo "written $out"
