import argparse
import sys
import logging
import random
import json
import tempfile
import configparser
import time
from datetime import datetime, timedelta
from dataclasses import asdict
from pathlib import Path
import os
from exp_runner import (
    core, config_loader, profiling, execution, result_exporter, utils, profiler_parser
)

import signal

def cleanup_handler(signum, frame):
    logger.info("\n🛑 Interrupt detected! Cleaning active processes...")
    execution.kill_active_process()
    logger.info("✅ Cleanup complete. Exiting.")
    sys.exit(1)

# Register signals
signal.signal(signal.SIGINT, cleanup_handler)
signal.signal(signal.SIGTERM, cleanup_handler)

# Logging setup
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

# Full list of re-ranking methods
ALL_METHODS = [
    # "CPRR_MKL",
    # "CPRR_BLAS",
    # "CPRR_EIGEN",
    # "LHRR_MKL",
    # "LHRR_BLAS",
    # "LHRR_EIGEN",
    "CPRR_SPARSE",
    "LHRR_SPARSE",
    "CPRR",
    "LHRR",
]

# Base configuration directories
BASE_CONFIGS = [
    Path("config"),  # Directory containing base configs
]

# Executables to be compared
_binary_path = str(Path(__file__).resolve().parent.parent.parent / "Source" / "udlf")
EXECUTABLES = {
    # "mkl": _binary_path,
    "csr": _binary_path,
    # "eigen": _binary_path,
    "mmap": _binary_path, 
    "swap": _binary_path,
}

# Mapping of allowed methods per executable
APPROACH_METHODS_MAP = {
    "csr": ["CPRR_SPARSE", "LHRR_SPARSE"],
    "mmap": ["CPRR", "LHRR"],
    "swap": ["CPRR", "LHRR"],
    # "mkl": ["CPRR_MKL", "LHRR_MKL", "CPRR_BLAS", "LHRR_BLAS"],
    # "eigen": ["CPRR_EIGEN", "LHRR_EIGEN"],
}

# Wrappers with execution count (N repetitions)
WRAPPERS = {
    "perf": 10, 
}

TIMEOUT_WRAPPER = 1800  # 2 hours


class ProgressTracker:
    """Tracks progress and estimates completion time for batch testing."""
    
    def __init__(self, total_runs):
        self.total_runs = total_runs
        self.completed_runs = 0
        self.start_time = time.time()
        self.run_times = []  # Store individual run times for better estimates
        
    def update(self, run_time):
        """Update progress with a completed run."""
        self.completed_runs += 1
        self.run_times.append(run_time)
        
    def get_progress_pct(self):
        """Get progress as percentage."""
        if self.total_runs == 0:
            return 100.0
        return (self.completed_runs / self.total_runs) * 100
    
    def get_eta(self):
        """Estimate time remaining based on average run time."""
        if self.completed_runs == 0:
            return "Calculating..."
        
        avg_time = sum(self.run_times) / len(self.run_times)
        remaining_runs = self.total_runs - self.completed_runs
        eta_seconds = avg_time * remaining_runs
        
        return str(timedelta(seconds=int(eta_seconds)))
    
    def get_elapsed(self):
        """Get elapsed time since start."""
        elapsed = time.time() - self.start_time
        return str(timedelta(seconds=int(elapsed)))
    
    def get_estimated_completion(self):
        """Get estimated completion datetime."""
        if self.completed_runs == 0:
            return "Calculating..."
        
        avg_time = sum(self.run_times) / len(self.run_times)
        remaining_runs = self.total_runs - self.completed_runs
        eta_seconds = avg_time * remaining_runs
        
        completion_time = datetime.now() + timedelta(seconds=eta_seconds)
        return completion_time.strftime("%Y-%m-%d %H:%M:%S")
    
    def log_progress(self, logger):
        """Log current progress with ETA."""
        logger.info(f"📊 Progress: {self.completed_runs}/{self.total_runs} ({self.get_progress_pct():.1f}%)")
        logger.info(f"⏱️  Elapsed: {self.get_elapsed()} | ETA: {self.get_eta()}")
        logger.info(f"🎯 Estimated completion: {self.get_estimated_completion()}")


