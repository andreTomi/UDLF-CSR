# exp_runner/core.py
from dataclasses import dataclass, field
from typing import List
import platform
try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False
import logging

logger = logging.getLogger(__name__)

@dataclass
class SystemInfo:
    platform: str = ""
    processor: str = ""
    total_memory_gb: float = 0.0
    cpu_cores: int = 0
    cpu_threads: int = 0
    python_version: str = ""
    kernel_version: str = ""

@dataclass
class DetailedMetrics:
    rss_samples: List[float] = field(default_factory=list)
    vms_samples: List[float] = field(default_factory=list)
    cpu_samples: List[float] = field(default_factory=list)
    pss_mb: float = 0.0
    uss_mb: float = 0.0
    shared_mb: float = 0.0
    read_ops: int = 0
    write_ops: int = 0
    context_switches_vol: int = 0
    context_switches_invol: int = 0
    cpu_temp_max: float = 0.0
    cpu_temp_avg: float = 0.0
    thermal_throttling: bool = False
    user_time: float = 0.0
    system_time: float = 0.0
    page_faults_minor: int = 0
    page_faults_major: int = 0

@dataclass
class RunResult:
    config: str = ""
    approach: str = ""
    wrapper: str = ""
    run: int = 0
    elapsed_time: float = 0.0
    exit_code: int = 0
    status: str = ""
    error_message: str = ""
    max_rss_mb: float = 0.0
    avg_rss_mb: float = 0.0
    peak_vms_mb: float = 0.0
    cpu_percent_avg: float = 0.0
    read_ops: int = 0
    write_ops: int = 0
    page_faults_minor: int = 0
    page_faults_major: int = 0
    profiler_metrics: str = ""
    system_load_initial: float = 0.0
    memory_pressure_initial: float = 0.0

class SystemMonitor:
    @staticmethod
    def get_system_info() -> SystemInfo:
        """Coleta informações detalhadas do sistema"""
        if not HAS_PSUTIL:
            return SystemInfo(
                platform=f"{platform.system()} {platform.release()}",
                processor=platform.processor() or "Unknown",
                python_version=platform.python_version(),
                kernel_version=platform.version()
            )
            
        try:
            memory_gb = psutil.virtual_memory().total / (1024**3)
            cpu_info = psutil.cpu_count(logical=False), psutil.cpu_count(logical=True)
            
            return SystemInfo(
                platform=f"{platform.system()} {platform.release()}",
                processor=platform.processor() or "Unknown",
                total_memory_gb=round(memory_gb, 2),
                cpu_cores=cpu_info[0] or 0,
                cpu_threads=cpu_info[1] or 0,
                python_version=platform.python_version(),
                kernel_version=platform.version()
            )
        except Exception as e:
            logger.error(f"Erro ao obter informações do sistema: {e}")
            return SystemInfo()
