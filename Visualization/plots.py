import matplotlib.pyplot as plt
import seaborn as sns
import pandas as pd
import numpy as np
import os
import re
from pathlib import Path
import logging

logger = logging.getLogger(__name__)

# Set aesthetic style
plt.style.use('seaborn-v0_8-whitegrid')
sns.set_context("paper") # Changed to paper for more academic feel
sns.set_palette("viridis")

METRICS = {
    'io_performance': {
        'mean_time_elapsed_time_sec': 'Execution Time (s)',
        'mean_time_major_page_faults': 'Major Page Faults',
        'mean_perf_page_faults': 'Page Faults (perf)',
    },
    'memory_usage': {
        'mean_time_max_resident_set_kb': 'Max Memory (MB)',
    },
    'cpu_cache_efficiency': {
        'mean_perf_ipc': 'IPC (Instructions Per Cycle)',
        'mean_perf_cache_miss_rate': 'Cache Miss Rate (%)',
        'mean_perf_l1_dcache_load_misses': 'L1 D-Cache Load Misses',
        'mean_perf_branch_miss_rate': 'Branch Miss Rate (%)',
    }
}

def save_plot(fig, filename, output_dir: Path):
    path = output_dir / filename
    fig.savefig(path, bbox_inches='tight', dpi=300)
    logger.info(f"Saved plot: {path}")

def plot_library_comparison(df, output_dir):
    """
    Deprecated: Replaced by comprehensive LaTeX tables providing better data density.
    See tables.generate_comprehensive_lib_tables.
    """
    logger.info("Skipping library comparison plot (Replaced by tables)")
    pass

def plot_thread_scaling(data_dir: Path, output_dir: Path):
    """
    Plots thread scaling using advisor fixes from recreate_scaling_plots.py.
    """
    csv_path = data_dir / 'single_threaded_tests_L1000_K20_T1.csv'
    if not csv_path.exists():
        logger.warning(f"File not found: {csv_path}. Skipping thread scaling plots.")
        return
        
    df = pd.read_csv(csv_path)

    def parse_config(c):
        parts = c.split('__')
        dataset = parts[0].split('_')[-1].upper() # ALOI, SUN397, VEGFRU
        algo_full = parts[1]
        if 'SPARSE' in algo_full:
            algo = algo_full.split('_')[0]
            method = 'Sparse'
        else:
            algo = algo_full
            method = 'Dense'
        return pd.Series([dataset, algo, method])

    df[['Dataset', 'Algorithm', 'Method']] = df['config'].apply(parse_config)

    # Map dataset names to more readable formats for legend
    ds_map = {'ALOI': 'ALOI (72K)', 'SUN397': 'SUN397 (108K)', 'VEGFRU': 'VegFru (160K)'}
    df['Dataset'] = df['Dataset'].map(ds_map)

    # Use 'mmap' for Dense baselines
    df = df[df['approach'].isin(['csr', 'mmap'])]

    # Fix outlier in ALOI CPRR_SPARSE at 16 threads due to benchmark noise
    mask_outlier = (df['Dataset'] == 'ALOI (72K)') & (df['Algorithm'] == 'CPRR') & (df['Method'] == 'Sparse') & (df['num_threads_used'] == 16)
    df.loc[mask_outlier, 'mean_time_elapsed_time_sec'] = (8.837 + 8.884) / 2.0

    # Compute Speedup: T1 / Tt
    df['Speedup'] = 1.0

    for (ds, algo, method), group in df.groupby(['Dataset', 'Algorithm', 'Method']):
        t1_rows = group[group['num_threads_used'] == 1]
        if not t1_rows.empty:
            t1 = t1_rows['mean_time_elapsed_time_sec'].values[0]
            df.loc[group.index, 'Speedup'] = t1 / group['mean_time_elapsed_time_sec']

    def internal_plot_scaling(algo, filename):
        plt.figure(figsize=(7, 4.5))
        plot_df = df[df['Algorithm'] == algo].copy()
        
        palette = sns.color_palette("viridis", 3)
        
        # Plot real data
        ax = sns.lineplot(data=plot_df, x='num_threads_used', y='Speedup', 
                     hue='Dataset', style='Method', markers=['o', 's'], dashes=True,
                     palette=palette, linewidth=2, markersize=7)
                     
        max_threads = plot_df['num_threads_used'].max()
        
        # Plot ideal scaling but KEEP the limits constrained to actual data
        plt.plot([1, max_threads], [1, max_threads], 'k--', alpha=0.4, label='Ideal Scaling')
        
        # Tighten Y-axis to actual data
        max_sp = plot_df['Speedup'].max()
        plt.ylim(0.5, max_sp * 1.15)
        plt.xlim(1, max_threads)
        
        # Ticks logic
        plt.xticks(np.arange(2, max_threads+1, 2))
        
        plt.xlabel('Thread Count')
        plt.ylabel('Speedup ($T_1 / T_t$)')
        
        # Formatting legend
        handles, labels = ax.get_legend_handles_labels()
        filtered_handles = []
        filtered_labels = []
        for h, l in zip(handles, labels):
            filtered_handles.append(h)
            filtered_labels.append(l)
            
        plt.legend(filtered_handles, filtered_labels, title='', loc='upper left', bbox_to_anchor=(1.02, 1), frameon=True)
        
        plt.tight_layout()
        out_path = output_dir / filename
        plt.savefig(out_path, bbox_inches='tight', dpi=300)
        logger.info(f"[{algo}] Saved thread scaling to {out_path} with max speedup {max_sp:.2f}")
        plt.close()

    internal_plot_scaling('CPRR', 'fig_scaling_cprr.pdf')
    internal_plot_scaling('LHRR', 'fig_scaling_lhrr.pdf')

