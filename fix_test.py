with open('test_rfe_cc_e2e.cpp', 'r') as f:
    content = f.read()

old = '''  // Debug: compute sim for cell (0,32) manually
  // Access private nnz_cols via a helper - skip for now
  std::printf("DEBUG: checking sim for cell (0,32)\\\\n");
  
  // Continue core'''

new = '''  // Print first 5 ee values and sorted_ee
  std::printf("CORE ee[0..4]: ");
  for (int i = 0; i < 5; ++i) std::printf("%.9g ", (double)core.ee_[i]);
  std::printf("\\nCORE sorted_ee[0..4]: ");
  for (int i = 0; i < 5; ++i) std::printf("%d ", core.sorted_ee_[i]);
  std::printf("\\n");
  
  // Continue core'''

if old in content:
    content = content.replace(old, new)
    with open('test_rfe_cc_e2e.cpp', 'w') as f:
        f.write(content)
    print('Fixed')
else:
    print('Not found')
    idx = content.find('DEBUG: checking sim')
    if idx != -1:
        print('Found at', idx)
        print(repr(content[idx:idx+200]))