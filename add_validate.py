import re

with open('Source/src/Methods/hpp/RFECore.hpp', 'r') as f:
    s = f.read()

old = '''void run() {
    // The profile lives HERE, beside the sequence, not in the bench. A profile
    // outside run() would measure its own copy -- which is the defect X9 names,
    // and it is exactly how the pre-t-loop "full pipeline" numbers happened.
    const bool prof = runprof_.enabled;
    if (prof) runprof_.begin(t_, n_, l_);
    const double R0 = prof ? omp_get_wtime() : 0.0;'''

new = '''void run() {
    // H4: validate input at the START of run() -- O(N*L) fail-fast before any computation
    validate_input();

    // The profile lives HERE, beside the sequence, not in the bench. A profile
    // outside run() would measure its own copy -- which is the defect X9 names,
    // and it is exactly how the pre-t-loop "full pipeline" numbers happened.
    const bool prof = runprof_.enabled;
    if (prof) runprof_.begin(t_, n_, l_);
    const double R0 = prof ? omp_get_wtime() : 0.0;'''

if old in s:
    s = s.replace(old, new, 1)
    print('Replaced run() start')
else:
    print('NOT FOUND')

with open('Source/src/Methods/hpp/RFECore.hpp', 'w') as f:
    f.write(s)
print('Done')