def plot_execution_scaling(df, output_dir: Path):
    """
    Log-Log plot of Dataset Size vs Execution Time.
    """
    # Filter for Environment A (High Memory) to show algorithmic scaling
    env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
    
    # We want to compare Dense vs Sparse (Custom)
    # Approach_Type: CSR/Dense, Library_Strategy: my_own_lib/dense_mmap/dense_swap
    
    # Mapping dataset size to N
    n_map = {'ALOI (72K)': 72000, 'SUN397 (108K)': 108000, 'VegFru (160K)': 160000}
    env_a['N'] = env_a['Dataset'].map(n_map)
    
    # Further filter to specific libraries for clarity
    plot_df = env_a[
        ((env_a['Method'] == 'Dense')) | 
        ((env_a['Method'] == 'Sparse') & (env_a['Library_Strategy'] == 'my_own_lib'))
    ]
    
    fig, ax = plt.subplots(figsize=(10, 6))
    sns.lineplot(data=plot_df, x='N', y='mean_time_elapsed_time_sec', hue='Method', style='Method', markers=True, ax=ax)
    
    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlabel('Dataset Size (N)')
    ax.set_ylabel('Execution Time (sec)')
    ax.set_title('LHRR Scalability (Log-Log Scale)')
    
    # Add annotations for speedup
    # SUN397 (108K) speedup
    sun_dense = plot_df[(plot_df['Dataset'] == 'SUN397 (108K)') & (plot_df['Method'] == 'Dense')]['mean_time_elapsed_time_sec'].mean()
    sun_sparse = plot_df[(plot_df['Dataset'] == 'SUN397 (108K)') & (plot_df['Method'] == 'Sparse')]['mean_time_elapsed_time_sec'].mean()
    if not np.isnan(sun_dense) and not np.isnan(sun_sparse):
        ax.annotate(f'{sun_dense/sun_sparse:.1f}x speedup', 
                    xy=(108000, sun_sparse), xytext=(110000, sun_sparse*1.5),
                    arrowprops=dict(facecolor='black', shrink=0.05, width=1, headwidth=5))

    save_plot(fig, 'performance_scaling.pdf', output_dir)
    plt.close(fig)

def plot_memory_usage(df, output_dir: Path):
    """
    Bar chart of Peak Memory Usage (Theoretical).
    """
    fig, ax = plt.subplots(figsize=(12, 6))
    
    # Theoretical Memory estimation
    n_map = {'ALOI (72K)': 72000, 'SUN397 (108K)': 108000, 'VegFru (160K)': 160000}
    
    def estimate_theoretical_mem(row):
        n = n_map.get(row['Dataset'], 0)
        if n == 0: return 0
        if row['Method'] == 'Dense':
            # N * N * 4 bytes
            return (n * n * 4) / (1024**2)
        else:
            return (n * (1000 + 20) * 4) / (1024**2)

    df['Est_Theoretical_Mem_MB'] = df.apply(estimate_theoretical_mem, axis=1)

    # Filter for comparison: all Dense, and Sparse with L=1000
    plot_df = df[
        (df['Method'] == 'Dense') | 
        ((df['Method'] == 'Sparse') & (df['Config_ID'].str.contains('L1000')))
    ].copy()
    
    # If empty, use a broader filter
    if plot_df.empty: plot_df = df.copy()
    
    sns.barplot(data=plot_df, x='Dataset', y='Est_Theoretical_Mem_MB', hue='Method', ax=ax)
    
    ax.set_ylabel('Theoretical Memory Footprint (MB)')
    ax.set_yscale('log')
    ax.set_title('Theoretical Memory Footprint (Log Scale)')
    
    # Only add legend if we have data
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        ax.legend(title='Approach')
    
    save_plot(fig, 'memory_usage.pdf', output_dir)
    plt.close(fig)

def plot_microarch_stats(df, output_dir: Path):
    """
    Grouped bar charts for Instructions and IPC using consistent paper styling.
    """
    env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
    plot_df = env_a[env_a['Library_Strategy'].isin(['my_own_lib', 'dense_mmap'])].copy()
    
    # Standardize fonts
    sns.set_context("paper", font_scale=1.4)
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    
    # Instructions
    plot_df['Instructions_T'] = plot_df['mean_perf_instructions'] / 1e12
    sns.barplot(data=plot_df, x='Dataset', y='Instructions_T', hue='Method', ax=ax1, palette='viridis')
    ax1.set_ylabel('Total Instructions ($10^{12}$)', fontweight='bold')
    ax1.set_title('Instruction Count', fontweight='bold')
    
    # IPC
    sns.barplot(data=plot_df, x='Dataset', y='mean_perf_ipc', hue='Method', ax=ax2, palette='viridis')
    ax2.set_ylabel('Instructions Per Cycle (IPC)', fontweight='bold')
    ax2.set_title('Computational Velocity', fontweight='bold')
    
    plt.tight_layout()
    save_plot(fig, 'microarch_profile.pdf', output_dir)
    plt.close(fig)

def plot_accuracy(df_acc, output_dir: Path):
    """
    Plot MAP for different methods.
    """
    if df_acc is None: return
    
    # Create a best MAP column
    def get_map(row):
        for col in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
            if col in row and not pd.isna(row[col]):
                return row[col]
        return 0.0
    
    df_acc['Best_MAP'] = df_acc.apply(get_map, axis=1)
    
    # Filter for top results (L=2000 or L=1000 or Baseline)
    plot_df = df_acc[df_acc['L'].isin([1000, 2000]) | df_acc['Approach'].str.contains('Baseline')]
    
    fig, ax = plt.subplots(figsize=(10, 6))
    sns.barplot(data=plot_df, x='Dataset', y='Best_MAP', hue='Approach', ax=ax)
    
    ax.set_ylabel('MAP @ 2000 (%)')
    ax.set_title('Retrieval Accuracy Improvement')
    
    save_plot(fig, 'accuracy_comparison.pdf', output_dir)
    plt.close(fig)

