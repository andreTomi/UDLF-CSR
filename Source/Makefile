# Compiler settings
CC        = g++

# Vectorization and optimization flags
VECTOR_FLAGS    = -march=native -mtune=native -mavx2 -mfma -msse4.2
OPENMP_FLAGS    = -fopenmp
FAST_MATH_FLAGS = -ffast-math -funsafe-math-optimizations -ffinite-math-only
LOOP_FLAGS      = -funroll-loops -fprefetch-loop-arrays -floop-nest-optimize
INLINE_FLAGS    = -finline-functions -finline-limit=300
CACHE_FLAGS     = -falign-functions=32 -falign-loops=32

# Base optimization flags
# NDEBUG compiles out the two runtime asserts in SparseMatrix.hpp; the debug
# target redefines CFLAGS without it, so sanitizer/debug builds keep asserts.
BASE_FLAGS = -std=gnu++23 -O3 -g -fno-omit-frame-pointer -D_GNU_SOURCE 
RELEASE_FLAGS = -DNDEBUG

# Combined flags for maximum performance
CFLAGS = $(BASE_FLAGS) $(RELEASE_FLAGS) $(VECTOR_FLAGS) $(OPENMP_FLAGS) $(FAST_MATH_FLAGS) $(LOOP_FLAGS) $(INLINE_FLAGS) $(CACHE_FLAGS)

# Define MKL Root for clarity
MKL_ROOT = /opt/intel/oneapi/mkl/latest

# Link flags
LDFLAGS   = -L/usr/lib/x86_64-linux-gnu \
            -lm -lpthread -ltbb -ljemalloc -lgraphblas \
            -L$(MKL_ROOT)/lib/intel64 \
            -Wl,--no-as-needed -lmkl_intel_lp64 -lmkl_gnu_thread -lmkl_core -lgomp -lmkl_rt

# Include directories
INCFLAGS  = -I./src \
						-I/usr/include/eigen3 \
						-I/usr/include/suitesparse \
						-I$(MKL_ROOT)/include \
            -I./src/Config/hpp \
            -I./src/Config/models \
            -I./src/Core/hpp \
            -I./src/Methods/hpp \
            -I./src/Evaluation/hpp \
            -I./src/Utils/hpp

# Build directories
OBJ_DIR   = obj
OBJ_DIR_SWAP = obj_swap
SRC_DIR   = src
BUILD_DIR = bin

