# exp_runner/utils.py
import hashlib
import os
import logging
from pathlib import Path

logger = logging.getLogger(__name__)

def ensure_directory_exists(path: Path) -> Path:
    """Creates directory if it does not exist"""
    try:
        path.mkdir(parents=True, exist_ok=True)
        return path
    except Exception as e:
        logger.error(f"Error creating directory: {path} - {e}")
        raise

def hash_file_sha256(file_path: Path, block_size: int = 65536) -> str:
    """Generates SHA-256 hash of a file (useful for ensuring reproducibility)"""
    sha256 = hashlib.sha256()
    try:
        with open(file_path, "rb") as f:
            while chunk := f.read(block_size):
                sha256.update(chunk)
        return sha256.hexdigest()
    except Exception as e:
        logger.error(f"Error calculating hash for {file_path}: {e}")
        return "ERROR"

def format_log_separator(title: str = "") -> str:
    """Returns a formatted separator with a title"""
    separator = "=" * 80
    if title:
        return f"{separator}\n{title.upper()}\n{separator}\n"
    return f"{separator}\n"

def sanitize_filename(name: str) -> str:
    """Removes problematic characters for filenames"""
    return "".join(c if c.isalnum() or c in "_-." else "_" for c in name)

def format_experiment_name(config_name: str, approach: str) -> str:
    """Creates a standardized unique name for an experiment combination"""
    return sanitize_filename(f"{config_name}__{approach}")

def get_cpu_thread_count(logical: bool = True) -> int:
    """Returns the number of threads in the system"""
    try:
        import psutil
        return psutil.cpu_count(logical=logical)
    except ImportError:
        return os.cpu_count()