def calculate_total_runs(config_files, methods, executables, wrappers):
    """Calculates the total estimated executions."""
    total = 0
    available_wrappers = {w for w in wrappers if w in profiling.get_available_wrappers()}
    
    for wrapper_name, run_count in wrappers.items():
        if wrapper_name in available_wrappers:
            for approach in executables.keys():
                valid_methods = APPROACH_METHODS_MAP.get(approach, methods)
                # Count only the methods that are in 'methods' (the active ones)
                active_valid_methods = [m for m in methods if m in valid_methods]
                total += len(config_files) * len(active_valid_methods) * run_count
            
    return total


def json_dump_pretty(obj):
    return json.dumps(obj, indent=2, ensure_ascii=False)


def create_temp_config_with_method(base_config_path: Path, method_name: str, temp_dir: Path) -> Path:
    """Creates a temporary config file with UDL_METHOD set and absolute paths."""
    
    base_dir = Path.cwd()
    
    with open(base_config_path, 'r') as f:
        lines = f.readlines()
    
    updated_lines = []
    udl_method_found = False
    
    for line in lines:
        stripped_line = line.strip()
        
        if stripped_line.startswith('#') or not stripped_line:
            updated_lines.append(line)
            continue
        
        if '=' in stripped_line:
            key, value = stripped_line.split('=', 1)
            key = key.strip()
            value = value.strip()
            
            if key == 'UDL_METHOD':
                updated_lines.append(f"UDL_METHOD = {method_name}\n")
                udl_method_found = True
            # Convert relative paths to absolute paths
            elif key in ['INPUT_FILE', 'INPUT_FILE_LIST', 'INPUT_FILE_CLASSES']:
                if not value.startswith('/'):
                    absolute_path = base_dir / value
                    updated_lines.append(f"{key} = {absolute_path}\n")
                else:
                    updated_lines.append(line)
            elif key == 'INPUT_IMAGES_PATH':
                # Special handling: preserve trailing slash for directories
                if not value.startswith('/'):
                    absolute_path = base_dir / value
                    # Ensure trailing slash is preserved
                    path_str = str(absolute_path)
                    if value.endswith('/') and not path_str.endswith('/'):
                        path_str += '/'
                    updated_lines.append(f"{key} = {path_str}\n")
                else:
                    updated_lines.append(line)
            else:
                updated_lines.append(line)
        else:
            updated_lines.append(line)
    
    if not udl_method_found:
        updated_lines.append(f"UDL_METHOD = {method_name}\n")
    
    temp_config_path = temp_dir / f"{base_config_path.stem}__{method_name}.ini"
    with open(temp_config_path, 'w') as f:
        f.writelines(updated_lines)
    
    return temp_config_path
    
def profile_approach(run_count: int,
                     config_name: str,
                     approach: str,
                     wrapper_name: str,
                     exe_path: str,
                     config_file: Path,
                     temp_dir: Path,
                     output_path: Path,
                     progress_tracker=None,
                     timeout_file_path: str = None,
                     global_timeouts: set = None) -> list:
    """Executes N runs of an approach with profiling."""
    results = []
    
    timeout_key = f"{config_name}__{approach}__{wrapper_name}"
    if global_timeouts is not None and timeout_key in global_timeouts:
        logger.warning(f"⏩ Skipping {timeout_key} due to previous timeout.")
        for _ in range(run_count):
            if progress_tracker:
                progress_tracker.completed_runs += 1
        return []

    for run in range(1, run_count + 1):
        run_start_time = time.time()
        
        logger.info(
            f"Approach: {approach} | Wrapper: {wrapper_name} | {config_name} | (run {run}/{run_count})"
        )
        
        # Command is built to use the temporary directory
        cmd = profiling.build_wrapper_command(
            wrapper_name, exe_path, str(config_file), temp_dir, run, approach
        )
        
        # The process log also goes to the temporary directory and will be discarded
        log_path = temp_dir / f"{config_name}_{approach}_{wrapper_name}_run_{run}.log"
        
        result = execution.run_with_advanced_monitoring(cmd, TIMEOUT_WRAPPER, log_path)

        if result.status == "timeout":
            logger.warning(f"⚠️ Timeout ({TIMEOUT_WRAPPER}s) in {config_name} with {approach} using {wrapper_name}")
            logger.warning(f"   Elapsed time: {result.elapsed_time:.1f}s | Partial results saved")
            
            if global_timeouts is not None and timeout_file_path is not None:
                if timeout_key not in global_timeouts:
                    global_timeouts.add(timeout_key)
                    try:
                        with open(timeout_file_path, 'w') as tf:
                            json.dump(list(global_timeouts), tf)
                    except Exception as e:
                        logger.error(f"Error saving timeout_file: {e}")
            
            result.config = config_name
            result.approach = approach
            result.wrapper = wrapper_name
            result.run = run
            result.profiler_metrics = "{}"
            results.append(result)
            
            if progress_tracker:
                progress_tracker.update(time.time() - run_start_time)
                # Advance progress for runs that will be aborted
                remaining = run_count - run
                progress_tracker.completed_runs += remaining
                
            break
        
        # Fill result information
        result.config = config_name
        result.approach = approach
        result.wrapper = wrapper_name
        result.run = run
        result.threads = int(os.environ.get("OMP_NUM_THREADS", 1))
        
        # Update progress tracker
        run_elapsed = time.time() - run_start_time
        if progress_tracker:
            progress_tracker.update(run_elapsed)
        
        # Parse profiler metrics from temporary directory
        if result.status == "success":
            try:
                profiler_metrics = profiler_parser.parse_profiler_results(
                    temp_dir, wrapper_name, run, approach
                )
                result.profiler_metrics = json.dumps(profiler_metrics) if profiler_metrics else "{}"
                
                if profiler_metrics:
                    logger.debug(f"  -> {wrapper_name} metrics parsed successfully.")

            except Exception as e:
                logger.warning(f"⚠️ Error parsing {wrapper_name} metrics (run {run}): {e}")
                result.profiler_metrics = "{}"
        else:
            logger.warning(f"⚠️ Error executing {config_name} with {approach} using {wrapper_name} (run {run})")
            result.profiler_metrics = "{}"
        
        results.append(result)
    
    # Save partial results directly in the output directory
    logger.info("💾 Saving partial results...")
    result_exporter.export_to_csv_with_profilers(results, output_path / "detailed_results.csv")
    result_exporter.export_to_json(results, output_path / "detailed_results.json")
    result_exporter.export_failed_runs(results, output_path / "failures.json")
    result_exporter.export_summary_table(results, output_path / "summary_table.csv")
    result_exporter.export_profiler_summary(results, output_path / "profiler_summary.json")

    logger.info(f"✅ Execution result saved")
    return results


