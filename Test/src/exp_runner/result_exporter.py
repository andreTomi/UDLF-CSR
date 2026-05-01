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
    Exports a list of RunResults to a single CSV file, expanding
    profiler metrics into dedicated columns.
    """
    if not results:
        logger.warning("⚠️ No results to export to CSV.")
        return

    # Gets the base field names of RunResult, excluding profiler metrics
    base_field_names = [field.name for field in fields(RunResult) if field.name != 'profiler_metrics']
    
    # Discovers all unique profiler metric keys across all results
    profiler_keys = set()
    for r in results:
        if r.profiler_metrics and r.profiler_metrics != "{}":
            try:
                metrics_dict = json.loads(r.profiler_metrics)
                profiler_keys.update(metrics_dict.keys())
            except json.JSONDecodeError:
                logger.warning(f"Failed to decode JSON from profiler_metrics: {r.profiler_metrics}")
    
    # Sorts the keys for consistent column ordering
    sorted_profiler_keys = sorted(list(profiler_keys))
    
    # Combines headers
    all_field_names = base_field_names + sorted_profiler_keys

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline='', encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=all_field_names)
            writer.writeheader()
            
            for r in results:
                # Converts base result to a dictionary
                row_dict = asdict(r)
                
                # Removes the original profiler metrics field
                row_dict.pop('profiler_metrics', None)
                
                # Loads profiler metrics from the JSON string
                if r.profiler_metrics and r.profiler_metrics != "{}":
                    try:
                        profiler_data = json.loads(r.profiler_metrics)
                        # Adds profiler metrics to the row dictionary
                        row_dict.update(profiler_data)
                    except json.JSONDecodeError:
                        # Ignores if JSON is invalid
                        pass
                
                writer.writerow(row_dict)
                
        logger.info(f"📄 Detailed results saved to CSV: {path}")
    except Exception as e:
        logger.error(f"❌ Error saving detailed CSV: {e}", exc_info=True)


def export_to_json(results: List[RunResult], path: Path):
    """Exports list of RunResult to JSON"""
    if not results:
        return
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Saves profiler_metrics as a nested JSON string, which is natural for this format
        data = [asdict(r) for r in results]
        with path.open("w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        logger.info(f"📄 Results saved to JSON: {path}")
    except Exception as e:
        logger.error(f"❌ Error saving JSON: {e}")

def export_failed_runs(results: List[RunResult], path: Path):
    """Saves errors and failure messages separately for analysis"""
    failures = [asdict(r) for r in results if r.status != "success"]
    if not failures:
        logger.info("✅ No failures detected.")
        return

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as f:
            json.dump(failures, f, indent=2)
        logger.info(f"📄 Failures exported to: {path}")
    except Exception as e:
        logger.error(f"❌ Error saving failures: {e}")

def export_summary_table(results: List[RunResult], path: Path):
    """Generates table with average time and peak memory per approach/configuration"""
    summary = defaultdict(list)
    for r in results:
        # Groups by configuration, approach, and wrapper for a more granular summary
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
        logger.info(f"📊 Tabular summary saved: {path}")
    except Exception as e:
        logger.error(f"❌ Error saving tabular summary: {e}")

def export_profiler_summary(results: List[RunResult], path: Path):
    """
    Exports a JSON summary with average profiler metrics,
    grouped only by (approach, wrapper), ignoring config.
    """
    from collections import defaultdict

    # Structure: summary[approach][wrapper][metric_name] = [val1, val2, ...]
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
                logger.warning(f"⚠️ Error decoding metrics: {e}")

    if not summary:
        logger.info("ℹ️ No profiler metrics to summarize.")
        return

    # Calculates average metrics for each (approach, wrapper)
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
        logger.info(f"📈 Profiler summary saved: {path}")
    except Exception as e:
        logger.error(f"❌ Error saving profiler summary: {e}")