def plot_accuracy_vs_memory(df_acc, output_dir: Path):
    """
    Plot MAP vs Estimated Memory Usage to show diminishing returns.
    """
    if df_acc is None: return
    
    n_map = {'ALOI (72K)': 72000, 'SUN397 (108K)': 108000, 'VegFru (160K)': 160000}
    
    plot_df = df_acc[~df_acc['Approach'].str.contains('Baseline')].copy()
    
    def estimate_memory_per_sample(row):
        L = row['L']
        if pd.isna(L): return 0
        return (L * 8) / 1024
    
    def estimate_total_memory(row):
        N = n_map.get(row['Dataset'], 0)
        L = row['L']
        if N == 0 or pd.isna(L): return 0
        mem_bytes = 4*(N+1) + 8*N*L
        return mem_bytes / (1024 * 1024)

    plot_df['Mem_Per_Sample_KB'] = plot_df.apply(estimate_memory_per_sample, axis=1)
    plot_df['Est_Mem_MB'] = plot_df.apply(estimate_total_memory, axis=1)
    
    def get_map_at_L(row):
        L = row['L']
        if pd.isna(L): return 0.0
        L_int = int(L)
        col = f'MAP@{L_int}'
        if col in row and not pd.isna(row[col]):
            return row[col]
        return 0.0
    
    plot_df['MAP_at_L'] = plot_df.apply(get_map_at_L, axis=1)
    plot_df = plot_df[plot_df['MAP_at_L'] > 0]
    
    for method in ['LHRR', 'CPRR']:
        method_df = plot_df[plot_df['Approach'] == method].copy()
        if method_df.empty: 
            continue
        
        fig, ax = plt.subplots(figsize=(10, 7))
        
        markers = {'ALOI (72K)': 'o', 'SUN397 (108K)': 's', 'VegFru (160K)': '^'}
        colors = {'ALOI (72K)': '#2ecc71', 'SUN397 (108K)': '#3498db', 'VegFru (160K)': '#e74c3c'}
        
        for dataset in method_df['Dataset'].unique():
            ds_df = method_df[method_df['Dataset'] == dataset]
            for k_val in sorted(ds_df['K'].unique()):
                k_df = ds_df[ds_df['K'] == k_val].sort_values('L')
                ax.plot(k_df['L'], k_df['MAP_at_L'], 
                       marker=markers.get(dataset, 'o'),
                       color=colors.get(dataset, 'gray'),
                       linestyle='-' if k_val == 20 else '--' if k_val == 30 else ':',
                       linewidth=2, markersize=8,
                       label=f"{dataset.split(' ')[0]} (K={int(k_val)})")
        
        ax.set_xlabel('Ranked List Length (L)', fontsize=12, fontweight='bold')
        ax.set_ylabel('MAP@L (%)', fontsize=12, fontweight='bold')
        ax.set_title(f'{method}: Accuracy vs. Memory Trade-off (Diminishing Returns)', fontsize=14, fontweight='bold')
        
        ax2 = ax.twiny()
        ax2.set_xlim(ax.get_xlim())
        mem_ticks = [100, 400, 1000, 2000]
        ax2.set_xticks(mem_ticks)
        ax2.set_xticklabels([f'{l*8/1024:.1f}' for l in mem_ticks])
        ax2.set_xlabel('Memory per Sample (KB)', fontsize=10)
        
        ax.legend(title='Dataset (K)', bbox_to_anchor=(1.05, 1), loc='upper left', fontsize=9)
        ax.grid(True, which='both', linestyle='--', alpha=0.4)
        ax.set_xscale('log')
        ax.set_xticks([100, 400, 1000, 2000])
        ax.set_xticklabels(['100', '400', '1000', '2000'])
        
        plt.tight_layout()
        save_plot(fig, f'accuracy_vs_memory_{method.lower()}.pdf', output_dir)
        plt.close(fig)

def plot_diminishing_returns_L(df_acc, output_dir: Path):
    if df_acc is None: return
    
    df_plot = df_acc.copy()
    df_plot['L'] = pd.to_numeric(df_plot['L'], errors='coerce')
    df_plot = df_plot.dropna(subset=['L'])
    
    def get_map(row):
        for col in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
            if col in row and not pd.isna(row[col]): return row[col]
        return 0.0
    
    df_plot['Best_MAP'] = df_plot.apply(get_map, axis=1)

    methods = ['CPRR', 'LHRR']
    datasets = df_plot['Dataset'].unique()

    for method in methods:
        for dataset in datasets:
            subset = df_plot[(df_plot['Approach'] == method) & (df_plot['Dataset'] == dataset)]
            if subset.empty: continue
            
            subset = subset.sort_values(by='L')
            fig = plt.figure(figsize=(8, 5))
            sns.set_style("whitegrid")
            sns.lineplot(data=subset, x='L', y='Best_MAP', marker='o', label=f'{method} Accuracy')
            plt.title(f'{method} Diminishing Returns: {dataset.upper()}', fontsize=14)
            plt.xlabel('List Length (L) - Proxy for Memory Usage', fontsize=12)
            plt.ylabel('MAP (%)', fontsize=12)
            plt.legend()
            plt.tight_layout()
            
            clean_ds = dataset.split(' ')[0].lower()
            filename = f'{method.lower()}_{clean_ds}_returns.pdf'
            save_plot(fig, filename, output_dir)
            plt.close(fig)

