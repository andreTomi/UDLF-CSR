# UDLF (Unsupervised Distance Learning Framework)

![C++23](https://img.shields.io/badge/C++-23-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)

UDLF is a high-performance C++ framework designed for unsupervised distance learning, rank estimation, and image re-ranking tasks. This repository serves as the official software artifact for the Master's Thesis titled **"Scaling Manifold Learning through Sparse Matrix Reformulation"**.

In Content-Based Image Retrieval (CBIR), standard manifold re-ranking algorithms typically require an $O(N^2)$ dense similarity matrix, causing Out-Of-Memory (OOM) failures on large datasets (e.g., reaching 102GB of RAM). This framework introduces a custom **Fused Multiply-and-Prune kernel**, reducing memory footprint by **$400\times$** (down to 256MB) and accelerating execution by **$2.4\times$** on 18 physical cores.

---

## The Custom Engine vs. Baselines

Standard manifold re-ranking methods (like CPRR and LHRR) struggle to scale:
- **Dense Baselines:** Require full dense matrices, scaling poorly in both memory ($O(N^2)$) and compute, rendering them unusable for very large image datasets.
- **Sparse Baselines (`_MKL`, `_BLAS`, `_EIGEN`):** While they use sparse matrices, general-purpose sparse libraries suffer from **intermediate density explosion** during Sparse General Matrix-Matrix Multiplication (SpGEMM), ultimately exhausting memory before pruning can occur.
- **The Custom `_SPARSE` Engine:** This repository introduces a purpose-built high-performance engine utilizing the Compressed Sparse Row (CSR) format. By incorporating a **fused multiply-and-prune SpGEMM kernel**, a custom BlockAllocator with Transparent Huge Pages (THP), and 128-byte cache-line padding, the engine actively controls intermediate density explosion. This guarantees strictly bounded memory consumption while outperforming established math libraries.

---

## Prerequisites & System Requirements

To ensure full performance and reproducibility, the framework must be executed in a Linux environment. The following dependencies are strictly required:

- **OS:** Linux
- **Compiler:** `g++` with **C++23** support
- **Math Libraries:** 
  - Intel oneAPI Math Kernel Library (MKL)
  - SuiteSparse (specifically GraphBLAS)
- **Concurrency & Memory Management:** 
  - `tbb` (Intel Threading Building Blocks)
  - `jemalloc`

---

## Build Instructions

The project is built using the provided `Makefile`. 

1. Ensure the `MKL_ROOT` environment variable is correctly set to your Intel oneAPI installation path. By default, it looks for:
   ```bash
   export MKL_ROOT=/opt/intel/oneapi/mkl/latest
   ```

2. Compile the framework:
   ```bash
   cd Source/
   make all
   ```
   This will build two versions of the engine in `Source/bin/`:
   - **`udlf`**: Standard version using memory-mapped files (MMAP). Recommended for large datasets.
   - **`udlf_swap`**: Version using anonymous memory (Swap-backed). Used for dense implementation baseline comparisons.

---

## Configuration & Usage

The framework's execution is driven entirely by the `config.ini` file located in the root directory. 

### Method Selection Example
You can switch between the baselines and the custom sparse engine by modifying the `UDL_METHOD` variable. For example, to compare the standard MKL baseline with the custom Sparse engine for Local High-Order Reciprocal Rank (LHRR):

**Using Intel MKL:**
```ini
UDL_METHOD = LHRR_MKL
PARAM_LHRR_MKL_L = 1000
PARAM_LHRR_MKL_K = 20
```

**Using the Custom Sparse Engine:**
```ini
UDL_METHOD = LHRR_SPARSE
PARAM_LHRR_SPARSE_L = 1000
PARAM_LHRR_SPARSE_K = 20
```
*(Where `L` represents the neighborhood list length and `K` is the number of nearest neighbors).*

### Hardware Isolation
To achieve the exact scaling results presented in the thesis, the execution environment must be strictly controlled to prevent CPU migration and NUMA remote-node access. **Hardware isolation is handled externally** via OS-level tools. 

To execute the framework manually:
```bash
# From the project root
numactl --membind=0 taskset -c 0-17 ./Source/bin/udlf
```

---

## Benchmarking & Profiling

The `Test/` directory contains a comprehensive suite for automated performance evaluation and hardware metric collection. These scripts were used to gather the raw data presented in the thesis.

### Key Scripts
- **`validate_setup.py`**: Verifies the environment by checking for required Python packages, system tools (`perf`, `numactl`), and compiled binaries. It also estimates the total execution time for a full benchmark run.
- **`run_experiments.py`**: The primary orchestrator for benchmarking. It executes a full matrix of configurations (different methods, backends, and repetitions) and utilizes `perf` to collect hardware counters (cache misses, IPC, branches) and `time` for peak memory and CPU usage.
- **`run_scalability.py`**: Automates multi-threading scalability tests. It runs the benchmark suite across varying thread counts (e.g., 1 to 18) while enforcing CPU affinity and NUMA isolation to evaluate the speedup of the `_SPARSE` engine.

### How to Run
To ensure the system is correctly configured for benchmarking, first run the validation script:
```bash
python Test/src/validate_setup.py
```

To execute the full benchmarking suite:
```bash
# Example: Running benchmarks with results saved to 'my_benchmark_run'
python Test/src/run_experiments.py --output-dir my_benchmark_run
```

All results are exported to CSV and JSON formats, which are then processed by the `Visualization/` suite to produce the final thesis artifacts.

---

## Project Structure

The repository is organized as follows:

- **Source/**: Custom C++ engine source code and the `Makefile`.
- **Data/**: Raw benchmark CSV results, generated PDF plots (`visualizations/`), and LaTeX tables (`latex_tables/`).
- **Test/**: Python scripts for automated benchmarking and hardware metric collection.
- **Visualization/**: Python suite for processing benchmark data and generating publication-ready plots and tables.

---

## Reproducibility & Artifacts

This repository is designed for full transparency and reproducibility of the thesis results. 

- **Raw Data:** All raw benchmark data, hardware counters, and execution times are included in the `Data/` directory.
- **Artifact Generation:** The Python scripts in the `Visualization/` directory can be used to regenerate all PDF plots and LaTeX tables directly from the raw data.

To regenerate all visualizations, run:
```bash
cd Visualization/
python visualizer.py
```

By using the raw data and scripts, examiners and researchers can seamlessly regenerate the figures demonstrating the $400\times$ memory reduction and $2.4\times$ speedup.
