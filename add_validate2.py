import re

with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    s = f.read()

old = '''  // THE ENTRY POINT. This is the ONLY pipeline the port is allowed to run, and
  // the parity gate calls THIS rather than spelling the sequence out again.
  //
  // X9: the gate used to reimplement the sequence inside test_rfe_parity, and a
  // COPY can drift from the thing it is checking without anything failing. It
  // did: this function skipped f7a_rerank_by_emb_sim entirely and ran the embRR
  // block once instead of t_ times, and the gate stayed green because the gate
  // never called it. A gate must exercise the production entry point.
  //
  // Mirrors RFE::runUdlMethod() (RFE.cpp:102-124) restricted to the in-scope
  // core. The legacy's tail -- if (performCCs) execRRCCs(); and
  // if (rerankByEmb || exportEmbeddings) execRRByEmbCCs(); -- is NOT ported, so
  // this method REFUSES those configs at construction rather than skipping them
  // here and silently emitting different rank lists.
  void run() {'''

new = '''  // H4: validate input at the START of run() -- O(N*L) fail-fast before any computation
  // Checks: all ids in [0, n), each row has exactly l distinct ids, self at index 0.
  void validate_input() {
    // Check all ids in [0, n) and each row has exactly l entries
    std::vector<char> seen((std::size_t)n_, 0);
    for (std::size_t q = 0; q < (std::size_t)n_; ++q) {
      std::size_t base = (std::size_t)l_ * q;
      // Check self at position 0
      if (rk_[(std::size_t)l_ * q] != (int)q) {
        throw std::runtime_error("RFE: row " + std::to_string(q) + " does not have self at index 0");
      }
      for (int i = 0; i < l_; ++i) {
        int v = rk_[(std::size_t)l_ * q + i];
        if (v < 0 || v >= n_) {
          throw std::runtime_error("RFE: row " + std::to_string(q) + " has id " + std::to_string(v) + " out of range [0, " + std::to_string(n_) + ")");
        }
        if (seen[(std::size_t)v]) {
          throw std::runtime_error("RFE: row " + std::to_string(q) + " has duplicate id " + std::to_string(v));
        }
        seen[(std::size_t)v] = 1;
      }
      // Reset seen for next row
      for (int i = 0; i < l_; ++i) {
        seen[(std::size_t)rk_[(std::size_t)l_ * q + i]] = 0;
      }
    }
  }

  // THE ENTRY POINT. This is the ONLY pipeline the port is allowed to run, and
  // the parity gate calls THIS rather than spelling the sequence out again.
  //
  // X9: the gate used to reimplement the sequence inside test_rfe_parity, and a
  // COPY can drift from the thing it is checking without anything failing. It
  // did: this function skipped f7a_rerank_by_emb_sim entirely and ran the embRR
  // block once instead of t_ times, and the gate stayed green because the gate
  // never called it. A gate must exercise the production entry point.
  //
  // Mirrors RFE::runUdlMethod() (RFE.cpp:102-124) restricted to the in-scope
  // core. The legacy's tail -- if (performCCs) execRRCCs(); and
  // if (rerankByEmb || exportEmbeddings) execRRByEmbCCs(); -- is NOT ported, so
  // this method REFUSES those configs at construction rather than skipping them
  // here and silently emitting different rank lists.
  void run() {'''

with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    s = f.read()

if old in s:
    s = s.replace(old, new, 1)
    print('Replaced validate_input insertion')
else:
    print('NOT FOUND')

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(s)
print('Done')