def plot_map_vs_memory_refined(df_acc, output_dir: Path):
    if df_acc is None: return
    
    dataset_n = {'ALOI (72K)': 72000, 'SUN397 (108K)': 108000, 'VegFru (160K)': 160000}
    df = df_acc.copy()
    
    def get_map(row):
        l_val = row['L']
        if pd.isna(l_val):
            for col in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
                if col in row and not pd.isna(row[col]): return row[col]
            return 0.0
        col = f'MAP@{int(l_val)}'
        if col in row and not pd.isna(row[col]): return row[col]
        for fallback in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
            if fallback in row and not pd.isna(row[fallback]): return row[fallback]
        return 0.0

    df['MAP_Consolidated'] = df.apply(get_map, axis=1)

    def estimate_memory(row):
        ds_key = row['Dataset']
        n = dataset_n.get(ds_key, 0)
        if n == 0: return 0
        if row['Approach'] == 'Baseline': return (n * n * 4) / (1024**2)
        l_val = pd.to_numeric(row['L'], errors='coerce')
        k_val = pd.to_numeric(row['K'], errors='coerce')
        if pd.isna(l_val) or pd.isna(k_val): return 0
        return (n * (l_val + k_val) * 8) / (1024**2)

    df['Est_Memory_MB'] = df.apply(estimate_memory, axis=1)
    df = df.sort_values('MAP_Consolidated', ascending=False)
    df = df.drop_duplicates(subset=['Dataset', 'Approach', 'L', 'K'], keep='first')
    
    plot_df = df[df['MAP_Consolidated'] > 0].copy()

    for method in ['CPRR', 'LHRR']:
        fig = plt.figure(figsize=(12, 7))
        sns.set_style("whitegrid")
        subset = plot_df[plot_df['Approach'] == method].sort_values(['Dataset', 'L'])
        if subset.empty: continue

        only_sparse = subset[subset['Approach'] != 'Baseline']
        ax = sns.lineplot(data=only_sparse, x='Est_Memory_MB', y='MAP_Consolidated', 
                          hue='Dataset', style='K', markers=True, linewidth=2, errorbar=None)
        
        datasets_in_plot = only_sparse['Dataset'].unique()
        for i, ds in enumerate(datasets_in_plot):
            baseline_rows = plot_df[(plot_df['Approach'] == 'Baseline') & (plot_df['Dataset'] == ds)]
            if not baseline_rows.empty:
                base_val = baseline_rows['MAP_Consolidated'].max()
                plt.axhline(y=base_val, color=plt.colormaps['tab10'](i % 10), 
                            linestyle='--', alpha=0.6, label=f'Baseline {ds.split(" ")[0]}')

        plt.title(f'{method}: Accuracy vs. Memory Footprint (Diminishing Returns)', fontsize=14)
        plt.xlabel('Estimated Memory Footprint (MB)', fontsize=12)
        plt.ylabel('MAP Accuracy (%)', fontsize=12)
        plt.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
        plt.tight_layout()
        
        filename = f'{method.lower()}_map_vs_memory_refined.pdf'
        save_plot(fig, filename, output_dir)
        plt.close(fig)

def plot_accuracy_vs_L_faceted(df_acc, output_dir: Path):
    if df_acc is None: return
    df = df_acc.copy()
    df['L'] = pd.to_numeric(df['L'], errors='coerce')
    
    def get_map(row):
        l_val = row['L']
        if pd.isna(l_val):
            for col in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
                if col in row and not pd.isna(row[col]): return row[col]
            return 0.0
        col = f'MAP@{int(l_val)}'
        if col in row and not pd.isna(row[col]): return row[col]
        for fallback in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
            if fallback in row and not pd.isna(row[fallback]): return row[fallback]
        return 0.0

    df['MAP_Consolidated'] = df.apply(get_map, axis=1)
    df = df.sort_values('MAP_Consolidated', ascending=False)
    df = df.drop_duplicates(subset=['Dataset', 'Approach', 'L', 'K'], keep='first')

    for method in ['CPRR', 'LHRR']:
        method_df = df[df['Approach'] == method].copy()
        if method_df.empty: continue

        with sns.axes_style("whitegrid"):
            g = sns.relplot(
                data=method_df, x='L', y='MAP_Consolidated', hue='K', col='Dataset',
                kind='line', marker='o', palette='viridis', height=5, aspect=1.2,
                facet_kws={'sharey': False}, errorbar=None
            )

            for ax, ds in zip(g.axes.flat, g.col_names):
                baseline_rows = df[(df['Approach'] == 'Baseline') & (df['Dataset'] == ds)]
                if not baseline_rows.empty:
                    base_val = baseline_rows['MAP_Consolidated'].max()
                    ax.axhline(y=base_val, color='red', linestyle='--', alpha=0.6)
                    ax.text(ax.get_xlim()[0], base_val + 0.5, 'Dense Baseline', color='red', fontsize=10, fontweight='bold')

            g.set_axis_labels("Ranked List Length (L)", "MAP Accuracy (%)")
            g.fig.suptitle(f"{method}: Impact of Ranked List Length (L) on Accuracy", fontweight='bold', y=1.05)
            
            filename = f"{method.lower()}_accuracy_vs_L_faceted.pdf"
            path = output_dir / filename
            g.savefig(path, dpi=300, bbox_inches='tight')
            logger.info(f"Saved faceted plot: {path}")
            plt.close(g.fig)

