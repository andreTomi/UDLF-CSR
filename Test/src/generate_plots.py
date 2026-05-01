"""
Script de Análise e Visualização para Benchmarking de Out-of-Core Framework

Este script consome o CSV agregado gerado por run_experiments_updated.py e produz
visualizações para a dissertação de mestrado, focando na comparação entre as
abordagens mmap e batch (BatchExecutor).

Métricas-chave analisadas:
- Performance de I/O: elapsed_time, major_page_faults, perf_page_faults
- Uso de Memória: max_resident_set_kb
- Eficiência de CPU/Cache: ipc, cache_miss_rate, l1_dcache_load_misses, branch_miss_rate
"""

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
import logging
from pathlib import Path
from typing import List, Tuple, Dict
import re

# Configuração de logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Configurações de estilo para publicação
plt.style.use('seaborn-v0_8-paper')
sns.set_palette("husl")
plt.rcParams['figure.dpi'] = 300
plt.rcParams['savefig.dpi'] = 300
plt.rcParams['font.size'] = 10
plt.rcParams['axes.labelsize'] = 11
plt.rcParams['axes.titlesize'] = 12
plt.rcParams['legend.fontsize'] = 9
plt.rcParams['xtick.labelsize'] = 9
plt.rcParams['ytick.labelsize'] = 9

# Métricas-chave para análise (nomes exatos das colunas no CSV)
METRICS = {
    'io_performance': {
        'mean_time_elapsed_time_sec': 'Tempo de Execução (s)',
        'mean_time_major_page_faults': 'Major Page Faults',
        'mean_perf_page_faults': 'Page Faults (perf)',
    },
    'memory_usage': {
        'mean_time_max_resident_set_kb': 'Memória Máxima (MB)',
    },
    'cpu_cache_efficiency': {
        'mean_perf_ipc': 'IPC (Instructions Per Cycle)',
        'mean_perf_cache_miss_rate': 'Cache Miss Rate (%)',
        'mean_perf_l1_dcache_load_misses': 'L1 D-Cache Load Misses',
        'mean_perf_branch_miss_rate': 'Branch Miss Rate (%)',
    }
}


def load_and_process_data(csv_path: Path) -> pd.DataFrame:
    """
    Carrega o CSV agregado e processa a coluna 'config' para extrair dataset e método.
    
    Formato esperado da coluna config: 'config_dataset_name__METHOD_NAME'
    Exemplo: 'config_cifar_10k__RKGRAPH'
    """
    logger.info(f"📂 Carregando dados de: {csv_path}")
    
    if not csv_path.exists():
        raise FileNotFoundError(f"Arquivo não encontrado: {csv_path}")
    
    df = pd.read_csv(csv_path)
    logger.info(f"✅ Carregados {len(df)} registros")
    
    # Extrai dataset e método do nome do config
    # Padrão: config_<dataset>__<METHOD>
    def parse_config(config_str):
        match = re.match(r'(.+?)__(.+)$', config_str)
        if match:
            dataset = match.group(1)
            method = match.group(2)
            return dataset, method
        else:
            # Fallback se o padrão não for encontrado
            return config_str, 'UNKNOWN'
    
    df[['dataset', 'method']] = df['config'].apply(
        lambda x: pd.Series(parse_config(x))
    )
    
    # Converte memória de KB para MB se necessário
    if 'mean_time_max_resident_set_kb' in df.columns:
        df['mean_time_max_resident_set_kb_mb'] = df['mean_time_max_resident_set_kb'] / 1024
    
    logger.info(f"📊 Datasets encontrados: {df['dataset'].nunique()}")
    logger.info(f"🧮 Métodos encontrados: {df['method'].nunique()}")
    logger.info(f"⚙️  Abordagens encontradas: {df['approach'].unique().tolist()}")
    logger.info(f"🔧 Wrappers encontrados: {df['wrapper'].unique().tolist()}")
    
    return df


