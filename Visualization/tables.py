import pandas as pd
from pathlib import Path
import logging

logger = logging.getLogger(__name__)

def format_latex_value(val):
    if pd.isna(val): return "---"
    if isinstance(val, float):
        if val > 1000: return f"{val:,.1f}".replace(",", "{,}")
        return f"{val:.1f}"
    return str(val)

def generate_performance_table(df, output_dir: Path):
    """
    Generates tab:full_memory_performance for LHRR on 192GB Server.
    """
    env_a = df[(df['Environment'] == 'Env A: 192GB Server') & 
               (df['Config_ID'].str.contains('L2000')) & 
               (df['Approach_Type'].isin(['CSR', 'Dense'])) &
               (df['Library_Strategy'].isin(['my_own_lib', 'dense_mmap']))]
    
    # Filter for LHRR specifically
    env_a = env_a[env_a['Config_ID'].str.contains('LHRR')]
    
    latex = "\\begin{table}[htbp]\n\\centering\n"
    latex += "\\caption{LHRR Performance on 192~GB Server ($L=2000$, 18 Threads)}\n"
    latex += "\\label{tab:full_memory_performance}\n"
    latex += "\\footnotesize\n"
    latex += "\\begin{tabular}{@{}llrrr@{}}\n\\toprule\n"
    latex += "\\textbf{Dataset} & \\textbf{Method} & \\textbf{Time (s)} & \\textbf{Page Faults (M)} & \\textbf{Instructions ($\\times 10^{12}$)} \\\\\\midrule\n"
    
    datasets = ['ALOI (72K)', 'SUN397 (108K)', 'VegFru (160K)']
    for ds in datasets:
        ds_df = env_a[env_a['Dataset'] == ds]
        # Sort so Dense comes first
        ds_df = ds_df.sort_values(by='Method', ascending=True)
        
        for idx, (i, row) in enumerate(ds_df.iterrows()):
            method = row['Method']
            time = format_latex_value(row['mean_time_elapsed_time_sec'])
            pf = format_latex_value(row['mean_perf_page_faults'] / 1e6)
            instr = format_latex_value(row['mean_perf_instructions'] / 1e12)
            
            is_last = (ds == datasets[-1]) and (idx == len(ds_df) - 1)
            line_end = " \\\\\n" if is_last else " \\\\\\midrule\n"

            if method == 'Dense':
                latex += f"\\multirow{{2}}{{*}}{{{ds}}} \n    & {method} & {time} & {pf} & {instr} \\\\\n"
            else:
                latex += f"    & {method} & \\textbf{{{time}}} & {pf} & \\textbf{{{instr}}}{line_end}"
    
    latex += "\\bottomrule\n\\end{tabular}\n\\end{table}"
    
    file_path = output_dir / 'table_performance.tex'
    with open(file_path, 'w') as f:
        f.write(latex)
    logger.info(f"Generated {file_path.name}")