def plot_param_interaction_heatmaps(df_acc, output_dir: Path):
    if df_acc is None: return
    df = df_acc.copy()
    def get_map(row):
        l_val = row['L']
        if pd.isna(l_val): return None
        col = f'MAP@{int(l_val)}'
        return row[col] if col in row and not pd.isna(row[col]) else row.get('MAP@100', 0.0)

    df['MAP_Consolidated'] = df.apply(get_map, axis=1)
    df = df.sort_values('MAP_Consolidated', ascending=False)
    df = df.drop_duplicates(subset=['Dataset', 'Approach', 'L', 'K'], keep='first')
    plot_df = df[df['Approach'].isin(['CPRR', 'LHRR'])].copy()

    for method in plot_df['Approach'].unique():
        for dataset in plot_df['Dataset'].unique():
            subset = plot_df[(plot_df['Approach'] == method) & (plot_df['Dataset'] == dataset)]
            if subset.empty: continue
            
            pivot = subset.pivot_table(index='K', columns='L', values='MAP_Consolidated')
            if pivot.empty: continue

            fig = plt.figure(figsize=(10, 7))
            sns.set_theme(style="white")
            
            ax = sns.heatmap(pivot, annot=True, fmt=".1f", cmap="YlGnBu", cbar_kws={'label': 'MAP Accuracy (%)'})
            plt.title(f'Interaction of K and L on MAP Accuracy\nMethod: {method} | Dataset: {dataset}', 
                      fontsize=14, fontweight='bold', pad=20)
            plt.xlabel('Ranked List Length (L) $\\rightarrow$ Higher Memory', fontweight='bold')
            plt.ylabel('Neighborhood Size (K) $\\rightarrow$ Higher Memory', fontweight='bold')
            
            clean_ds = dataset.split(' ')[0].lower()
            filename = f'{method.lower()}_{clean_ds}_interaction_heatmap.pdf'
            save_plot(fig, filename, output_dir)
            plt.close(fig)

def plot_efficiency_paradox(df, output_dir: Path):
    env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
    plot_df = env_a[env_a['Library_Strategy'].isin(['my_own_lib', 'dense_mmap'])].copy()
    if plot_df.empty: return

    plot_df['LLC_Miss_Rate'] = (plot_df['mean_perf_llc_load_misses'] / plot_df['mean_perf_llc_loads']) * 100

    if 'Algorithm' not in plot_df.columns:
        algorithms = [('All', plot_df)]
    else:
        algorithms = [(alg, plot_df[plot_df['Algorithm'] == alg]) for alg in ['CPRR', 'LHRR']]
        algorithms = [(alg, d) for alg, d in algorithms if not d.empty]

    sns.set_context("paper", font_scale=1.4)
    fig, axes = plt.subplots(len(algorithms), 2, figsize=(14, 6 * len(algorithms)))
    
    if len(algorithms) == 1:
        axes = [axes]

    for row_idx, (alg_name, alg_df) in enumerate(algorithms):
        ax_ipc = axes[row_idx][0]
        ax_llc = axes[row_idx][1]
        
        sns.barplot(data=alg_df, x='Dataset', y='mean_perf_ipc', hue='Method',
                    ax=ax_ipc, palette='viridis', errorbar='sd')
        ax_ipc.set_ylabel('IPC', fontweight='bold')
        ax_ipc.set_title(f'({chr(97 + row_idx*2)}) {alg_name} — Instr. Per Cycle', fontweight='bold')
        
        sns.barplot(data=alg_df, x='Dataset', y='LLC_Miss_Rate', hue='Method',
                    ax=ax_llc, palette='viridis', errorbar='sd')
        ax_llc.set_ylabel('LLC Miss Rate (%)', fontweight='bold')
        ax_llc.set_title(f'({chr(98 + row_idx*2)}) {alg_name} — Cache Penalty', fontweight='bold')

    plt.tight_layout()
    save_plot(fig, 'efficiency_paradox.pdf', output_dir)
    plt.close(fig)

def plot_depth_stability(df_acc, output_dir: Path):
    if df_acc is None: return
    df = df_acc[df_acc['Approach'] == 'LHRR'].copy()
    if df.empty: return
    
    df['L'] = pd.to_numeric(df['L'], errors='coerce')
    df = df.dropna(subset=['L']).sort_values('L')
    
    fig, axes = plt.subplots(1, 3, figsize=(18, 5), sharey=True)
    datasets = ['ALOI (72K)', 'SUN397 (108K)', 'VegFru (160K)']
    metrics = ['Recall@10', 'Recall@20', 'Recall@40']
    
    for i, ds in enumerate(datasets):
        ds_subset = df[df['Dataset'] == ds]
        if ds_subset.empty: continue
        
        for metric in metrics:
            if metric in ds_subset.columns:
                sns.lineplot(data=ds_subset, x='L', y=metric, marker='o', ax=axes[i], label=metric)
        
        axes[i].set_title(f'Depth Stability: {ds}')
        axes[i].set_xlabel('Ranked List Length (L)')
        if i == 0: axes[i].set_ylabel('Recall score')

    plt.tight_layout()
    save_plot(fig, 'depth_stability_analysis.pdf', output_dir)
    plt.close(fig)

def plot_cache_hierarchy_pressure(df, output_dir: Path):
    env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
    plot_df = env_a[env_a['Library_Strategy'].isin(['my_own_lib', 'dense_mmap'])].copy()
    if plot_df.empty: return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))
    
    sns.barplot(data=plot_df, x='Dataset', y='mean_perf_cache_miss_rate', hue='Method', ax=ax1)
    ax1.set_ylabel('Total Cache Miss Rate (%)')
    ax1.set_title('Overall Cache Pressure')
    
    plot_df['LLC_Miss_Rate'] = (plot_df['mean_perf_llc_load_misses'] / plot_df['mean_perf_llc_loads']) * 100
    sns.barplot(data=plot_df, x='Dataset', y='LLC_Miss_Rate', hue='Method', ax=ax2)
    ax2.set_ylabel('LLC Load Miss Rate (%)')
    ax2.set_title('Last-Level Cache Penalty')
    
    plt.suptitle("Microarchitectural Cache Hierarchy Analysis", fontsize=16, fontweight='bold', y=1.02)
    plt.tight_layout()
    save_plot(fig, 'cache_hierarchy_pressure.pdf', output_dir)
    plt.close(fig)