def create_comparison_barplots(df: pd.DataFrame, output_dir: Path):
    """
    Gera gráficos de barras comparando mmap vs. batch para as métricas-chave.
    
    Cria gráficos facetados (separados por dataset e método) com barras de erro
    mostrando o desvio padrão das N execuções.
    """
    logger.info("📊 Gerando gráficos de comparação (barras)...")
    
    comparison_dir = output_dir / "comparison_barplots"
    comparison_dir.mkdir(parents=True, exist_ok=True)
    
    # Para cada categoria de métricas
    for category, metrics in METRICS.items():
        logger.info(f"  - Categoria: {category}")
        
        for metric_col, metric_label in metrics.items():
            std_col = metric_col.replace('mean_', 'std_')
            
            # Verifica se a métrica existe no dataframe
            if metric_col not in df.columns:
                logger.warning(f"⚠️  Métrica {metric_col} não encontrada no dataset. Pulando.")
                continue
            
            # Filtra dados válidos (não-nulos)
            df_metric = df[df[metric_col].notna()].copy()
            
            if df_metric.empty:
                logger.warning(f"⚠️  Nenhum dado válido para {metric_col}. Pulando.")
                continue
            
            # Cria gráfico para cada combinação de dataset e wrapper
            for dataset in df_metric['dataset'].unique():
                for wrapper in df_metric['wrapper'].unique():
                    df_subset = df_metric[
                        (df_metric['dataset'] == dataset) & 
                        (df_metric['wrapper'] == wrapper)
                    ]
                    
                    if df_subset.empty:
                        continue
                    
                    # Agrupa por método e abordagem
                    methods = sorted(df_subset['method'].unique())
                    approaches = sorted(df_subset['approach'].unique())
                    
                    # Configura o gráfico
                    fig, ax = plt.subplots(figsize=(14, 6))
                    
                    x = np.arange(len(methods))
                    width = 0.35
                    
                    # Plota barras para cada abordagem
                    for i, approach in enumerate(approaches):
                        df_approach = df_subset[df_subset['approach'] == approach]
                        
                        means = []
                        stds = []
                        for method in methods:
                            method_data = df_approach[df_approach['method'] == method]
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
                               label=approach.upper(), alpha=0.8, capsize=4)
                    
                    # Configurações do gráfico
                    ax.set_xlabel('Método de Re-ranqueamento', fontweight='bold')
                    ax.set_ylabel(metric_label, fontweight='bold')
                    ax.set_title(
                        f'{metric_label} - {dataset} (wrapper: {wrapper})',
                        fontweight='bold', pad=20
                    )
                    ax.set_xticks(x)
                    ax.set_xticklabels(methods, rotation=45, ha='right')
                    ax.legend(title='Abordagem', loc='best')
                    ax.grid(axis='y', alpha=0.3, linestyle='--')
                    
                    plt.tight_layout()
                    
                    # Salva o gráfico
                    filename = f"{category}_{metric_col}_{dataset}_{wrapper}.png"
                    filepath = comparison_dir / filename
                    plt.savefig(filepath, bbox_inches='tight')
                    plt.close()
                    
                    logger.debug(f"    ✓ Salvo: {filename}")
    
    logger.info(f"✅ Gráficos de comparação salvos em: {comparison_dir}")


