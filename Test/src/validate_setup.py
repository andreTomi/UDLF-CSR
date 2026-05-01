"""
Benchmarking Setup Validation Script

Verifies if all necessary components are installed and correctly configured
before executing the complete test matrix.
"""

import sys
import subprocess
import logging
from pathlib import Path
from typing import List, Tuple

logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s: %(message)s"
)
logger = logging.getLogger(__name__)

# Configurations (must match run_experiments_updated.py)
ALL_METHODS = [
    "RKGRAPH", "CONTEXTRR", "CORGRAPH", "CPRR", "RECKNNGRAPH", 
    "RLRECOM", "RLSIM", "LHRR", "BFSTREE", "RDPAC", "RFE"
]

_binary_path = str(Path(__file__).resolve().parent.parent.parent / "Source" / "udlf")
EXECUTABLES = {
    "mmap": _binary_path,
    "batch": _binary_path,
}

BASE_CONFIGS = [Path("config")]


def check_python_packages() -> bool:
    """Verifies if the required Python packages are installed."""
    logger.info("🐍 Verifying Python packages...")
    
    required_packages = [
        'pandas',
        'matplotlib',
        'seaborn',
        'numpy',
        'psutil',
        'configparser'
    ]
    
    missing = []
    for package in required_packages:
        try:
            __import__(package)
            logger.info(f"  ✓ {package}")
        except ImportError:
            logger.error(f"  ✗ {package} - NOT INSTALLED")
            missing.append(package)
    
    if missing:
        logger.error(f"\n❌ Missing packages: {', '.join(missing)}")
        logger.info(f"💡 Install with: pip install {' '.join(missing)}")
        return False
    
    logger.info("✅ All Python packages are installed\n")
    return True


def check_system_tools() -> bool:
    """Verifies if the system profiling tools are available."""
    logger.info("🔧 Verifying system tools...")
    
    tools = {
        'perf': 'Performance profiling (hardware events)',
        'time': 'Time and resource measurement',
    }
    
    available = []
    missing = []
    
    for tool, description in tools.items():
        try:
            if tool == 'time':
                # Check for GNU time specifically
                result = subprocess.run(
                    ['/usr/bin/time', '--version'],
                    capture_output=True,
                    timeout=2
                )
                if result.returncode == 0:
                    logger.info(f"  ✓ {tool} - {description}")
                    available.append(tool)
                else:
                    raise FileNotFoundError
            else:
                result = subprocess.run(
                    [tool, '--version'],
                    capture_output=True,
                    timeout=2
                )
                if result.returncode == 0:
                    logger.info(f"  ✓ {tool} - {description}")
                    available.append(tool)
                else:
                    raise FileNotFoundError
        except (subprocess.TimeoutExpired, FileNotFoundError, PermissionError):
            logger.warning(f"  ⚠ {tool} - NOT AVAILABLE ({description})")
            missing.append(tool)
    
    if not available:
        logger.error("\n❌ No profiling tool available!")
        return False
    
    if missing:
        logger.warning(f"\n⚠️  Missing tools: {', '.join(missing)}")
        logger.info("💡 Install perf: sudo apt-get install linux-tools-generic")
        logger.info("   Benchmarking will continue only with available tools\n")
    else:
        logger.info("✅ All system tools are available\n")
    
    return True


def check_executables() -> bool:
    """Verifies if the application executables exist and have execution permissions."""
    logger.info("⚙️  Verifying application executables...")
    
    all_ok = True
    for name, path in EXECUTABLES.items():
        exe_path = Path(path)
        if exe_path.exists():
            if exe_path.is_file() and exe_path.stat().st_mode & 0o111:
                logger.info(f"  ✓ {name}: {path}")
            else:
                logger.error(f"  ✗ {name}: {path} exists but is NOT EXECUTABLE")
                logger.info(f"     Run: chmod +x {path}")
                all_ok = False
        else:
            logger.error(f"  ✗ {name}: {path} - NOT FOUND")
            all_ok = False
    
    if not all_ok:
        logger.error("\n❌ Missing executables or execution permission denied")
        logger.info("💡 Compile the executables before proceeding")
        return False
    
    logger.info("✅ All executables are available\n")
    return True


def check_config_files() -> Tuple[bool, List[Path]]:
    """Verifies if base configuration files exist."""
    logger.info("📁 Verifying configuration files...")
    
    all_configs = []
    for config_dir in BASE_CONFIGS:
        if not config_dir.exists():
            logger.error(f"  ✗ Directory not found: {config_dir}")
            continue
        
        configs = list(config_dir.glob("*.ini"))
        if not configs:
            logger.warning(f"  ⚠ No .ini files found in {config_dir}")
        else:
            logger.info(f"  ✓ {len(configs)} .ini file(s) found in {config_dir}")
            for config in configs:
                logger.info(f"    - {config.name}")
                all_configs.append(config)
    
    if not all_configs:
        logger.error("\n❌ No configuration files found!")
        logger.info("💡 Create .ini files in config/ before proceeding")
        return False, []
    
    logger.info(f"✅ A total of {len(all_configs)} base configuration(s) found\n")
    return True, all_configs