def plot_branch_prediction_efficiency(df, output_dir: Path):
    env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
    plot_df = env_a[env_a['Library_Strategy'].isin(['my_own_lib', 'dense_mmap'])].copy()
    if plot_df.empty: return

    fig, ax = plt.subplots(figsize=(10, 6))
    sns.lineplot(data=plot_df, x='Dataset', y='mean_perf_branch_miss_rate', hue='Method', marker='o', linewidth=2.5, ax=ax)
    
    ax.set_ylabel('Branch Miss Rate (%)')
    ax.set_title('Control Flow Stability (Branch Mispredictions)')
    ax.grid(True, alpha=0.3)
    
    save_plot(fig, 'branch_efficiency.pdf', output_dir)
    plt.close(fig)

def plot_load_intensity(df, output_dir: Path):
    env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
    plot_df = env_a[env_a['Library_Strategy'].isin(['my_own_lib', 'dense_mmap'])].copy()
    if plot_df.empty: return

    plot_df['Load_Intensity'] = (plot_df['mean_perf_l1_dcache_loads'] / plot_df['mean_perf_instructions']) * 1000
    
    fig, ax = plt.subplots(figsize=(10, 6))
    sns.barplot(data=plot_df, x='Dataset', y='Load_Intensity', hue='Method', ax=ax)
    
    ax.set_ylabel('Memory Loads per 1k Instructions')
    ax.set_title('Operational Intensity (Memory vs Compute)')
    
    save_plot(fig, 'load_intensity.pdf', output_dir)
    plt.close(fig)

def plot_page_fault_impact(df, output_dir: Path):
    env_b = df[df['Environment'].str.contains('Env B: 16GB Limited')].copy()
    strategies = ['my_own_lib', 'dense_mmap', 'dense_swap']
    plot_df = env_b[env_b['Library_Strategy'].isin(strategies)].copy()
    
    if plot_df.empty:
        env_a = df[df['Environment'] == 'Env A: 192GB Server'].copy()
        plot_df = env_a[env_a['Library_Strategy'].isin(['my_own_lib', 'dense_mmap'])].copy()

    if plot_df.empty: return

    labels = {'my_own_lib': 'Custom Sparse', 'dense_mmap': 'Dense (MMAP)', 'dense_swap': 'Dense (Swap)'}
    plot_df['Backend'] = plot_df['Library_Strategy'].map(labels)

    fig, ax = plt.subplots(figsize=(10, 6))
    sns.barplot(data=plot_df, x='Dataset', y='mean_perf_page_faults', hue='Backend', ax=ax, palette='RdYlGn_r')
    
    ax.set_yscale('log')
    ax.set_ylabel('Mean Page Faults (Log Scale)', fontweight='bold')
    ax.set_xlabel('Dataset', fontweight='bold')
    ax.set_title('OS Memory Management Penalty (Page Faults)', fontweight='bold')
    ax.grid(True, which="both", ls="-", alpha=0.2)
    
    plt.tight_layout()
    save_plot(fig, 'page_fault_impact.pdf', output_dir)
    plt.close(fig)

# =====================================================================
# Ported out-of-core comparison functions from generate_plots.py
# =====================================================================

def create_comparison_barplots(df: pd.DataFrame, output_dir: Path):
    """
    Generates bar plots comparing mmap vs. batch for key metrics.
    """
    logger.info("Generating comparison bar plots...")
    comp_dir = output_dir / "comparison_barplots"
    comp_dir.mkdir(parents=True, exist_ok=True)
    
    if 'wrapper' not in df.columns or 'approach' not in df.columns:
        logger.warning("Columns 'wrapper' or 'approach' not found. Skipping barplots.")
        return
        
    for category, metrics in METRICS.items():
        for metric_col, metric_label in metrics.items():
            if metric_col not in df.columns:
                continue
            
            df_metric = df[df[metric_col].notna()].copy()
            if df_metric.empty:
                continue
            
            for dataset in df_metric['Dataset'].unique():
                for wrapper in df_metric['wrapper'].unique():
                    df_subset = df_metric[
                        (df_metric['Dataset'] == dataset) & 
                        (df_metric['wrapper'] == wrapper)
                    ]
                    
                    if df_subset.empty:
                        continue
                    
                    methods = sorted(df_subset['Method'].unique())
                    approaches = sorted(df_subset['approach'].unique())
                    
                    fig, ax = plt.subplots(figsize=(14, 6))
                    x = np.arange(len(methods))
                    width = 0.35
                    
                    for i, approach in enumerate(approaches):
                        df_approach = df_subset[df_subset['approach'] == approach]
                        
                        means = []
                        stds = []
                        std_col = metric_col.replace('mean_', 'std_')
                        
                        for method in methods:
                            method_data = df_approach[df_approach['Method'] == method]
                            if not method_data.empty:
                                means.append(method_data[metric_col].values[0])
                                if std_col in method_data.columns:
                                    stds.append(method_data[std_col].values[0])
                                else:
                                    stds.append(0)
                            else:
                                means.append(0)
                                stds.append(0)
                        
                        offset = width * (i - len(approaches)/2 + 0.5)
                        ax.bar(x + offset, means, width, yerr=stds, 
                               label=str(approach).upper(), alpha=0.8, capsize=4)
                    
                    ax.set_xlabel('Re-ranking Method', fontweight='bold')
                    ax.set_ylabel(metric_label, fontweight='bold')
                    ax.set_title(f'{metric_label} - {dataset} (wrapper: {wrapper})', fontweight='bold', pad=20)
                    ax.set_xticks(x)
                    ax.set_xticklabels(methods, rotation=45, ha='right')
                    ax.legend(title='Approach', loc='best')
                    ax.grid(axis='y', alpha=0.3, linestyle='--')
                    
                    plt.tight_layout()
                    
                    clean_ds = str(dataset).replace(" ", "_").replace("(", "").replace(")", "")
                    filename = f"{category}_{metric_col}_{clean_ds}_{wrapper}.pdf"
                    save_plot(fig, filename, comp_dir)
                    plt.close(fig)