def create_scalability_lineplots(df: pd.DataFrame, output_dir: Path):
    """
    Gera gráficos de linha mostrando como as métricas-chave escalam com o tamanho do dataset.
    
    Extrai o tamanho do dataset do nome (ex: 'config_10k', 'config_1M') e plota
    curvas de escalabilidade separadas para mmap e batch.
    """
    logger.info("📈 Gerando gráficos de escalabilidade (linhas)...")
    
    scalability_dir = output_dir / "scalability_lineplots"
    scalability_dir.mkdir(parents=True, exist_ok=True)
    
    # Extrai tamanho do dataset do nome
    def extract_dataset_size(dataset_name: str) -> Tuple[float, str]:
        """
        Extrai o tamanho numérico do dataset para ordenação.
        Ex: 'config_cifar_10k' -> (10000, '10k')
            'config_cifar_1M' -> (1000000, '1M')
        """
        # Procura por padrões como 10k, 1M, 100k, etc.
        match = re.search(r'(\d+)(k|K|M|m)?', dataset_name)
        if match:
            num = float(match.group(1))
            suffix = match.group(2)
            if suffix and suffix.lower() == 'k':
                num *= 1000
            elif suffix and suffix.lower() == 'm':
                num *= 1000000
            return (num, match.group(0))
        return (0, dataset_name)
    
    df['dataset_size_num'], df['dataset_size_label'] = zip(
        *df['dataset'].apply(extract_dataset_size)
    )
    
    # Seleciona as métricas mais importantes para escalabilidade
    key_scalability_metrics = {
        'mean_time_elapsed_time_sec': 'Tempo de Execução (s)',
        'mean_time_major_page_faults': 'Major Page Faults',
        'mean_time_max_resident_set_kb_mb': 'Memória Máxima (MB)',
        'mean_perf_ipc': 'IPC (Instructions Per Cycle)',
        'mean_perf_cache_miss_rate': 'Cache Miss Rate (%)',
    }
    
    for metric_col, metric_label in key_scalability_metrics.items():
        if metric_col not in df.columns:
            logger.warning(f"⚠️  Métrica {metric_col} não encontrada. Pulando.")
            continue
        
        # Filtra dados válidos
        df_metric = df[df[metric_col].notna()].copy()
        
        if df_metric.empty:
            logger.warning(f"⚠️  Nenhum dado válido para {metric_col}. Pulando.")
            continue
        
        # Cria gráfico para cada combinação de método e wrapper
        for method in df_metric['method'].unique():
            for wrapper in df_metric['wrapper'].unique():
                df_subset = df_metric[
                    (df_metric['method'] == method) & 
                    (df_metric['wrapper'] == wrapper)
                ]
                
                if df_subset.empty or df_subset['dataset_size_num'].nunique() < 2:
                    continue
                
                # Ordena por tamanho do dataset
                df_subset = df_subset.sort_values('dataset_size_num')
                
                # Configura o gráfico
                fig, ax = plt.subplots(figsize=(10, 6))
                
                # Plota uma linha para cada abordagem
                for approach in df_subset['approach'].unique():
                    df_approach = df_subset[df_subset['approach'] == approach]
                    
                    ax.plot(
                        df_approach['dataset_size_num'], 
                        df_approach[metric_col],
                        marker='o', linewidth=2, markersize=8,
                        label=approach.upper(), alpha=0.8
                    )
                    
                    # Adiciona área de erro se disponível
                    std_col = metric_col.replace('mean_', 'std_')
                    if std_col in df_approach.columns:
                        ax.fill_between(
                            df_approach['dataset_size_num'],
                            df_approach[metric_col] - df_approach[std_col],
                            df_approach[metric_col] + df_approach[std_col],
                            alpha=0.2
                        )
                
                # Configurações do gráfico
                ax.set_xlabel('Tamanho do Dataset', fontweight='bold')
                ax.set_ylabel(metric_label, fontweight='bold')
                ax.set_title(
                    f'Escalabilidade: {metric_label} - {method} (wrapper: {wrapper})',
                    fontweight='bold', pad=20
                )
                
                # Formata o eixo X com os labels originais
                unique_sizes = df_subset[['dataset_size_num', 'dataset_size_label']].drop_duplicates()
                unique_sizes = unique_sizes.sort_values('dataset_size_num')
                ax.set_xticks(unique_sizes['dataset_size_num'])
                ax.set_xticklabels(unique_sizes['dataset_size_label'], rotation=45, ha='right')
                
                ax.legend(title='Abordagem', loc='best')
                ax.grid(True, alpha=0.3, linestyle='--')
                
                plt.tight_layout()
                
                # Salva o gráfico
                filename = f"scalability_{metric_col}_{method}_{wrapper}.png"
                filepath = scalability_dir / filename
                plt.savefig(filepath, bbox_inches='tight')
                plt.close()
                
                logger.debug(f"    ✓ Salvo: {filename}")
    
    logger.info(f"✅ Gráficos de escalabilidade salvos em: {scalability_dir}")


def create_heatmaps(df: pd.DataFrame, output_dir: Path):
    """
    Gera heatmaps para visualizar rapidamente a performance relativa entre
    diferentes métodos e datasets.
    """
    logger.info("🔥 Gerando heatmaps...")
    
    heatmap_dir = output_dir / "heatmaps"
    heatmap_dir.mkdir(parents=True, exist_ok=True)
    
    # Métricas para heatmap
    heatmap_metrics = {
        'mean_time_elapsed_time_sec': 'Tempo de Execução (s)',
        'mean_perf_ipc': 'IPC',
        'mean_perf_cache_miss_rate': 'Cache Miss Rate (%)',
    }
    
    for metric_col, metric_label in heatmap_metrics.items():
        if metric_col not in df.columns:
            continue
        
        df_metric = df[df[metric_col].notna()].copy()
        
        if df_metric.empty:
            continue
        
        # Cria heatmap para cada abordagem e wrapper
        for approach in df_metric['approach'].unique():
            for wrapper in df_metric['wrapper'].unique():
                df_subset = df_metric[
                    (df_metric['approach'] == approach) & 
                    (df_metric['wrapper'] == wrapper)
                ]
                
                if df_subset.empty:
                    continue
                
                # Cria pivot table (dataset x método)
                pivot = df_subset.pivot_table(
                    values=metric_col,
                    index='dataset',
                    columns='method',
                    aggfunc='mean'
                )
                
                if pivot.empty:
                    continue
                
                # Configura o heatmap
                fig, ax = plt.subplots(figsize=(14, 8))
                
                sns.heatmap(
                    pivot, 
                    annot=True, 
                    fmt='.2f', 
                    cmap='YlOrRd', 
                    linewidths=0.5,
                    cbar_kws={'label': metric_label},
                    ax=ax
                )
                
                ax.set_title(
                    f'{metric_label} - {approach.upper()} (wrapper: {wrapper})',
                    fontweight='bold', pad=20
                )
                ax.set_xlabel('Método', fontweight='bold')
                ax.set_ylabel('Dataset', fontweight='bold')
                
                plt.xticks(rotation=45, ha='right')
                plt.yticks(rotation=0)
                plt.tight_layout()
                
                # Salva o heatmap
                filename = f"heatmap_{metric_col}_{approach}_{wrapper}.png"
                filepath = heatmap_dir / filename
                plt.savefig(filepath, bbox_inches='tight')
                plt.close()
                
                logger.debug(f"    ✓ Salvo: {filename}")
    
    logger.info(f"✅ Heatmaps salvos em: {heatmap_dir}")


