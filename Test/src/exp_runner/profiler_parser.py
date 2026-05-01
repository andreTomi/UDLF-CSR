# exp_runner/profiler_parser.py
import re
import logging
from pathlib import Path
from typing import Dict, Optional, Any
from dataclasses import dataclass

logger = logging.getLogger(__name__)

@dataclass
class PerfMetrics:
    cache_misses: Optional[int] = None
    cache_references: Optional[int] = None
    cycles: Optional[int] = None
    instructions: Optional[int] = None
    page_faults: Optional[int] = None
    context_switches: Optional[int] = None
    cpu_migrations: Optional[int] = None
    branch_misses: Optional[int] = None
    branches: Optional[int] = None
    l1_dcache_loads: Optional[int] = None
    l1_dcache_load_misses: Optional[int] = None
    llc_loads: Optional[int] = None
    llc_load_misses: Optional[int] = None
    task_clock_ms: Optional[float] = None
    cpu_clock_ms: Optional[float] = None
    cache_miss_rate: Optional[float] = None
    branch_miss_rate: Optional[float] = None
    ipc: Optional[float] = None  # Instructions per cycle

@dataclass
class TimeMetrics:
    user_time_sec: Optional[float] = None
    system_time_sec: Optional[float] = None
    elapsed_time_sec: Optional[float] = None
    cpu_percent: Optional[float] = None
    max_resident_set_kb: Optional[int] = None
    average_resident_set_kb: Optional[int] = None
    major_page_faults: Optional[int] = None
    minor_page_faults: Optional[int] = None
    voluntary_context_switches: Optional[int] = None
    involuntary_context_switches: Optional[int] = None
    swaps: Optional[int] = None
    file_system_inputs: Optional[int] = None
    file_system_outputs: Optional[int] = None
    socket_messages_sent: Optional[int] = None
    socket_messages_received: Optional[int] = None

@dataclass
class StraceMetrics:
    total_calls: Optional[int] = None
    total_errors: Optional[int] = None
    memory_calls: Optional[int] = None
    file_calls: Optional[int] = None
    process_calls: Optional[int] = None
    total_time_sec: Optional[float] = None
    avg_call_time_us: Optional[float] = None

def parse_perf_output(file_path: Path) -> PerfMetrics:
    """Parse do arquivo de saída do perf stat"""
    metrics = PerfMetrics()
    
    if not file_path.exists():
        logger.warning(f"Arquivo perf não encontrado: {file_path}")
        return metrics
    
    try:
        content = file_path.read_text(encoding='utf-8', errors='ignore')
        
        # Padrões para extrair métricas do perf
        patterns = {
            'cache_misses': r'([\d,]+)\s+cache-misses',
            'cache_references': r'([\d,]+)\s+cache-references',
            'cycles': r'([\d,]+)\s+cycles',
            'instructions': r'([\d,]+)\s+instructions',
            'page_faults': r'([\d,]+)\s+page-faults',
            'context_switches': r'([\d,]+)\s+context-switches',
            'cpu_migrations': r'([\d,]+)\s+cpu-migrations',
            'branch_misses': r'([\d,]+)\s+branch-misses',
            'branches': r'([\d,]+)\s+branches',
            'l1_dcache_loads': r'([\d,]+)\s+L1-dcache-loads',
            'l1_dcache_load_misses': r'([\d,]+)\s+L1-dcache-load-misses',
            'llc_loads': r'([\d,]+)\s+LLC-loads',
            'llc_load_misses': r'([\d,]+)\s+LLC-load-misses',
            'task_clock_ms': r'([\d,\.]+)\s+msec\s+task-clock',
            'cpu_clock_ms': r'([\d,\.]+)\s+msec\s+cpu-clock'
        }
        
        for metric, pattern in patterns.items():
            match = re.search(pattern, content, re.IGNORECASE)
            if match:
                value_str = match.group(1).replace(',', '')
                try:
                    if 'ms' in metric:
                        setattr(metrics, metric, float(value_str))
                    else:
                        setattr(metrics, metric, int(value_str))
                except ValueError:
                    continue
        
        # Calcula métricas derivadas
        if metrics.cache_references and metrics.cache_misses and metrics.cache_references > 0:
            metrics.cache_miss_rate = (metrics.cache_misses / metrics.cache_references) * 100
        
        if metrics.branches and metrics.branch_misses and metrics.branches > 0:
            metrics.branch_miss_rate = (metrics.branch_misses / metrics.branches) * 100
            
        if metrics.instructions and metrics.cycles and metrics.cycles > 0:
            metrics.ipc = metrics.instructions / metrics.cycles
            
    except Exception as e:
        logger.error(f"Erro ao parsear perf output {file_path}: {e}")
    
    return metrics