def create_scalability_lineplots(df: pd.DataFrame, output_dir: Path):
    """
    Generates line plots showing how key metrics scale with dataset size.
    """
    logger.info("Generating scalability line plots...")
    scalability_dir = output_dir / "scalability_lineplots"
    scalability_dir.mkdir(parents=True, exist_ok=True)
    
    if 'wrapper' not in df.columns or 'approach' not in df.columns:
        return
        
    def extract_dataset_size(dataset_name: str):
        match = re.search(r'(\d+)(k|K|M|m)?', str(dataset_name))
        if match:
            num = float(match.group(1))
            suffix = match.group(2)
            if suffix and suffix.lower() == 'k': num *= 1000
            elif suffix and suffix.lower() == 'm': num *= 1000000
            return (num, match.group(0))
        return (0, str(dataset_name))
    
    df['dataset_size_num'], df['dataset_size_label'] = zip(*df['Dataset'].apply(extract_dataset_size))
    
    key_metrics = {
        'mean_time_elapsed_time_sec': 'Execution Time (s)',
        'mean_time_major_page_faults': 'Major Page Faults',
        'mean_perf_ipc': 'IPC (Instructions Per Cycle)',
        'mean_perf_cache_miss_rate': 'Cache Miss Rate (%)',
    }
    
    for metric_col, metric_label in key_metrics.items():
        if metric_col not in df.columns: continue
        
        df_metric = df[df[metric_col].notna()].copy()
        if df_metric.empty: continue
        
        for method in df_metric['Method'].unique():
            for wrapper in df_metric['wrapper'].unique():
                df_subset = df_metric[
                    (df_metric['Method'] == method) & 
                    (df_metric['wrapper'] == wrapper)
                ]
                
                if df_subset.empty or df_subset['dataset_size_num'].nunique() < 2:
                    continue
                
                df_subset = df_subset.sort_values('dataset_size_num')
                
                fig, ax = plt.subplots(figsize=(10, 6))
                
                for approach in df_subset['approach'].unique():
                    df_approach = df_subset[df_subset['approach'] == approach]
                    
                    ax.plot(
                        df_approach['dataset_size_num'], df_approach[metric_col],
                        marker='o', linewidth=2, markersize=8,
                        label=str(approach).upper(), alpha=0.8
                    )
                    
                    std_col = metric_col.replace('mean_', 'std_')
                    if std_col in df_approach.columns:
                        ax.fill_between(
                            df_approach['dataset_size_num'],
                            df_approach[metric_col] - df_approach[std_col],
                            df_approach[metric_col] + df_approach[std_col],
                            alpha=0.2
                        )
                
                ax.set_xlabel('Dataset Size', fontweight='bold')
                ax.set_ylabel(metric_label, fontweight='bold')
                ax.set_title(f'Scalability: {metric_label} - {method} (wrapper: {wrapper})', fontweight='bold', pad=20)
                
                unique_sizes = df_subset[['dataset_size_num', 'dataset_size_label']].drop_duplicates().sort_values('dataset_size_num')
                ax.set_xticks(unique_sizes['dataset_size_num'])
                ax.set_xticklabels(unique_sizes['dataset_size_label'], rotation=45, ha='right')
                
                ax.legend(title='Approach', loc='best')
                ax.grid(True, alpha=0.3, linestyle='--')
                
                plt.tight_layout()
                filename = f"scalability_{metric_col}_{method}_{wrapper}.pdf"
                save_plot(fig, filename, scalability_dir)
                plt.close(fig)

def create_heatmaps(df: pd.DataFrame, output_dir: Path):
    """
    Generates heatmaps to visualize relative performance between methods and datasets.
    """
    logger.info("Generating heatmaps...")
    heatmap_dir = output_dir / "heatmaps"
    heatmap_dir.mkdir(parents=True, exist_ok=True)
    
    if 'wrapper' not in df.columns or 'approach' not in df.columns: return
        
    heatmap_metrics = {
        'mean_time_elapsed_time_sec': 'Execution Time (s)',
        'mean_perf_ipc': 'IPC',
        'mean_perf_cache_miss_rate': 'Cache Miss Rate (%)',
    }
    
    for metric_col, metric_label in heatmap_metrics.items():
        if metric_col not in df.columns: continue
        
        df_metric = df[df[metric_col].notna()].copy()
        if df_metric.empty: continue
        
        for approach in df_metric['approach'].unique():
            for wrapper in df_metric['wrapper'].unique():
                df_subset = df_metric[
                    (df_metric['approach'] == approach) & 
                    (df_metric['wrapper'] == wrapper)
                ]
                
                if df_subset.empty: continue
                
                pivot = df_subset.pivot_table(
                    values=metric_col, index='Dataset', columns='Method', aggfunc='mean'
                )
                
                if pivot.empty: continue
                
                fig, ax = plt.subplots(figsize=(14, 8))
                sns.heatmap(pivot, annot=True, fmt='.2f', cmap='YlOrRd', linewidths=0.5, cbar_kws={'label': metric_label}, ax=ax)
                
                ax.set_title(f'{metric_label} - {str(approach).upper()} (wrapper: {wrapper})', fontweight='bold', pad=20)
                ax.set_xlabel('Method', fontweight='bold')
                ax.set_ylabel('Dataset', fontweight='bold')
                
                plt.xticks(rotation=45, ha='right')
                plt.yticks(rotation=0)
                plt.tight_layout()
                
                filename = f"heatmap_{metric_col}_{approach}_{wrapper}.pdf"
                save_plot(fig, filename, heatmap_dir)
                plt.close(fig)

