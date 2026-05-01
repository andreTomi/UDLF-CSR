import os
import data_loader
import plots
import tables
import pandas as pd
import sys
import analytical_stats
import shutil
from pathlib import Path
import logging

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

def main():
    # Paths
    base_dir = Path(__file__).resolve().parent.parent
    data_dir = base_dir / "Data"
    viz_dir = data_dir / "visualizations"
    latex_dir = data_dir / "latex_tables"
    
    viz_dir.mkdir(parents=True, exist_ok=True)
    latex_dir.mkdir(parents=True, exist_ok=True)
    logger.info("Output directories configured.")

    # Load and preprocess data
    logger.info("Loading data...")
    df_summary, df_accuracy = data_loader.load_summary_results(data_dir)
    
    if df_summary is not None:
        df_summary = data_loader.preprocess_summary(df_summary)
    else:
        logger.error("Could not load summary results. Exiting.")
        sys.exit(1)
        
    if df_accuracy is not None:
        df_accuracy = data_loader.preprocess_accuracy(df_accuracy)

    # Generate Plots
    logger.info("Generating plots...")
    
    plots.plot_execution_scaling(df_summary, viz_dir)
    plots.plot_thread_scaling(data_dir, viz_dir)
    plots.plot_memory_usage(df_summary, viz_dir)
    plots.plot_microarch_stats(df_summary, viz_dir)
    plots.plot_page_fault_impact(df_summary, viz_dir)
    
    # Out of core comparison plots
    plots.create_comparison_barplots(df_summary, viz_dir)
    plots.create_scalability_lineplots(df_summary, viz_dir)
    plots.create_heatmaps(df_summary, viz_dir)
    plots.create_speedup_analysis(df_summary, viz_dir)
    plots.generate_summary_report(df_summary, viz_dir)

    if df_accuracy is not None:
        plots.plot_accuracy(df_accuracy, viz_dir)
        plots.plot_accuracy_vs_memory(df_accuracy, viz_dir)
        plots.plot_diminishing_returns_L(df_accuracy, viz_dir)
        plots.plot_map_vs_memory_refined(df_accuracy, viz_dir)
        plots.plot_accuracy_vs_L_faceted(df_accuracy, viz_dir)
        plots.plot_param_interaction_heatmaps(df_accuracy, viz_dir)
        plots.plot_efficiency_paradox(df_summary, viz_dir)
        plots.plot_depth_stability(df_accuracy, viz_dir)
        plots.plot_cache_hierarchy_pressure(df_summary, viz_dir)
        plots.plot_branch_prediction_efficiency(df_summary, viz_dir)
        plots.plot_load_intensity(df_summary, viz_dir)
    
    # Generate Tables
    logger.info("Generating LaTeX tables...")
    tables.generate_performance_table(df_summary, latex_dir)
    tables.generate_unified_feasibility_table(df_summary, latex_dir)
    tables.generate_impl_battle_table(df_summary, latex_dir)

    if df_accuracy is not None:
        tables.generate_accuracy_table(df_accuracy, latex_dir)
        tables.generate_thesis_summary_table(df_accuracy, latex_dir)
    
    # Statistical Analysis
    analytical_stats.generate_statistical_report(df_summary, viz_dir)
    
    logger.info("Processing complete.")

if __name__ == "__main__":
    main()