def run(config_path: Path, output_path: Path, timeout_file_path: str = None, global_timeouts: set = None):
    """
    Main function that orchestrates the complete test matrix.
    
    For each base .ini file found in config_path:
        - Generates 11 temporary configs (one per method)
        - Executes each config with mmap and batch
        - Executes N repetitions with each wrapper
    """
    logger.info(f"🚀 Starting tests in {config_path}")
    
    utils.ensure_directory_exists(output_path)

    # Load base configuration files
    base_config_files = config_loader.load_configs_from_directory(config_path, extension=".ini")
    if not base_config_files:
        logger.error("❌ No configuration found in directory: %s", config_path)
        return

    # Check available wrappers
    available = profiling.get_available_wrappers()
    available_wrappers = {}
    
    for name, count in WRAPPERS.items():
        if name in available:
            available_wrappers[name] = count
        elif name == "perf" and "time" in available:
            logger.warning("⚠️ 'perf' not available or permission denied. Using 'time' as automatic fallback.")
            available_wrappers["time"] = count
            
    if not available_wrappers:
        logger.warning("⚠️ No requested wrapper is available. Running without wrappers (base metrics only).")
        available_wrappers["none"] = 1
        
    logger.info(f"🔧 Active wrappers: {list(available_wrappers.keys())}")
    
    # Calculate total executions
    total = calculate_total_runs(base_config_files, ALL_METHODS, EXECUTABLES, available_wrappers)
    logger.info(f"🔢 Total estimated executions: {total}")
    logger.info(f"📊 Base datasets: {len(base_config_files)}")
    logger.info(f"🧮 Methods: {len(ALL_METHODS)}")
    logger.info(f"⚙️  Approaches: {len(EXECUTABLES)}")
    logger.info(f"🔁 Repetitions per wrapper: {list(available_wrappers.values())}")
    
    # Initialize progress tracker
    progress_tracker = ProgressTracker(total)
    logger.info("⏱️  Progress tracking started")

    # List to store all individual results
    all_results = []
    
    with tempfile.TemporaryDirectory(prefix="exp_runner_") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        logger.info(f"📦 Using temporary directory for artifacts: {temp_dir}")

        # Loop over each base config (each dataset)
        for base_config_file in base_config_files:
            base_config_name = base_config_file.stem
            logger.info(f"\n{'='*80}")
            logger.info(f"📁 Processing base dataset: {base_config_name}")
            logger.info(f"{'='*80}")
            
            # Loop over each re-ranking method
            for method in ALL_METHODS:
                logger.info(f"\n🔬 Method: {method}")
                
                # Create temporary config with current method
                temp_config_file = create_temp_config_with_method(
                    base_config_file, method, temp_dir
                )
                
                # Unique name to identify this dataset+method combination
                config_name = f"{base_config_name}__{method}"
                
                # Loop over each approach (mmap, batch)
                for approach, exe_path in EXECUTABLES.items():
                    if not Path(exe_path).exists():
                        logger.warning(f"⚠️ Executable missing, skipping approach '{approach}': {exe_path}")
                        continue
                        
                    # Check if the method is compatible with the approach
                    valid_methods = APPROACH_METHODS_MAP.get(approach, ALL_METHODS)
                    if method not in valid_methods:
                        logger.debug(f"⏩ Skipping incompatible combination: {approach} does not run with {method}")
                        continue

                    # Loop over each wrapper (perf, time)
                    for wrapper_name, run_count in available_wrappers.items():
                        out = Path(f"{output_path}/{approach}/{wrapper_name}/{config_name}")
                        utils.ensure_directory_exists(out)
                        
                        # Execute N repetitions and collect results
                        partial_results = profile_approach(
                            run_count, config_name, approach, wrapper_name, 
                            exe_path, temp_config_file, temp_dir, out, progress_tracker,
                            timeout_file_path, global_timeouts
                        )
                        all_results.extend(partial_results)
                        
                        # Incremental saving in root folder to avoid data loss
                        logger.info("💾 [Incremental] Updating main results with current batch.")
                        result_exporter.export_to_csv_with_profilers(all_results, output_path / "all_detailed_results.csv")
                        result_exporter.export_summary_table(all_results, output_path / "all_summary_table.csv")
                        export_aggregated_summary(all_results, output_path / "all_summary_table_with_profilers.csv")
                        
                        # Log progress after each config completion
                        logger.info("")
                        progress_tracker.log_progress(logger)
                        logger.info("")

    # Export final consolidated results
    logger.info(f"\n{'='*80}")
    logger.info("📊 Exporting final consolidated results...")
    logger.info(f"{'='*80}")
    
    result_exporter.export_to_csv_with_profilers(all_results, output_path / "all_detailed_results.csv")
    result_exporter.export_to_json(all_results, output_path / "all_detailed_results.json")
    result_exporter.export_failed_runs(all_results, output_path / "all_failures.json")
    result_exporter.export_summary_table(all_results, output_path / "all_summary_table.csv")
    result_exporter.export_profiler_summary(all_results, output_path / "all_profiler_summary.json")
    
    # Create aggregated version with averages of profiler metrics by (config, approach, wrapper)
    export_aggregated_summary(all_results, output_path / "all_summary_table_with_profilers.csv")
    
    logger.info(f"✅ All consolidated results saved with {len(all_results)} executions")


