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
BASE_FLAGS = -std=gnu++23 -O3 -g -fno-omit-frame-pointer -D_GNU_SOURCE 

# Combined flags for maximum performance
CFLAGS = $(BASE_FLAGS) $(VECTOR_FLAGS) $(OPENMP_FLAGS) $(FAST_MATH_FLAGS) $(LOOP_FLAGS) $(INLINE_FLAGS) $(CACHE_FLAGS)

# Define MKL Root for clarity
MKL_ROOT = /opt/intel/oneapi/mkl/latest

# Link flags
LDFLAGS   = -L/usr/lib/x86_64-linux-gnu \
                                          -lzstd -lhwloc -lnuma -lm -luring -lpthread -llz4 -ltbb -ltbbmalloc -ljemalloc \
					  -lgraphblas -lsuitesparseconfig \
						-L$(MKL_ROOT)/lib/intel64 \
						-lmkl_intel_lp64 -lmkl_sequential -lmkl_core \
                                                -ldl

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

# Build targets
.PHONY: all debug profile vectorize clean run run-debug run-profile check-vectorization

# Default target - optimized build
all: udlf

# Main optimized build target
udlf: $(OBJ_FILES)
	@echo "Building optimized executable with vectorization..."
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJ_FILES) -o $(BUILD_DIR)/udlf $(CFLAGS) $(LDFLAGS)
	@echo "Build complete. Executable: $(BUILD_DIR)/udlf"

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
profile: CFLAGS = $(BASE_FLAGS) $(VECTOR_FLAGS) $(OPENMP_FLAGS) -O3 -pg -fno-inline
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
	rm -rf $(OBJ_DIR) $(BUILD_DIR)/udlf $(BUILD_DIR)/udlf-debug $(BUILD_DIR)/udlf-profile $(BUILD_DIR)/udlf-vec
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
	@echo "  help               - Show this help message"
