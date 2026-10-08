import re

with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    s = f.read()

# M1: merge_sim - skip non-finite sim values
old = '''  static float merge_sim(const std::vector<std::pair<idx_t, float>>& a,
                         const std::vector<std::pair<idx_t, float>>& b) {
    float sim = 0;
    std::size_t ib = 0;
    for (const auto& kv : a) {
      while (ib < b.size() && b[ib].first < kv.first) ++ib;
      if (ib < b.size() && b[ib].first == kv.first) sim += kv.second * b[ib].second;
    }
    return sim;
  }'''

new = '''  // M1: skip non-finite sim values (bad_sim) to avoid propagating NaN/Inf
  static float merge_sim(const std::vector<std::pair<idx_t, float>>& a,
                           const std::vector<std::pair<idx_t, float>>& b) {
    float sim = 0;
    std::size_t ib = 0;
    for (const auto& kv : a) {
      while (ib < b.size() && b[ib].first < kv.first) ++ib;
      if (ib < b.size() && b[ib].first == kv.first) {
        float prod = kv.second * b[ib].second;
        if (std::isfinite(prod)) sim += prod;  // M1: skip non-finite products
      }
    }
    return sim;
  }'''

if old in s:
    s = s.replace(old, new, 1)
    print('Replaced M1 merge_sim')
else:
    print('NOT FOUND')

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(s)
print('Done')