def generate_reduced_memory_table(df, output_dir: Path):
    """
    Generates tab:reduced_memory for Env B.
    Creates two tables: CPRR and LHRR.
    """
    algos = ['CPRR', 'LHRR']
    
    for algo in algos:
        env_b = df[(df['Environment'] == 'Env B: 16GB Limited') & 
                   (df['Config_ID'].str.contains('L1000')) &
                   (df['Approach_Type'].isin(['CSR', 'Dense']))]
        
        env_b = env_b[env_b['Config_ID'].str.contains(algo)]
        
        if env_b.empty: continue
        
        latex = "\\begin{table}[htbp]\n\\centering\n"
        latex += f"\\caption{{Feasibility Comparison ({algo}): Dense Baseline vs. Sparse Reformulation (16~GB RAM, L=1000, K=20, 12 Threads)}}\n"
        latex += f"\\label{{tab:reduced_memory_{algo.lower()}}}\n"
        latex += "\\begin{tabular}{@{}llcc@{}}\n\\toprule\n"
        latex += "\\textbf{Dataset} & \\textbf{Method} & \\textbf{Status} & \\textbf{Time (s)} \\\\\n\\midrule\n"
        
        datasets = ['ALOI (72K)', 'SUN397 (108K)', 'VegFru (160K)']
        for ds_idx, ds in enumerate(datasets):
            ds_df = env_b[env_b['Dataset'] == ds]
            
            # Dense
            dense_row = ds_df[ds_df['Method'] == 'Dense']
            if dense_row.empty or dense_row.iloc[0]['n_success'] == 0:
                dense_status = "\\textbf{OOM}"
                dense_time = "---"
            else:
                dense_status = "Success"
                dense_time = format_latex_value(dense_row.iloc[0]['mean_time_elapsed_time_sec'])
                
            # Sparse
            sparse_row = ds_df[(ds_df['Method'] == 'Sparse') & (ds_df['Library_Strategy'] == 'my_own_lib')]
            if sparse_row.empty or sparse_row.iloc[0]['n_success'] == 0:
                sparse_status = "\\textbf{OOM}"
                sparse_time = "---"
            else:
                sparse_status = "Success"
                sparse_time = format_latex_value(sparse_row.iloc[0]['mean_time_elapsed_time_sec'])
                
            is_last = (ds_idx == len(datasets) - 1)
            line_end = " \\\\\n" if is_last else " \\\\\\midrule\n"

            latex += f"\\multirow{{2}}{{*}}{{{ds}}} \n    & Dense & {dense_status} & {dense_time} \\\\\n"
            latex += f"    & Sparse (Custom) & {sparse_status} & {sparse_time}{line_end}"
            
        latex += "\\bottomrule\n\\end{tabular}\n\\end{table}"
        
        filename = f'table_reduced_memory_{algo.lower()}.tex'
        file_path = output_dir / filename
        with open(file_path, 'w') as f:
            f.write(latex)
            logger.info(f"Generated {filename}")