def export_aggregated_summary(results: list, path: Path):
    """
    Exports a CSV with averages of ALL metrics aggregated by (config, approach, wrapper).
    
    This CSV is the main file for analysis, containing:
    - Base metrics (time, memory)
    - Averages of all perf metrics (IPC, cache miss rate, page faults, etc.)
    - Averages of all time metrics (major/minor page faults, etc.)
    - Standard deviations for error bars
    """
    from collections import defaultdict
    import statistics
    
    # Group by (config, approach, wrapper, threads)
    grouped = defaultdict(list)
    for r in results:
        if r.status == "success":
            key = (r.config, r.approach, r.wrapper, getattr(r, "threads", 1))
            grouped[key].append(r)
    
    if not grouped:
        logger.warning("⚠️ No successful results to aggregate.")
        return
    
    # Collect all possible metrics
    all_metric_keys = set()
    for runs in grouped.values():
        for r in runs:
            if r.profiler_metrics and r.profiler_metrics != "{}":
                try:
                    metrics = json.loads(r.profiler_metrics)
                    all_metric_keys.update(metrics.keys())
                except json.JSONDecodeError:
                    pass
    
    # Sort metrics for consistent order
    sorted_metric_keys = sorted(list(all_metric_keys))
    
    # CSV headers
    headers = [
        "config", "approach", "wrapper", "threads", "n_success", "n_total",
        "mean_time_elapsed_time_sec", "std_time_elapsed_time_sec",
        "mean_max_rss_mb", "std_max_rss_mb",
        "mean_cpu_percent_avg", "std_cpu_percent_avg"
    ]
    
    # Add columns for each profiler metric (mean and standard deviation)
    for metric in sorted_metric_keys:
        headers.append(f"mean_{metric}")
        headers.append(f"std_{metric}")
    
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        
        with path.open("w", newline='', encoding="utf-8") as f:
            import csv
            writer = csv.DictWriter(f, fieldnames=headers)
            writer.writeheader()
            
            for (config, approach, wrapper, threads), runs in sorted(grouped.items()):
                row = {
                    "config": config,
                    "approach": approach,
                    "wrapper": wrapper,
                    "threads": threads,
                    "n_success": len(runs),
                    "n_total": len(runs)
                }
                
                # Calculate means and deviations of base metrics
                times = [r.elapsed_time for r in runs]
                rss_values = [r.max_rss_mb for r in runs]
                cpu_values = [r.cpu_percent_avg for r in runs]
                
                row["mean_time_elapsed_time_sec"] = statistics.mean(times)
                row["std_time_elapsed_time_sec"] = statistics.stdev(times) if len(times) > 1 else 0
                row["mean_max_rss_mb"] = statistics.mean(rss_values)
                row["std_max_rss_mb"] = statistics.stdev(rss_values) if len(rss_values) > 1 else 0
                row["mean_cpu_percent_avg"] = statistics.mean(cpu_values)
                row["std_cpu_percent_avg"] = statistics.stdev(cpu_values) if len(cpu_values) > 1 else 0
                
                # Collect values for each profiler metric
                metric_values = defaultdict(list)
                for r in runs:
                    if r.profiler_metrics and r.profiler_metrics != "{}":
                        try:
                            metrics = json.loads(r.profiler_metrics)
                            for key, value in metrics.items():
                                if isinstance(value, (int, float)):
                                    metric_values[key].append(value)
                        except json.JSONDecodeError:
                            pass
                
                # Calculate mean and deviation of each profiler metric
                for metric in sorted_metric_keys:
                    if metric in metric_values and metric_values[metric]:
                        values = metric_values[metric]
                        row[f"mean_{metric}"] = statistics.mean(values)
                        row[f"std_{metric}"] = statistics.stdev(values) if len(values) > 1 else 0
                    else:
                        row[f"mean_{metric}"] = None
                        row[f"std_{metric}"] = None
                
                writer.writerow(row)
        
        logger.info(f"📊 Aggregated summary with profilers saved: {path}")
        logger.info(f"   Included metrics: {len(sorted_metric_keys)} profiler metrics")
        
    except Exception as e:
        logger.error(f"❌ Error saving aggregated summary: {e}", exc_info=True)


