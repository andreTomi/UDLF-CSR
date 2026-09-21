#!/usr/bin/env python3
"""Fase 3: nth_element sobre pares contíguos."""
import sys

filepath = '/mnt/c/Users/andre/Documents/phd/UDLF-CSR/UDLF-CSR/Source/src/Core/hpp/SparseMatrix.hpp'

with open(filepath, 'r') as f:
    content = f.read()

# 1. Find and replace the multiply_prune_row signature (add pairs parameter)
old_sig = '    static void multiply_prune_row(size_type i, const CSRMatrix& A, const CSRMatrix& B,\n                                    size_t limit, Acc& acc, BlockAllocator<Triplet>& allocator\n#ifdef PHASE_PROFILE\n                                    , phprof::RowTimer* rt = nullptr\n#endif\n                                    ) {'

new_sig = '    static void multiply_prune_row(size_type i, const CSRMatrix& A, const CSRMatrix& B,\n                                    size_t limit, Acc& acc, BlockAllocator<Triplet>& allocator,\n                                    std::vector<std::pair<index_type, T>>& pairs\n#ifdef PHASE_PROFILE\n                                    , phprof::RowTimer* rt = nullptr\n#endif\n                                    ) {'

if old_sig not in content:
    print("ERRO: old_sig nao encontrado")
    sys.exit(1)
content = content.replace(old_sig, new_sig, 1)
print("1. signature OK")

# 2. Replace the nth_element + emit block
ns_start = content.find('        auto& used = acc.used();')
if ns_start == -1:
    print("ERRO: auto& used nao encontrado")
    sys.exit(1)
ns_end_marker = '        if (rt) rt->f2 += phprof::tick() - t0_row;'
ns_end = content.find(ns_end_marker, ns_start)
if ns_end == -1:
    print("ERRO: fim do bloco de emissao nao encontrado")
    sys.exit(1)
ns_end += len(ns_end_marker)

new_body = '''        // FASE 3: materialize contiguous (col, val) pairs from the accumulator,
        // then run nth_element + col-sort on contiguous memory. This eliminates
        // the indirect comparator lookups (acc.val_of(slot)) that dominated
        // F2_PRUNE on the HashRowAccumulator (random access into slot_val).
        // The pairs buffer is reused across rows via clear()+reserve().
        auto& used = acc.used();
        size_t touched = used.size();

        pairs.clear();
        pairs.reserve(touched);
        for (size_t idx = 0; idx < touched; ++idx) {
            pairs.emplace_back(acc.col_of(used[idx]), acc.val_of(used[idx]));
        }

        size_t emit_count = pairs.size();
        if (emit_count > limit) {
            std::nth_element(pairs.begin(), pairs.begin() + limit, pairs.end(),
                [](const std::pair<index_type, T>& a,
                   const std::pair<index_type, T>& b) {
                    return std::abs(a.second) > std::abs(b.second);
                });
            pairs.resize(limit);
        }

        // FASE 1: sort by col so build_from_ordered_fragments receives
        // (row, col) ordered streams. Contiguous sort on pairs.
        std::sort(pairs.begin(), pairs.end(),
            [](const std::pair<index_type, T>& a,
               const std::pair<index_type, T>& b) {
                return a.first < b.first;
            });

        for (auto& p : pairs) {
            if (std::abs(p.second) > 1e-9) {
                Triplet* t = allocator.allocate();
                t->row = i; t->col = p.first; t->value = p.second; t->is_set = false;
#ifdef PHASE_PROFILE
                if (rt) ++rt->emitted;
#endif
            }
        }
#ifdef PHASE_PROFILE
        if (rt) rt->f2 += phprof::tick() - t0_row;'''

content = content[:ns_start] + new_body + content[ns_end:]
print("2. nth_element body OK")

# 3. Update the call sites in multiply_and_prune
for old in ['multiply_prune_row(i, A, B, limit, acc, allocator,']:
    new = 'multiply_prune_row(i, A, B, limit, acc, allocator, pairs,'
    content = content.replace(old, new)
for old in ['multiply_prune_row(i, A, B, limit, acc, allocator);']:
    new = 'multiply_prune_row(i, A, B, limit, acc, allocator, pairs);'
    content = content.replace(old, new)
print("3. call sites OK")

# 4. Declare pairs buffer in the parallel region (once per thread)
old_decl = '            size_t max_bound = 0;'
new_decl = '            std::vector<std::pair<index_type, T>> pairs;  // FASE 3: reused across rows\n            size_t max_bound = 0;'

if old_decl in content:
    content = content.replace(old_decl, new_decl, 1)
    print("4. pairs declaration OK")
else:
    print("ERRO: pairs declaration marker not found")
    sys.exit(1)

with open(filepath, 'w') as f:
    f.write(content)

print("Fase 3: todas as modificacoes aplicadas")
