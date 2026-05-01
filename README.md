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
   make
   ```

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

Example running on specific physical cores:
```bash
numactl --membind=0 taskset -c 0-17 ./udlf
```

---

## Reproducibility & Artifacts

This repository is designed for full transparency and reproducibility of the thesis results. 

- **Raw Data:** All raw benchmark data, hardware counters, and execution times (in `.csv` format) are included in the `data/` directory.
- **Plotting Scripts:** The exact Python plotting scripts used to generate the scaling and memory metric charts in the thesis are available in the `scripts/` directory.

By using the raw data and scripts, examiners and researchers can seamlessly regenerate the figures demonstrating the $400\times$ memory reduction and $2.4\times$ speedup.
