# Thesis Visualization Suite

This directory contains the Python visualization suite for generating the academic artifacts (PDF plots and LaTeX tables) required for the Master's Thesis. 

## Overview

The visualization suite consumes the raw benchmark CSV results located in the `../Data/` directory and generates:
- **Scalability Plots**: Demonstrating the log-log scaling limits and thread utilization of the `_SPARSE` custom engine.
- **Microarchitectural Profiling**: Instructions, IPC, cache miss rates, and page faults.
- **Accuracy Comparisons**: MAP @ L analysis showing diminishing returns in large-scale retrieval.
- **LaTeX Tables**: Pre-formatted, publication-ready tables summarizing performance, feasibility across hardware environments, and algorithm configurations.

## Directory Structure

```
Visualization/
├── visualizer.py       # Main entry point for generating all artifacts
├── data_loader.py      # Handles reading and preprocessing CSVs from ../Data/
├── plots.py            # Centralized matplotlib/seaborn plotting logic
├── tables.py           # Logic for generating LaTeX tabular environments
└── analytical_stats.py # Statistical report generation
```

## Requirements

Ensure you have the required Python packages installed. From the `Test/` or root directory, install dependencies via:
```bash
pip install -r ../Test/requirements.txt
```

## Usage

To generate all plots and tables, simply execute `visualizer.py` from within this directory:

```bash
cd Visualization/
python visualizer.py
```

### Outputs

The script will automatically dynamically resolve paths and output artifacts into the following directories:
- **Plots**: `../Data/visualizations/*.pdf`
- **Tables**: `../Data/latex_tables/*.tex`

These files are directly consumable by the main LaTeX thesis and presentation templates.