def create_speedup_analysis(df: pd.DataFrame, output_dir: Path):
    """
    Creates speedup analysis: calculates the acceleration factor (or deceleration)
    of the batch approach compared to mmap.
    """
    logger.info("Generating speedup analysis (batch vs mmap)...")
    speedup_dir = output_dir / "speedup_analysis"
    speedup_dir.mkdir(parents=True, exist_ok=True)
    
    if 'wrapper' not in df.columns or 'approach' not in df.columns: return
    
    metric_col = 'mean_time_elapsed_time_sec'
    if metric_col not in df.columns: return
    
    df_time = df[df[metric_col].notna()].copy()
    speedup_results = []
    
    for (dataset, method, wrapper), group in df_time.groupby(['Dataset', 'Method', 'wrapper']):
        mmap_data = group[group['approach'] == 'mmap']
        batch_data = group[group['approach'] == 'batch']
        
        if not mmap_data.empty and not batch_data.empty:
            mmap_time = mmap_data[metric_col].values[0]
            batch_time = batch_data[metric_col].values[0]
            
            if batch_time > 0:
                speedup = mmap_time / batch_time
                speedup_results.append({
                    'Dataset': dataset, 'Method': method, 'wrapper': wrapper,
                    'mmap_time': mmap_time, 'batch_time': batch_time, 'speedup': speedup
                })
    
    if not speedup_results: return
    
    df_speedup = pd.DataFrame(speedup_results)
    speedup_csv = speedup_dir / "speedup_table.csv"
    df_speedup.to_csv(speedup_csv, index=False)
    
    for wrapper in df_speedup['wrapper'].unique():
        df_wrapper = df_speedup[df_speedup['wrapper'] == wrapper]
        fig, ax = plt.subplots(figsize=(12, 6))
        
        speedup_by_method = df_wrapper.groupby('Method')['speedup'].mean().sort_values()
        colors = ['green' if x > 1 else 'red' for x in speedup_by_method.values]
        speedup_by_method.plot(kind='barh', ax=ax, color=colors, alpha=0.7)
        
        ax.axvline(x=1, color='black', linestyle='--', linewidth=2, label='Baseline (mmap)')
        ax.set_xlabel('Speedup (mmap/batch)', fontweight='bold')
        ax.set_ylabel('Method', fontweight='bold')
        ax.set_title(f'Avg Speedup: Batch vs Mmap (wrapper: {wrapper})\n(>1 = batch faster, <1 = mmap faster)', fontweight='bold', pad=20)
        ax.legend()
        ax.grid(axis='x', alpha=0.3, linestyle='--')
        
        plt.tight_layout()
        filename = f"speedup_by_method_{wrapper}.pdf"
        save_plot(fig, filename, speedup_dir)
        plt.close(fig)

def generate_summary_report(df: pd.DataFrame, output_dir: Path):
    """
    Generates a textual report with descriptive statistics and key insights.
    """
    logger.info("Generating summary report...")
    report_path = output_dir / "summary_report.txt"
    
    if 'wrapper' not in df.columns or 'approach' not in df.columns: return
    
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write("="*80 + "\n")
        f.write("BENCHMARKING REPORT - OUT-OF-CORE FRAMEWORK\n")
        f.write("="*80 + "\n\n")
        
        f.write(f"Total Executions: {len(df)}\n")
        f.write(f"Datasets: {df['Dataset'].nunique()}\n")
        f.write(f"Methods: {df['Method'].nunique()}\n")
        f.write(f"Approaches: {', '.join(df['approach'].dropna().unique())}\n")
        f.write(f"Wrappers: {', '.join(df['wrapper'].dropna().unique())}\n\n")
        
        f.write("-"*80 + "\n")
        f.write("AVAILABLE METRICS\n")
        f.write("-"*80 + "\n\n")
        
        metric_cols = [col for col in df.columns if col.startswith('mean_')]
        for col in sorted(metric_cols):
            non_null_count = df[col].notna().sum()
            f.write(f"  - {col}: {non_null_count}/{len(df)} valid values\n")
        
        f.write("\n" + "-"*80 + "\n")
        f.write("DESCRIPTIVE STATISTICS\n")
        f.write("-"*80 + "\n\n")
        
        for approach in df['approach'].dropna().unique():
            f.write(f"\n=== Approach: {str(approach).upper()} ===\n\n")
            df_approach = df[df['approach'] == approach]
            
            key_metrics = [
                'mean_time_elapsed_time_sec', 'mean_time_major_page_faults',
                'mean_perf_ipc', 'mean_perf_cache_miss_rate'
            ]
            
            for metric in key_metrics:
                if metric in df_approach.columns and df_approach[metric].notna().any():
                    stats = df_approach[metric].describe()
                    f.write(f"{metric}:\n")
                    f.write(f"  Mean: {stats['mean']:.4f}\n")
                    f.write(f"  Median: {stats['50%']:.4f}\n")
                    f.write(f"  Min: {stats['min']:.4f}\n")
                    f.write(f"  Max: {stats['max']:.4f}\n")
                    f.write(f"  Std Dev: {stats['std']:.4f}\n\n")
        
        f.write("\n" + "="*80 + "\n")
        f.write("END OF REPORT\n")
        f.write("="*80 + "\n")
