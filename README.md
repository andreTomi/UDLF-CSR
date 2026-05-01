# UDLF (Unsupervised Distance Learning Framework)

UDLF is a C++ framework for unsupervised distance learning, rank estimation, and image re-ranking tasks.

## Key Features

- **Core Tasks:** Supports Unsupervised Distance Learning (UDL) and multi-feature Fusion.
- **Algorithms:** Extensive support for the following re-ranking methods:
  - **CPRR** (Cartesian Product Reciprocal Rank)
  - **LHRR** (Local High-Order Reciprocal Rank)
- **Linear Algebra Backends:** Multiple implementations for core methods (e.g., CPRR and LHRR):
  - `_SPARSE`: Custom sparse matrix operations
  - `_EIGEN`: Eigen C++ template library
  - `_BLAS`: Basic Linear Algebra Subprograms
  - `_MKL`: Intel Math Kernel Library
- **Input Flexibility:** Handles raw matrices (Distance or Similarity), Ranked Lists (Numeric or String formats), and can dynamically convert matrices to ranked lists using Heap or Insertion sorting.
- **Rich Output & Visualization:** Outputs updated matrices or ranked lists.
- **Built-in Evaluation:** Automated computation of standard retrieval metrics including Precision, MAP (Mean Average Precision), and Recall at various depths.

## Usage

The framework is driven entirely by a configuration file (default: `config.ini`). 

### The `config.ini` File

The configuration file is divided into several sections:
1. **General Settings:** Define the task, method, and dataset size.
2. **Input Settings:** Specify data paths, formats, and sorting methods.
3. **Output Settings:** Configure the format, and paths for the results.
4. **Evaluation Settings:** Toggle efficiency tracking and specific effectiveness metrics (MAP, Precision, Recall).
5. **Method-Specific Parameters:** Fine-tune algorithm behaviors (e.g., `L` for list size, `K` for nearest neighbors, `T` for iterations).

Please refer to the `config.ini` file in the root directory for a fully documented example detailing all available parameters and their valid options.