def generate_accuracy_table(df_acc, output_dir: Path):
    """
    Generates Grid Search Accuracy tables (L vs K) for each dataset.
    Creates table_accuracy_aloi.tex, table_accuracy_sun397.tex, table_accuracy_vegfru.tex.
    """
    if df_acc is None: return
    
    datasets = [
        ('ALOI (72K)', 'aloi', 'ALOI (72K): MAP Accuracy Grid Search ($L$ vs $K$)'),
        ('SUN397 (108K)', 'sun397', 'SUN397 (108K): MAP Accuracy Grid Search ($L$ vs $K$)'),
        ('VegFru (160K)', 'vegfru', 'VegFru (160K): MAP Accuracy Grid Search ($L$ vs $K$)')
    ]
    
    k_vals = [20, 30, 50]
    l_vals = [100, 400, 1000, 2000]
    
    # helper to get best MAP
    def get_map(row):
        for col in ['MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
            if col in row and not pd.isna(row[col]):
                return row[col]
        return 0.0

    for ds_name, ds_filename, ds_caption in datasets:
        ds_df = df_acc[df_acc['Dataset'] == ds_name]
        if ds_df.empty: continue
        
        # Determine global max for highlighting
        sparse_df = ds_df[ds_df['Approach'] != 'Baseline']
        max_map = 0.0
        if not sparse_df.empty:
            max_map = sparse_df.apply(get_map, axis=1).max()

        # 1. Baseline
        baseline_rows = ds_df[ds_df['Approach'] == 'Baseline']
        b_map = 0.0
        if not baseline_rows.empty:
            b_map = get_map(baseline_rows.iloc[0])

        latex = "\\begin{table}[htbp]\n\\centering\n"
        latex += f"\\caption{{{ds_caption}}}\n"
        latex += f"\\label{{tab:acc_{ds_filename}}}\n"
        latex += "\\begin{tabular}{llccc}\n\\toprule\n"
        latex += "\\multirow{2}{*}{\\textbf{Method}} & \\multirow{2}{*}{\\textbf{L Parameter}} & \\multicolumn{3}{c}{\\textbf{K Parameter (Neighbors)}} \\\\ \\cmidrule(l){3-5} \n"
        latex += " & & \\textbf{K=20} & \\textbf{K=30} & \\textbf{K=50} \\\\ \\midrule\n"
        
        latex += f"Baseline & (No Re-ranking) & \\multicolumn{{3}}{{c}}{{{b_map:.1f}}} \\\\ \\midrule\n"
        
        for algo in ['CPRR', 'LHRR']:
            algo_df = ds_df[ds_df['Approach'] == algo]
            n_l = len(algo_df['L'].unique())
            
            for i, l_val in enumerate(l_vals):
                l_row = algo_df[algo_df['L'] == l_val]
                if l_row.empty: continue
                
                method_label = f"\\multirow{{{n_l}}}{{*}}{{{algo}}}" if i == 0 else ""
                row_str = f"{method_label} & L={int(l_val)} "
                
                for k_val in k_vals:
                    k_row = l_row[l_row['K'] == k_val]
                    if k_row.empty:
                        row_str += " & --- "
                    else:
                        val = get_map(k_row.iloc[0])
                        val_str = f"{val:.1f}"
                        if val == max_map and max_map > 0:
                            val_str = f"\\textbf{{{val_str}}}"
                        row_str += f" & {val_str} "
                
                row_str += "\\\\\n"
                latex += row_str
            
            if algo == 'CPRR':
                latex += "\\midrule\n"

        latex += "\\bottomrule\n\\end{tabular}\n\\end{table}\n"
        
        filename = f'table_accuracy_{ds_filename}.tex'
        file_path = output_dir / filename
        with open(file_path, 'w') as f:
            f.write(latex)
        logger.info(f"Generated {filename}")
    
    file_path = output_dir / 'table_accuracy.tex'
    with open(file_path, 'w') as f:
        f.write("% Deprecated: use per-dataset tables")

def generate_thesis_summary_table(df_acc, output_dir: Path):
    """
    Generates a high-level summary table for the thesis.
    Focuses on VegFru (Largest dataset) and ALOI (Comparison).
    """
    if df_acc is None: return
    
    # Dataset size mapping
    dataset_n = {'ALOI (72K)': 72000, 'SUN397 (108K)': 108000, 'VegFru (160K)': 160000}
    
    def estimate_memory(row):
        n = dataset_n.get(row['Dataset'], 0)
        if n == 0: return 0
        if row['Approach'] == 'Baseline': return (n * n * 4) / (1024**2)
        return (n * (row['L'] + row['K']) * 4) / (1024**2)

    # Coalesce MAP
    def get_map(row):
        for col in [f"MAP@{int(row['L'])}" if not pd.isna(row['L']) else "", 'MAP@2000', 'MAP@1000', 'MAP@400', 'MAP@100']:
            if col in row and not pd.isna(row[col]): return row[col]
        return 0.0

    df = df_acc.copy()
    df['Mem'] = df.apply(estimate_memory, axis=1)
    df['MAP'] = df.apply(get_map, axis=1)

    latex = "\\begin{table}[htbp]\n\\centering\n"
    latex += "\\caption{Comparative Summary: Dense Baseline vs. Sparse Reformulation}\n"
    latex += "\\label{tab:thesis_summary}\n"
    latex += "\\begin{tabular}{@{}lccc@{}}\n\\toprule\n"
    latex += "\\textbf{Metric} & \\textbf{Baseline (Dense)} & \\textbf{Sparse ($L=1000, K=30$)} & \\textbf{Benefit} \\\\\n\\midrule\n"
    
    # Row 1: Complexity
    latex += "Complexity & $O(N^2)$ & $O(N \\cdot (L+K))$ & Scalability \\\\\n"
    
    # Row 2: VegFru Memory
    v_base_mem = df[(df['Dataset'] == 'VegFru (160K)') & (df['Approach'] == 'Baseline')]['Mem'].max()
    v_sparse_mem = df[(df['Dataset'] == 'VegFru (160K)') & (df['Approach'] == 'LHRR') & (df['L'] == 1000) & (df['K'] == 30)]['Mem'].mean()
    if pd.isna(v_sparse_mem): # fallback if K=30 not for VegFru
        v_sparse_mem = df[(df['Dataset'] == 'VegFru (160K)') & (df['Approach'] == 'LHRR') & (df['L'] == 1000)]['Mem'].mean()
    
    latex += f"VegFru RAM & $\\approx$ {v_base_mem/1024:.1f} GB & $\\approx$ {v_sparse_mem/1024:.2f} GB & \\textbf{{{v_base_mem/v_sparse_mem:.1f}x Savings}} \\\\\n"
    
    # Row 3: ALOI MAP
    a_base_map = df[(df['Dataset'] == 'ALOI (72K)') & (df['Approach'] == 'Baseline')]['MAP'].max()
    a_sparse_map = df[(df['Dataset'] == 'ALOI (72K)') & (df['Approach'] == 'LHRR') & (df['L'] == 1000) & (df['K'] == 20)]['MAP'].mean()
    
    latex += f"ALOI MAP (LHRR) & {a_base_map:.1f}\\% & {a_sparse_map:.1f}\\% & \\textbf{{+{a_sparse_map - a_base_map:.1f}\\% Accuracy}} \\\\\n"
    
    # Row 4: Diminishing Returns
    latex += f"Diminishing Returns & N/A & High after $L=1000$ & Efficiency Optimized \\\\\n"

    latex += "\\bottomrule\n\\end{tabular}\n\\end{table}"
    
    file_path = output_dir / 'table_thesis_summary.tex'
    with open(file_path, 'w') as f:
        f.write(latex)
    logger.info("Generated table_thesis_summary.tex")

def generate_impl_battle_table(df, output_dir: Path):
    """
    Compares Custom CSR vs MKL vs GraphBLAS.
    """
    env_a = df[(df['Environment'] == 'Env A: 192GB Server') & 
               (df['Config_ID'].str.contains('L1000')) & 
               (df['Approach_Type'] == 'CSR')].copy()
    
    if env_a.empty: return

    latex = "\\begin{table}[htbp]\n\\centering\n"
    latex += "\\caption{Implementation Battle: Custom CSR vs. High-Performance Libraries}\n"
    latex += "\\label{tab:impl_battle}\n"
    latex += "\\begin{tabular}{@{}llrrr@{}}\n\\toprule\n"
    latex += "\\textbf{Dataset} & \\textbf{Backend} & \\textbf{Time (s)} & \\textbf{Instructions (T)} & \\textbf{IPC} \\\\\\midrule\n"
    
    datasets = ['ALOI (72K)', 'SUN397 (108K)', 'VegFru (160K)']
    for ds_idx, ds in enumerate(datasets):
        ds_df = env_a[env_a['Dataset'] == ds]
        if ds_df.empty: continue
        
        # Strategies to compare
        strategies = [('my_own_lib', 'Custom FMP'), ('IntelMKL', 'Intel MKL'), ('GraphBLAS', 'SuiteSparse')]
        
        for strat_idx, (strat, label) in enumerate(strategies):
            row = ds_df[ds_df['Library_Strategy'] == strat]
            if row.empty: continue
            r = row.iloc[0]
            
            time = format_latex_value(r['mean_time_elapsed_time_sec'])
            instr = format_latex_value(r['mean_perf_instructions'] / 1e12)
            ipc = format_latex_value(r['mean_perf_ipc'])
            
            ds_label = ds if strat_idx == 0 else ""
            
            is_last = (ds_idx == len(datasets) - 1) and (strat_idx == len(strategies) - 1)
            line_end = " \\\\\n" if is_last else " \\\\\n" # Implementation battle uses manual midrules
            
            latex += f"{ds_label} & {label} & {time} & {instr} & {ipc}{line_end}"
        
        if ds_idx < len(datasets) - 1:
            latex += "\\midrule\n"
    
    latex += "\\bottomrule\n\\end{tabular}\n\\end{table}"
    
    file_path = output_dir / 'table_impl_battle.tex'
    with open(file_path, 'w') as f:
        f.write(latex)
    logger.info("Generated table_impl_battle.tex")

def generate_comprehensive_lib_tables(df, output_dir: Path):
    """
    Generates 6 tables: 3 Environments x 2 Algorithms.
    Rows: Libraries, Columns: Datasets + Speedup.
    """
    import numpy as np
    
    # Environment Definitions & Parameters
    envs = [
        ('Env A: 192GB Server', 'Environment A (High-Performance Server)', 'L=2000, K=50', 'env_a'),
        ('Env B: 16GB Limited', 'Environment B (Constrained Hardware)', 'L=1000, K=20', 'env_b'),
        ('Env C: 8GB Consumer', 'Environment C (Commodity Desktop)', 'L=1000, K=20', 'env_c')
    ]
    
    # Algorithms
    algos = ['CPRR', 'LHRR']
    
    # Library Mapping
    lib_map = {
        'my_own_lib': 'Custom Engine',
        'IntelMKL': 'Intel MKL',
        'GraphBLAS': 'GraphBLAS',
        'eigen': 'Eigen'
    }
    
    datasets = ['ALOI (72K)', 'SUN397 (108K)', 'VegFru (160K)']
    
    for env_name, env_title, params, env_tag in envs:
        # Filter Environment
        env_df = df[df['Environment'].str.contains(env_name.split(':')[0], regex=False) & 
                   (df['Approach_Type'] == 'CSR')]
        
        if env_df.empty: 
            logger.warning(f"No data found for {env_name}")
            continue

        for algo in algos:
            # Filter Algorithm
            algo_df = env_df[env_df['Config_ID'].str.contains(algo)].copy()
            
            if algo_df.empty: continue
            
            libraries = algo_df['Library_Strategy'].unique()
            ordered_libs = ['my_own_lib'] + [l for l in libraries if l != 'my_own_lib']
            
            custom_params = params
            if env_tag == 'env_c' and algo == 'LHRR':
                custom_params = "L=1000 (ALOI), L=400 (SUN/VegFru), K=20"
                
            latex = "\\begin{table}[htbp]\n\\centering\n"
            latex += f"\\caption{{{algo} Performance on {env_title} ({custom_params})}}\n"
            latex += f"\\label{{tab:lib_comp_{env_tag}_{algo.lower()}}}\n"
            latex += "\\small\n"
            latex += "\\begin{tabular}{@{}l|ccc|c@{}}\n\\toprule\n"
            latex += "\\textbf{Library} & \\textbf{ALOI (s)} & \\textbf{SUN (s)} & \\textbf{VegFru (s)} & \\textbf{Avg. Speedup} \\\\\n\\midrule\n"
            
            baseline_times = {}
            for ds in datasets:
                row = algo_df[(algo_df['Library_Strategy'] == 'my_own_lib') & (algo_df['Dataset'] == ds)]
                if not row.empty:
                    baseline_times[ds] = row.iloc[0]['mean_time_elapsed_time_sec']
                else:
                    baseline_times[ds] = None

            for lib in ordered_libs:
                lib_name = lib_map.get(lib, lib)
                row_str = f"{lib_name}"
                
                speedups = []
                for ds in datasets:
                    row = algo_df[(algo_df['Library_Strategy'] == lib) & (algo_df['Dataset'] == ds)]
                    
                    if row.empty or row.iloc[0].get('n_success', 0) == 0:
                        time_str = "\\textbf{OOM}"
                        if baseline_times.get(ds):
                            speedups.append(np.inf)
                    else:
                        time = row.iloc[0]['mean_time_elapsed_time_sec']
                        time_str = format_latex_value(time)
                        if baseline_times.get(ds):
                            base_t = baseline_times[ds]
                            sp = base_t / time
                            speedups.append(sp)
                    
                    row_str += f" & {time_str}"
                
                if not speedups:
                    avg_speedup = "---"
                elif any(s == np.inf for s in speedups):
                    avg_speedup = "\\textbf{$\\infty$}"
                else:
                    gmean = np.exp(np.mean(np.log(speedups)))
                    avg_speedup = f"{gmean:.1f}$\\times$"
                    if lib == 'my_own_lib':
                        avg_speedup = "\\textbf{1.0$\\times$}"
                
                row_str += f" & {avg_speedup} \\\\\n"
                latex += row_str
            
            latex += "\\bottomrule\n\\end{tabular}\n\\end{table}"
            
            filename = f"table_lib_{env_tag}_{algo.lower()}.tex"
            file_path = output_dir / filename
            with open(file_path, 'w') as f:
                f.write(latex)
            logger.info(f"Generated {filename}")

def generate_unified_feasibility_table(df, output_dir: Path):
    """
    Generates two unified tables (CPRR and LHRR) comparing performance and feasibility
    across all three environments (A, B, C) and all datasets.
    """
    import numpy as np
    
    algos = ['CPRR', 'LHRR']
    envs = [
        ('Env A: 192GB Server', 'A (192GB)'),
        ('Env B: 16GB Limited', 'B (16GB)'),
        ('Env C: 8GB Consumer', 'C (8GB)')
    ]
    datasets = ['ALOI (72K)', 'SUN397 (108K)', 'VegFru (160K)']
    
    backends = [
        ('my_own_lib', 'Custom engine'),
        ('IntelMKL', 'Intel MKL'),
        ('GraphBLAS', 'GraphBLAS'),
        ('eigen', 'Eigen'),
        ('dense_mmap', 'Dense (MMAP)'),
        ('dense_swap', 'Dense (Swap)')
    ]

    for algo in algos:
        latex = "\\begin{table}[htbp]\n\\centering\n"
        latex += f"\\caption{{Unified Performance and Feasibility Comparison: {algo} across Hardware Environments. Status: Success (Time) or OOM. Parameters adjusted per environment to reflect hardware constraints.}}\n"
        latex += f"\\label{{tab:unified_feasibility_{algo.lower()}}}\n"
        latex += "\\footnotesize\n"
        latex += "\\begin{tabular}{@{}llccc@{}}\n\\toprule\n"
        latex += "\\textbf{Dataset} & \\textbf{Backend} & \\textbf{Env A (192GB)} & \\textbf{Env B (16GB)} & \\textbf{Env C (8GB)} \\\\\n"
        latex += "& & \\scriptsize $L=2000, K=50$ & \\scriptsize $L=1000, K=20$ & \\scriptsize $L=1000^{*}$ \\\\\\midrule\n"
        
        for ds_idx, ds in enumerate(datasets):
            ds_df = df[df['Dataset'] == ds]
            
            for b_idx, (b_strat, b_label) in enumerate(backends):
                row_str = f"{ds if b_idx == 0 else ''} & {b_label}"
                
                env_results = []
                for env_idx, (env_name, env_label) in enumerate(envs):
                    mask = (ds_df['Environment'].str.contains(env_name.split(':')[0])) & \
                           (ds_df['Config_ID'].str.contains(algo)) & \
                           (ds_df['Library_Strategy'] == b_strat)
                    
                    match = ds_df[mask]
                    if match.empty or match.iloc[0]['n_success'] == 0:
                        status = "\\textbf{OOM}"
                    else:
                        time = match.iloc[0]['mean_time_elapsed_time_sec']
                        status = f"{time:.1f}s"
                        if env_idx == 2 and algo == 'LHRR' and ds in ['SUN397 (108K)', 'VegFru (160K)']:
                            status += "\\textsuperscript{$\\dagger$}"
                    
                    env_results.append((env_name, status))

                is_custom = (b_strat == 'my_own_lib')
                env_c_res = env_results[2][1]
                
                if is_custom and "\\textbf{OOM}" not in env_c_res:
                    others_failed = True
                    for other_strat, _ in backends:
                        if other_strat == 'my_own_lib': continue
                        mask_other = (ds_df['Environment'].str.contains('Env C')) & \
                                     (ds_df['Config_ID'].str.contains(algo)) & \
                                     (ds_df['Library_Strategy'] == other_strat)
                        match_other = ds_df[mask_other]
                        if not match_other.empty and match_other.iloc[0]['n_success'] > 0:
                            others_failed = False
                            break
                    
                    if others_failed:
                        if "\\textsuperscript{$\\dagger$}" in env_c_res:
                            clean_time = env_c_res.replace("\\textsuperscript{$\\dagger$}", "")
                            env_results[2] = (env_results[2][0], f"\\textbf{{{clean_time}}}\\textsuperscript{{$\\dagger$}}")
                        else:
                            env_results[2] = (env_results[2][0], f"\\textbf{{{env_c_res}}}")

                for _, res in env_results:
                    row_str += f" & {res}"
                
                is_last_in_ds = (b_idx == len(backends) - 1)
                is_last_ds = (ds_idx == len(datasets) - 1)
                
                if is_last_in_ds and not is_last_ds:
                    row_str += " \\\\\\midrule\n"
                else:
                    row_str += " \\\\\n"
                
                latex += row_str

        latex += "\\bottomrule\n\\end{tabular}\n"
        latex += "\\begin{flushleft}\\scriptsize\n"
        latex += "* For Env C (8GB), $L=1000$ was used for ALOI/CPRR.\\\\\n"
        if algo == 'LHRR':
            latex += "\\textsuperscript{$\\dagger$} Parameters reduced to $L=400$ to fit 8GB RAM using \\texttt{multiply\\_and\\_prune}.\n"
        latex += "\\end{flushleft}\n"
        latex += "\\end{table}"
        
        filename = f'table_unified_feasibility_{algo.lower()}.tex'
        file_path = output_dir / filename
        with open(file_path, 'w') as f:
            f.write(latex)
        logger.info(f"Generated {filename}")
