# exp_runner/result_exporter.py
import csv
import json
from pathlib import Path
from typing import List
from dataclasses import asdict, fields
from collections import defaultdict
import logging


from .core import RunResult

logger = logging.getLogger(__name__)

def export_to_csv_with_profilers(results: List[RunResult], path: Path):
    """
    Exporta uma lista de RunResult para um único arquivo CSV, expandindo
    as métricas do profiler em colunas dedicadas.
    """
    if not results:
        logger.warning("⚠️ Nenhum resultado para exportar para CSV.")
        return

    # Obtém os nomes dos campos base do RunResult, excluindo as métricas do profiler
    base_field_names = [field.name for field in fields(RunResult) if field.name != 'profiler_metrics']
    
    # Descobre todas as chaves de métricas de profiler únicas em todos os resultados
    profiler_keys = set()
    for r in results:
        if r.profiler_metrics and r.profiler_metrics != "{}":
            try:
                metrics_dict = json.loads(r.profiler_metrics)
                profiler_keys.update(metrics_dict.keys())
            except json.JSONDecodeError:
                logger.warning(f"Falha ao decodificar JSON de profiler_metrics: {r.profiler_metrics}")
    
    # Ordena as chaves para uma ordem de coluna consistente
    sorted_profiler_keys = sorted(list(profiler_keys))
    
    # Combina os cabeçalhos
    all_field_names = base_field_names + sorted_profiler_keys

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline='', encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=all_field_names)
            writer.writeheader()
            
            for r in results:
                # Converte o resultado base para um dicionário
                row_dict = asdict(r)
                
                # Remove o campo original de métricas do profiler
                row_dict.pop('profiler_metrics', None)
                
                # Carrega as métricas do profiler da string JSON
                if r.profiler_metrics and r.profiler_metrics != "{}":
                    try:
                        profiler_data = json.loads(r.profiler_metrics)
                        # Adiciona as métricas do profiler ao dicionário da linha
                        row_dict.update(profiler_data)
                    except json.JSONDecodeError:
                        # Ignora se o JSON for inválido
                        pass
                
                writer.writerow(row_dict)
                
        logger.info(f"📄 Resultados detalhados salvos em CSV: {path}")
    except Exception as e:
        logger.error(f"❌ Erro ao salvar CSV detalhado: {e}", exc_info=True)


def export_to_json(results: List[RunResult], path: Path):
    """Exporta lista de RunResult para JSON"""
    if not results:
        return
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Salva o profiler_metrics como string JSON aninhada, o que é natural para este formato
        data = [asdict(r) for r in results]
        with path.open("w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        logger.info(f"📄 Resultados salvos em JSON: {path}")
    except Exception as e:
        logger.error(f"❌ Erro ao salvar JSON: {e}")

def export_failed_runs(results: List[RunResult], path: Path):
    """Salva erros e mensagens de falha separadamente para análise"""
    failures = [asdict(r) for r in results if r.status != "success"]
    if not failures:
        logger.info("✅ Nenhuma falha detectada.")
        return

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            json.dump(failures, f, indent=2)
        logger.info(f"📄 Falhas exportadas para: {path}")
    except Exception as e:
        logger.error(f"❌ Erro ao salvar falhas: {e}")

def export_summary_table(results: List[RunResult], path: Path):
    """Gera tabela com tempo médio e pico de memória por abordagem/configuração"""
    summary = defaultdict(list)
    for r in results:
        # Agrupa por configuração, abordagem e wrapper para um resumo mais granular
        key = (r.config, r.approach, r.wrapper)
        summary[key].append(r)

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline='', encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(["config", "approach", "wrapper", "n_success", "n_total", "mean_time_s", "mean_rss_mb", "mean_cpu_percent", "failure_rate_%"])

            for (config, approach, wrapper), runs in sorted(summary.items()):
                successes = [r for r in runs if r.status == "success"]
                total_runs = len(runs)
                failure_rate = ((total_runs - len(successes)) / total_runs) * 100 if total_runs > 0 else 0

                if successes:
                    mean_time = sum(r.elapsed_time for r in successes) / len(successes)
                    mean_rss = sum(r.max_rss_mb for r in successes) / len(successes)
                    mean_cpu = sum(r.cpu_percent_avg for r in successes) / len(successes)
                    writer.writerow([config, approach, wrapper, len(successes), total_runs,
                                     f"{mean_time:.3f}", f"{mean_rss:.2f}", f"{mean_cpu:.2f}", f"{failure_rate:.1f}"])
                else:
                    writer.writerow([config, approach, wrapper, 0, total_runs, "N/A", "N/A", "N/A", f"{failure_rate:.1f}"])
        logger.info(f"📊 Resumo tabular salvo: {path}")
    except Exception as e:
        logger.error(f"❌ Erro ao salvar resumo tabular: {e}")

def export_profiler_summary(results: List[RunResult], path: Path):
    """
    Exporta um resumo JSON com a média das métricas de profiler,
    agrupadas apenas por (approach, wrapper), ignorando config.
    """
    from collections import defaultdict

    # Estrutura: summary[approach][wrapper][metric_name] = [val1, val2, ...]
    summary = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))

    for r in results:
        if (
            r.status == "success"
            and r.profiler_metrics
            and r.profiler_metrics.strip() != "{}"
        ):
            try:
                metrics = json.loads(r.profiler_metrics)
                for key, value in metrics.items():
                    summary[r.approach][r.wrapper][key].append(value)
            except (json.JSONDecodeError, AttributeError, TypeError) as e:
                logger.warning(f"⚠️ Erro ao decodificar métricas: {e}")

    if not summary:
        logger.info("ℹ️ Nenhuma métrica de profiler para resumir.")
        return

    # Calcula média das métricas para cada (approach, wrapper)
    final_summary = defaultdict(dict)
    for approach, wrappers in summary.items():
        for wrapper, metrics in wrappers.items():
            final_summary[approach][wrapper] = {
                metric: sum(values) / len(values)
                for metric, values in metrics.items()
            }

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            json.dump(final_summary, f, indent=2)
        logger.info(f"📈 Resumo do profiler salvo: {path}")
    except Exception as e:
        logger.error(f"❌ Erro ao salvar resumo do profiler: {e}")