def main():
    parser = argparse.ArgumentParser(description="Run UDLF benchmarks.")
    parser.add_argument("--output-dir", type=str, default="benchmark_outputs", help="Output directory")
    parser.add_argument("--timeout-file", type=str, default=None, help="JSON file tracking timeouts")
    args = parser.parse_args()

    logger.info("🚀 Starting complete benchmarking experiment...")
    logger.info(f"🧮 Methods to test: {', '.join(ALL_METHODS)}")
    
    main_output_path = Path(args.output_dir)
    utils.ensure_directory_exists(main_output_path)

    # Load timeouts
    global_timeouts = set()
    if args.timeout_file and Path(args.timeout_file).exists():
        try:
            with open(args.timeout_file, 'r') as tf:
                global_timeouts = set(json.load(tf))
                logger.info(f"⚠️ Timeout file loaded with {len(global_timeouts)} configurations.")
        except Exception as e:
            logger.error(f"Error loading timeout_file: {e}")

    # Save system info
    system_info = core.SystemMonitor.get_system_info()
    (main_output_path / "system_info.json").write_text(
        json_dump_pretty(asdict(system_info))
    )

    # Run test matrix for each config directory
    for config_path in BASE_CONFIGS:
        if not config_path.exists():
            logger.warning(f"⚠️ Config directory not found: {config_path}")
            continue
        
        run(config_path, Path(f"{main_output_path}/{config_path.name}_output"), args.timeout_file, global_timeouts)
    
    logger.info("\n" + "="*80)
    logger.info("✅ BENCHMARKING COMPLETE!")
    logger.info("="*80)
    logger.info(f"📁 Results saved in: {main_output_path}")
    logger.info(f"📊 Main file for analysis: all_summary_table_with_profilers.csv")


if __name__ == "__main__":
    main()