def create_speedup_analysis(df: pd.DataFrame, output_dir: Path):
    """
    Cria análise de speedup: calcula o fator de aceleração (ou desaceleração)
    da abordagem batch em relação ao mmap.
    """
    logger.info("⚡ Gerando análise de speedup (batch vs mmap)...")
    
    speedup_dir = output_dir / "speedup_analysis"
    speedup_dir.mkdir(parents=True, exist_ok=True)
    
    metric_col = 'mean_time_elapsed_time_sec'
    
    if metric_col not in df.columns:
        logger.warning(f"⚠️  Métrica {metric_col} não encontrada. Pulando speedup.")
        return
    
    # Filtra dados válidos
    df_time = df[df[metric_col].notna()].copy()
    
    # Para cada combinação de dataset, método e wrapper, calcula speedup
    speedup_results = []
    
    for (dataset, method, wrapper), group in df_time.groupby(['dataset', 'method', 'wrapper']):
        mmap_data = group[group['approach'] == 'mmap']
        batch_data = group[group['approach'] == 'batch']
        
        if not mmap_data.empty and not batch_data.empty:
            mmap_time = mmap_data[metric_col].values[0]
            batch_time = batch_data[metric_col].values[0]
            
            if batch_time > 0:
                speedup = mmap_time / batch_time
                speedup_results.append({
                    'dataset': dataset,
                    'method': method,
                    'wrapper': wrapper,
                    'mmap_time': mmap_time,
                    'batch_time': batch_time,
                    'speedup': speedup
                })
    
    if not speedup_results:
        logger.warning("⚠️  Nenhum resultado de speedup calculado.")
        return
    
    df_speedup = pd.DataFrame(speedup_results)
    
    # Salva tabela de speedup
    speedup_csv = speedup_dir / "speedup_table.csv"
    df_speedup.to_csv(speedup_csv, index=False)
    logger.info(f"💾 Tabela de speedup salva: {speedup_csv}")
    
    # Cria gráfico de barras com speedup por método
    for wrapper in df_speedup['wrapper'].unique():
        df_wrapper = df_speedup[df_speedup['wrapper'] == wrapper]
        
        fig, ax = plt.subplots(figsize=(12, 6))
        
        # Agrupa por método e calcula speedup médio
        speedup_by_method = df_wrapper.groupby('method')['speedup'].mean().sort_values()
        
        colors = ['green' if x > 1 else 'red' for x in speedup_by_method.values]
        
        speedup_by_method.plot(kind='barh', ax=ax, color=colors, alpha=0.7)
        
        ax.axvline(x=1, color='black', linestyle='--', linewidth=2, label='Baseline (mmap)')
        ax.set_xlabel('Speedup (mmap/batch)', fontweight='bold')
        ax.set_ylabel('Método', fontweight='bold')
        ax.set_title(
            f'Speedup Médio: Batch vs Mmap (wrapper: {wrapper})\n'
            f'(>1 = batch mais rápido, <1 = mmap mais rápido)',
            fontweight='bold', pad=20
        )
        ax.legend()
        ax.grid(axis='x', alpha=0.3, linestyle='--')
        
        plt.tight_layout()
        
        filename = f"speedup_by_method_{wrapper}.png"
        filepath = speedup_dir / filename
        plt.savefig(filepath, bbox_inches='tight')
        plt.close()
        
        logger.debug(f"    ✓ Salvo: {filename}")
    
    logger.info(f"✅ Análise de speedup salva em: {speedup_dir}")