# Find all .cpp files
SRC_FILES  := $(wildcard \
              $(SRC_DIR)/Config/cpp/*.cpp \
              $(SRC_DIR)/Core/cpp/*.cpp \
              $(SRC_DIR)/Evaluation/cpp/*.cpp \
              $(SRC_DIR)/Methods/cpp/*.cpp \
              $(SRC_DIR)/Utils/cpp/*.cpp)

# Convert source file paths to object file paths
OBJ_FILES  := $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRC_FILES))
OBJ_FILES_SWAP := $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR_SWAP)/%.o, $(SRC_FILES))

# Build targets
.PHONY: all debug profile vectorize clean run run-debug run-profile check-vectorization swap test-rdpac test-bfstree

# Default target - optimized builds for both versions
all: udlf swap

# Main optimized build target (Pure MMAP)
udlf: $(OBJ_FILES)
	@echo "Building optimized MMAP executable..."
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ_FILES) -o $(BUILD_DIR)/udlf $(CFLAGS) $(LDFLAGS)
	@echo "Build complete. Executable: $(BUILD_DIR)/udlf"

# Swap build target (Anonymous Memory / Swap-backed)
swap: $(OBJ_FILES_SWAP)
	@echo "Building optimized SWAP executable..."
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ_FILES_SWAP) -o $(BUILD_DIR)/udlf_swap $(CFLAGS) -DVIRTUAL_ARRAY_FULL_MEMORY $(LDFLAGS)
	@echo "Build complete. Executable: $(BUILD_DIR)/udlf_swap"

# Rule to compile .cpp files into .o files (MMAP)
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(INCFLAGS) -c $< -o $@

# Rule to compile .cpp files into .o files (SWAP)
$(OBJ_DIR_SWAP)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $< (swap)..."
	$(CC) $(CFLAGS) -DVIRTUAL_ARRAY_FULL_MEMORY $(INCFLAGS) -c $< -o $@

# Debug build (less optimization, better debugging)
debug: CFLAGS = $(BASE_FLAGS) -O1 -DDEBUG -fsanitize=address -fsanitize=undefined
debug: LDFLAGS += -fsanitize=address -fsanitize=undefined
debug: clean
debug: $(OBJ_FILES)
	@echo "Building debug executable..."
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ_FILES) -o $(BUILD_DIR)/udlf-debug $(CFLAGS) $(LDFLAGS)
	@echo "Debug build complete. Executable: $(BUILD_DIR)/udlf-debug"

# Profile build (with profiling support)
profile: CFLAGS = $(BASE_FLAGS) $(RELEASE_FLAGS) $(VECTOR_FLAGS) $(OPENMP_FLAGS) -O3 -pg -fno-inline
profile: LDFLAGS += -pg
profile: clean
profile: $(OBJ_FILES)
	@echo "Building profile executable..."
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ_FILES) -o $(BUILD_DIR)/udlf-profile $(CFLAGS) $(LDFLAGS)
	@echo "Profile build complete. Executable: $(BUILD_DIR)/udlf-profile"

# Build with vectorization reports
vectorize: CFLAGS += -fopt-info-vec -fopt-info-vec-missed -fopt-info-loop
vectorize: clean
vectorize: $(OBJ_FILES)
	@echo "Building with vectorization reports..."
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ_FILES) -o $(BUILD_DIR)/udlf-vec $(CFLAGS) $(LDFLAGS) 2> vectorization_report.txt
	@echo "Vectorization build complete. Check vectorization_report.txt for details"

# Rule to compile .cpp files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(INCFLAGS) -c $< -o $@

# Clean target
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(OBJ_DIR) $(OBJ_DIR_SWAP) $(BUILD_DIR)/udlf $(BUILD_DIR)/udlf_swap $(BUILD_DIR)/udlf-debug $(BUILD_DIR)/udlf-profile $(BUILD_DIR)/udlf-vec
	rm -f vectorization_report.txt gmon.out

# Run targets
run: udlf
	@echo "Running optimized executable..."
	LSAN_OPTIONS=verbosity=1:log_threads=1:detect_leaks=1 ./$(BUILD_DIR)/udlf

run-debug: debug
	@echo "Running debug executable..."
	ASAN_OPTIONS=verbosity=1:halt_on_error=1 LSAN_OPTIONS=verbosity=1:log_threads=1:detect_leaks=1 ./$(BUILD_DIR)/udlf-debug

run-profile: profile
	@echo "Running profile executable..."
	./$(BUILD_DIR)/udlf-profile
	@echo "Generating profile report..."
	gprof $(BUILD_DIR)/udlf-profile gmon.out > profile_report.txt
	@echo "Profile report saved to profile_report.txt"

# Check if vectorization is working
check-vectorization:
	@echo "Checking CPU capabilities..."
	@echo "CPU Info:"
	@lscpu | grep -E "(Model name|Flags)" || echo "lscpu not available"
	@echo ""
	@echo "Checking for vectorization flags in /proc/cpuinfo:"
	@cat /proc/cpuinfo | grep -E "(sse|avx|fma)" | head -1 || echo "No vectorization info found"
	@echo ""
	@echo "Compiler vectorization test:"
	@echo 'int main(){float a[8]={1,2,3,4,5,6,7,8}; for(int i=0;i<8;i++)a[i]*=2; return 0;}' > test_vec.cpp
	@$(CC) -O3 -march=native -mavx2 -fopt-info-vec test_vec.cpp -o test_vec 2>&1 | grep -i "vectorized" || echo "No vectorization detected in test"
	@rm -f test_vec.cpp test_vec
	@echo ""
	@echo "Build with 'make vectorize' to see detailed vectorization reports"

# Show optimization flags being used
show-flags:
	@echo "Compiler: $(CC)"
	@echo "Optimization flags: $(CFLAGS)"
	@echo "Linker flags: $(LDFLAGS)"
	@echo "Include flags: $(INCFLAGS)"

# Performance testing target
perf-test: udlf
	@echo "Running performance test..."
	@echo "Note: This requires perf tool to be installed"
	perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses ./$(BUILD_DIR)/udlf || echo "perf not available or insufficient permissions"

# RDPAC port: phase parity gate + corner cases + thread determinism + bench.
# Strict IEEE (no -ffast-math): the core must match the frozen verbatim
# reference bit-for-bit, and serial/parallel must agree exactly.
# The determinism cmp aborts make on any md5 drift across thread counts.
# The bench runs under OMP_WAIT_POLICY=PASSIVE: libgomp's default ACTIVE
# spin-wait starves SMT siblings on 6-core/12-thread hosts (0.45x on the
# small case); PASSIVE keeps results bit-exact and reports real speedups.
# Run from Source/: make test-rdpac
test-rdpac:
	$(CC) -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I./src -I./src/Core/hpp tests/test_rdpac_parity.cpp -o /tmp/test_rdpac_parity
	/tmp/test_rdpac_parity
	$(CC) -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I./src -I./src/Core/hpp tests/test_rdpac_corner.cpp -o /tmp/test_rdpac_corner
	/tmp/test_rdpac_corner
	OMP_NUM_THREADS=1 /tmp/test_rdpac_parity > /tmp/rdpac_t1.out
	OMP_NUM_THREADS=12 /tmp/test_rdpac_parity > /tmp/rdpac_t12.out
	cmp /tmp/rdpac_t1.out /tmp/rdpac_t12.out
	$(CC) -std=gnu++2b -O2 -ffp-contract=off -fopenmp -I./src -I./src/Core/hpp bench/bench_rdpac.cpp -o /tmp/bench_rdpac
	OMP_WAIT_POLICY=PASSIVE /tmp/bench_rdpac 2000 64 8 42

# BFSTree port (path CSR A2/A4): hypothesis gate F5=SpGEMM (45 checks), real-list
# input contract (rc=5 row with < L survivors, rc=4 row without self) and bit-exact
# agreement of A/A2/A4 with the golden the legacy itself writes at N=800 L=300.
# Same strict-IEEE -O2 posture as test-rdpac, and the CK_FINAL it pins is an -O2
# contract. Measured here: at -O2 the pin is insensitive to -ffp-contract=off
# (identical with and without, N=800 and N=4000), but the product flag set
# (-O3 -march=native -mfma -ffast-math) breaks the port's identity -- at N=800 the
# legacy golden stays 0x7b70dfb313c2b151 while A4 becomes 0x0423b12779c26d3d
# (BITFINAL=NO, POS_EQUAL 0.999983), and at N=4000 even the golden moves
# (0xa299c0fa9515e5de). Re-measured 2026-09-28 with the real Makefile flag
# string (make show-flags) over BOTH fixtures -- N=800 L=300 k=8 s42 and
# N=4000 L=1400 k=20 s42, B=256 limit=N, see Source/bench/prod_numerics.sh:
# PROD fails this gate (hyp 6/45); PRODC (product + -ffp-contract=off) still
# fails the bit-pin (POS_EQUAL 0.999975 -> FMA contraction is not the cause);
# NFM (product minus -ffast-math/-funsafe-math-optimizations/-ffinite-math-only,
# plus -ffp-contract=off) and BENCH (-O2) both print BFSTREE GATES: OK and are
# bit-exact at N=800 AND N=4000. Shipping BFSTree therefore needs that numerics
# decision first (dossiê 7.4, with the measured cost: NFM = -0.2% on synth);
# the gate itself costs ~13 s and touches nothing in src/.
# Full audit: Data/phase_profile/numerics_prod/README.md
# Run from Source/: make test-bfstree
test-bfstree:
	bash tests/gate_bfstree.sh

# Help target
help:
	@echo "Available targets:"
	@echo "  all (default)       - Build optimized executable with vectorization"
	@echo "  debug              - Build debug version with sanitizers"
	@echo "  profile            - Build profiling version"
	@echo "  vectorize          - Build with vectorization reports"
	@echo "  clean              - Remove build artifacts"
	@echo "  run                - Run optimized executable"
	@echo "  run-debug          - Run debug executable"
	@echo "  run-profile        - Run profiling executable and generate report"
	@echo "  check-vectorization - Check CPU and compiler vectorization support"
	@echo "  show-flags         - Show compilation flags"
	@echo "  perf-test          - Run performance analysis (requires perf)"
	@echo "  test-rdpac         - Build+run the RDPAC parity gate and corner cases"
	@echo "  test-bfstree       - Build+run the BFSTree hypothesis, input-contract and bit-exactness gates"
	@echo "  help               - Show this help message"
