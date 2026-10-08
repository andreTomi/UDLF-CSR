import re

with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    s = f.read()

# M2: guarda não-finito no F7b - check for non-finite values before sorting
old = '''      std::vector<std::size_t> ord(cap);
      for (std::size_t s = 0; s < cap; ++s) ord[s] = s;
      std::stable_sort(ord.begin(), ord.end(),
                       [&a](std::size_t x, std::size_t y) { return a[x] > a[y]; });'''

new = '''      // M2: guarda não-finito no F7b - check for non-finite values before sorting
      for (std::size_t s = 0; s < cap; ++s) {
        if (!std::isfinite(a[s])) {
          a[s] = 0.0f;  // M2: replace non-finite with 0.0f to avoid NaN/Inf propagation
        }
      }
      std::vector<std::size_t> ord(cap);
      for (std::size_t s = 0; s < cap; ++s) ord[s] = s;
      std::stable_sort(ord.begin(), ord.end(),
                       [&a](std::size_t x, std::size_t y) { return a[x] > a[y]; });'''

if old in s:
    s = s.replace(old, new, 1)
    print('Replaced M2 F7b')
else:
    print('NOT FOUND')

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(s)
print('Done')