def generate_summary_report(df: pd.DataFrame, output_dir: Path):
    """
    Gera um relatório textual com estatísticas descritivas e insights principais.
    """
    logger.info("📝 Gerando relatório de resumo...")
    
    report_path = output_dir / "summary_report.txt"
    
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write("="*80 + "\n")
        f.write("RELATÓRIO DE BENCHMARKING - OUT-OF-CORE FRAMEWORK\n")
        f.write("="*80 + "\n\n")
        
        f.write(f"Total de Execuções: {len(df)}\n")
        f.write(f"Datasets: {df['dataset'].nunique()}\n")
        f.write(f"Métodos: {df['method'].nunique()}\n")
        f.write(f"Abordagens: {', '.join(df['approach'].unique())}\n")
        f.write(f"Wrappers: {', '.join(df['wrapper'].unique())}\n\n")
        
        f.write("-"*80 + "\n")
        f.write("MÉTRICAS DISPONÍVEIS\n")
        f.write("-"*80 + "\n\n")
        
        # Lista todas as métricas disponíveis
        metric_cols = [col for col in df.columns if col.startswith('mean_')]
        for col in sorted(metric_cols):
            non_null_count = df[col].notna().sum()
            f.write(f"  - {col}: {non_null_count}/{len(df)} valores válidos\n")
        
        f.write("\n" + "-"*80 + "\n")
        f.write("ESTATÍSTICAS DESCRITIVAS\n")
        f.write("-"*80 + "\n\n")
        
        # Estatísticas por abordagem
        for approach in df['approach'].unique():
            f.write(f"\n=== Abordagem: {approach.upper()} ===\n\n")
            df_approach = df[df['approach'] == approach]
            
            key_metrics = [
                'mean_time_elapsed_time_sec',
                'mean_time_major_page_faults',
                'mean_time_max_resident_set_kb',
                'mean_perf_ipc',
                'mean_perf_cache_miss_rate'
            ]
            
            for metric in key_metrics:
                if metric in df_approach.columns and df_approach[metric].notna().any():
                    stats = df_approach[metric].describe()
                    f.write(f"{metric}:\n")
                    f.write(f"  Média: {stats['mean']:.4f}\n")
                    f.write(f"  Mediana: {stats['50%']:.4f}\n")
                    f.write(f"  Min: {stats['min']:.4f}\n")
                    f.write(f"  Max: {stats['max']:.4f}\n")
                    f.write(f"  Desvio Padrão: {stats['std']:.4f}\n\n")
        
        f.write("\n" + "="*80 + "\n")
        f.write("FIM DO RELATÓRIO\n")
        f.write("="*80 + "\n")
    
    logger.info(f"✅ Relatório salvo em: {report_path}")


def main():
    """Função principal que orquestra toda a análise e geração de plots."""
    logger.info("="*80)
    logger.info("🚀 INICIANDO ANÁLISE E GERAÇÃO DE VISUALIZAÇÕES")
    logger.info("="*80)
    
    # Caminho para o CSV agregado (ajuste conforme necessário)
    csv_path = Path("benchmark_outputs/config_output/all_summary_table_with_profilers.csv")
    output_dir = Path("analysis_outputs")
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Carrega e processa os dados
    try:
        df = load_and_process_data(csv_path)
    except FileNotFoundError as e:
        logger.error(f"❌ {e}")
        logger.error("💡 Certifique-se de executar run_experiments_updated.py primeiro!")
        return
    
    # Gera todas as visualizações
    try:
        create_comparison_barplots(df, output_dir)
        create_scalability_lineplots(df, output_dir)
        create_heatmaps(df, output_dir)
        create_speedup_analysis(df, output_dir)
        generate_summary_report(df, output_dir)
        
    except Exception as e:
        logger.error(f"❌ Erro durante a geração de visualizações: {e}", exc_info=True)
        return
    
    logger.info("\n" + "="*80)
    logger.info("✅ ANÁLISE COMPLETA!")
    logger.info("="*80)
    logger.info(f"📁 Visualizações salvas em: {output_dir}")
    logger.info(f"📊 Gráficos organizados por categoria:")
    logger.info(f"   - comparison_barplots/: Comparações mmap vs batch")
    logger.info(f"   - scalability_lineplots/: Curvas de escalabilidade")
    logger.info(f"   - heatmaps/: Mapas de calor")
    logger.info(f"   - speedup_analysis/: Análise de speedup")
    logger.info(f"📝 Relatório textual: summary_report.txt")


if __name__ == "__main__":
    main()