def estimate_execution_time(num_configs: int) -> None:
    """Estimates the total execution time."""
    logger.info("⏱️  Estimating execution time...")
    
    num_methods = len(ALL_METHODS)
    num_approaches = len(EXECUTABLES)
    num_wrappers = 2  # perf and time
    repetitions_per_wrapper = 5
    
    total_runs = num_configs * num_methods * num_approaches * num_wrappers * repetitions_per_wrapper
    
    # Conservative estimate: 2 minutes per run (depends on dataset)
    avg_time_per_run_minutes = 2
    total_time_minutes = total_runs * avg_time_per_run_minutes
    total_time_hours = total_time_minutes / 60
    
    logger.info(f"  📊 Base configurations: {num_configs}")
    logger.info(f"  🧮 Methods: {num_methods}")
    logger.info(f"  ⚙️  Approaches: {num_approaches}")
    logger.info(f"  🔧 Wrappers: {num_wrappers}")
    logger.info(f"  🔄 Repetitions per wrapper: {repetitions_per_wrapper}")
    logger.info(f"  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    logger.info(f"  🔢 Total executions: {total_runs}")
    logger.info(f"  ⏱️  Estimated time: ~{total_time_hours:.1f} hours")
    logger.info(f"     (assuming ~{avg_time_per_run_minutes} min/run)\n")
    
    if total_time_hours > 24:
        logger.warning("⚠️  The complete benchmarking will take more than 24 hours!")
        logger.info("💡 Consider:")
        logger.info("   - Reducing the number of repetitions in WRAPPERS")
        logger.info("   - Running only a subset of methods initially")
        logger.info("   - Using smaller datasets for preliminary tests\n")


def check_disk_space() -> bool:
    """Verifies available disk space."""
    logger.info("💾 Verifying disk space...")
    
    try:
        import shutil
        stat = shutil.disk_usage('.')
        
        free_gb = stat.free / (1024**3)
        total_gb = stat.total / (1024**3)
        used_percent = (stat.used / stat.total) * 100
        
        logger.info(f"  Total: {total_gb:.1f} GB")
        logger.info(f"  Used: {used_percent:.1f}%")
        logger.info(f"  Free: {free_gb:.1f} GB")
        
        # Recommendation: at least 10 GB free
        if free_gb < 10:
            logger.warning("\n⚠️  Less than 10 GB of free space!")
            logger.info("💡 The results can take up several GB depending on the number of executions")
            return False
        
        logger.info("✅ Sufficient disk space\n")
        return True
        
    except Exception as e:
        logger.warning(f"⚠️  Could not verify disk space: {e}\n")
        return True


def main():
    """Runs all verifications."""
    logger.info("="*80)
    logger.info("🔍 BENCHMARKING SETUP VALIDATION")
    logger.info("="*80)
    logger.info("")
    
    checks = []
    
    # Execute all checks
    checks.append(("Python Packages", check_python_packages()))
    checks.append(("System Tools", check_system_tools()))
    checks.append(("Executables", check_executables()))
    
    config_ok, config_files = check_config_files()
    checks.append(("Configuration Files", config_ok))
    
    checks.append(("Disk Space", check_disk_space()))
    
    if config_ok:
        estimate_execution_time(len(config_files))
    
    # Final Summary
    logger.info("="*80)
    logger.info("📋 VALIDATION SUMMARY")
    logger.info("="*80)
    
    all_passed = True
    for check_name, passed in checks:
        status = "✅ PASS" if passed else "❌ FAIL"
        logger.info(f"  {status} - {check_name}")
        if not passed:
            all_passed = False
    
    logger.info("")
    
    if all_passed:
        logger.info("="*80)
        logger.info("✅ SETUP SUCCESSFULLY VALIDATED!")
        logger.info("="*80)
        logger.info("You can proceed with:")
        logger.info("  1. python src/run_experiments_updated.py")
        logger.info("  2. python src/generate_plots.py")
        logger.info("")
        return 0
    else:
        logger.error("="*80)
        logger.error("❌ VALIDATION FAILED")
        logger.error("="*80)
        logger.error("Fix the issues above before proceeding.")
        logger.info("See BENCHMARKING_README.md for more details.")
        logger.info("")
        return 1


if __name__ == "__main__":
    sys.exit(main())