def parse_time_output(file_path: Path) -> TimeMetrics:
    """Parse do arquivo de saída do time -v"""
    metrics = TimeMetrics()
    
    if not file_path.exists():
        logger.warning(f"Arquivo time não encontrado: {file_path}")
        return metrics
    
    try:
        content = file_path.read_text(encoding='utf-8', errors='ignore')
        
        # Padrões para extrair métricas do time
        patterns = {
            'user_time_sec': r'User time \(seconds\):\s+([\d\.]+)',
            'system_time_sec': r'System time \(seconds\):\s+([\d\.]+)',
            'elapsed_time_sec': r'Elapsed \(wall clock\) time.*:\s+([\d:\.]+)',
            'cpu_percent': r'Percent of CPU this job got:\s+([\d\.]+)%',
            'max_resident_set_kb': r'Maximum resident set size \(kbytes\):\s+([\d,]+)',
            'average_resident_set_kb': r'Average resident set size \(kbytes\):\s+([\d,]+)',
            'major_page_faults': r'Major \(requiring I/O\) page faults:\s+([\d,]+)',
            'minor_page_faults': r'Minor \(reclaiming a frame\) page faults:\s+([\d,]+)',
            'voluntary_context_switches': r'Voluntary context switches:\s+([\d,]+)',
            'involuntary_context_switches': r'Involuntary context switches:\s+([\d,]+)',
            'swaps': r'Swaps:\s+([\d,]+)',
            'file_system_inputs': r'File system inputs:\s+([\d,]+)',
            'file_system_outputs': r'File system outputs:\s+([\d,]+)',
            'socket_messages_sent': r'Socket messages sent:\s+([\d,]+)',
            'socket_messages_received': r'Socket messages received:\s+([\d,]+)'
        }
        
        for metric, pattern in patterns.items():
            match = re.search(pattern, content, re.IGNORECASE)
            if match:
                value_str = match.group(1).replace(',', '')
                try:
                    if metric == 'elapsed_time_sec' and ':' in value_str:
                        # Converte formato mm:ss.ss para segundos
                        parts = value_str.split(':')
                        if len(parts) == 2:
                            minutes, seconds = float(parts[0]), float(parts[1])
                            setattr(metrics, metric, minutes * 60 + seconds)
                    elif 'sec' in metric or 'percent' in metric:
                        setattr(metrics, metric, float(value_str))
                    else:
                        setattr(metrics, metric, int(value_str))
                except ValueError:
                    continue
                    
    except Exception as e:
        logger.error(f"Erro ao parsear time output {file_path}: {e}")
    
    return metrics

def parse_strace_output(file_path: Path) -> StraceMetrics:
    metrics = StraceMetrics()

    if not file_path.exists():
        logger.warning(f"Arquivo strace não encontrado: {file_path}")
        return metrics

    try:
        content = file_path.read_text(encoding='utf-8', errors='ignore')

        # Regex compatível com linha total
        total_pattern = r'^\s*[\d\.]+\s+([\d\.]+)\s+[\d\.]+\s+(\d+)\s+(\d+)\s+total$'
        match = re.search(total_pattern, content, re.MULTILINE)
        if match:
            metrics.total_time_sec = float(match.group(1))
            metrics.total_calls = int(match.group(2))
            metrics.total_errors = int(match.group(3))

        # Contagem de chamadas por categoria usando nomes extraídos da tabela
        syscall_names = re.findall(
            r'^\s*[\d\.]+\s+[\d\.]+\s+[\d\.]+\s+\d+\s*(?:\d+)?\s+(\w+)', content, re.MULTILINE
        )

        memory_set = {'mmap', 'munmap', 'mprotect', 'brk', 'sbrk', 'madvise', 'mlock', 'munlock'}
        file_set = {'open', 'openat', 'close', 'read', 'write', 'writev', 'pread', 'pread64', 'lseek', 'stat', 'fstat', 'access', 'rename', 'unlink', 'getdents64'}
        process_set = {'fork', 'vfork', 'execve', 'clone', 'clone3', 'wait', 'waitpid', 'exit', 'kill', 'signal'}

        metrics.memory_calls = sum(1 for s in syscall_names if s in memory_set) or None
        metrics.file_calls = sum(1 for s in syscall_names if s in file_set) or None
        metrics.process_calls = sum(1 for s in syscall_names if s in process_set) or None

        if metrics.total_time_sec and metrics.total_calls:
            metrics.avg_call_time_us = (metrics.total_time_sec * 1e6) / metrics.total_calls

    except Exception as e:
        logger.error(f"Erro ao parsear strace output {file_path}: {e}")

    return metrics

def parse_profiler_results(output_dir: Path, wrapper: str, run_id: int, approach: str) -> Dict[str, Any]:
    """Parse dos resultados de todos os profilers para uma execução específica"""
    results = {}
    
    if wrapper == "perf":
        perf_file = output_dir / f"perf_stat_{approach}_{run_id}.txt"
        perf_metrics = parse_perf_output(perf_file)
        # Converte para dict com prefixo
        for field_name, value in perf_metrics.__dict__.items():
            if value is not None:
                results[f"perf_{field_name}"] = value
    
    elif wrapper == "time":
        time_file = output_dir / f"time_{approach}_{run_id}.out"
        time_metrics = parse_time_output(time_file)
        # Converte para dict com prefixo
        for field_name, value in time_metrics.__dict__.items():
            if value is not None:
                results[f"time_{field_name}"] = value
    
    elif wrapper == "strace":
        strace_file = output_dir / f"strace_{approach}_{run_id}.out"
        strace_metrics = parse_strace_output(strace_file)
        # Converte para dict com prefixo
        for field_name, value in strace_metrics.__dict__.items():
            if value is not None:
                results[f"strace_{field_name}"] = value
    
    return results