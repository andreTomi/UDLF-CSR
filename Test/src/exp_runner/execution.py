import os
import subprocess
import time
import signal
import shutil
import threading
import resource
import tempfile
from pathlib import Path
from typing import List
import logging

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

from .core import RunResult, DetailedMetrics, SystemMonitor

logger = logging.getLogger(__name__)

# Global tracking for cleanup
_active_pgid = None

def kill_active_process():
    global _active_pgid
    if _active_pgid:
        try:
            os.killpg(_active_pgid, signal.SIGKILL)
            _active_pgid = None
        except: pass

class AdvancedProfiler:
    """
    (O código da classe AdvancedProfiler permanece o mesmo, sem alterações)
    """
    def __init__(self, sampling_interval: float = 0.01):
        self.sampling_interval = sampling_interval
        self.stop_event = threading.Event()
        self.metrics = DetailedMetrics()
        self.temp_dir = Path(tempfile.mkdtemp(prefix="profiler_"))

    def __del__(self):
        if self.temp_dir.exists():
            shutil.rmtree(self.temp_dir, ignore_errors=True)

    def monitor_process(self, proc):
        if not HAS_PSUTIL:
            logger.debug("Monitoramento psutil desativado (módulo não encontrado)")
            return
            
        temp_samples = []
        try:
            while not self.stop_event.is_set() and proc.is_running():
                try:
                    mem = proc.memory_info()
                    full = proc.memory_full_info()
                    self.metrics.rss_samples.append(mem.rss / 1024**2)
                    self.metrics.vms_samples.append(mem.vms / 1024**2)
                    try:
                        self.metrics.pss_mb = full.pss / 1024**2
                        self.metrics.uss_mb = full.uss / 1024**2
                        self.metrics.shared_mb = full.shared / 1024**2
                    except AttributeError:
                        pass
                    self.metrics.cpu_samples.append(proc.cpu_percent())
                    try:
                        io = proc.io_counters()
                        self.metrics.read_ops = io.read_count
                        self.metrics.write_ops = io.write_count
                    except (psutil.NoSuchProcess, psutil.AccessDenied):
                        logger.debug(f"Acesso negado a io_counters para pid={proc.pid}")
                    try:
                        ctx = proc.num_ctx_switches()
                        self.metrics.context_switches_vol = ctx.voluntary
                        self.metrics.context_switches_invol = ctx.involuntary
                    except (psutil.NoSuchProcess, psutil.AccessDenied):
                        logger.debug(f"Acesso negado a context_switches para pid={proc.pid}")

                except (psutil.NoSuchProcess, psutil.ZombieProcess):
                    break
                time.sleep(self.sampling_interval)
        except Exception as e:
            logger.warning(f"Erro no monitoramento: {e}")
        if temp_samples:
            self.metrics.cpu_temp_avg = sum(temp_samples) / len(temp_samples)


def run_with_advanced_monitoring(cmd: List[str], timeout: int, log_path: Path) -> RunResult:
    """
    Executa um comando com monitoramento avançado, ignorando stdout e stderr.
    """
    global _active_pgid
    profiler = AdvancedProfiler()
    system_info = SystemMonitor.get_system_info()
    
    # Nota: A pasta de log ainda pode ser necessária para os wrappers (perf, time, etc.)
    # então mantemos a criação do diretório.
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
    except Exception as e:
        logger.error(f"❌ Erro ao criar diretório de log {log_path.parent}: {e}")
        return RunResult(config="", approach="", wrapper="", run=0, elapsed_time=0.0,
                         exit_code=-1, status="error", error_message=f"Log dir creation failed: {e}")

    try:
        # Inicia o processo redirecionando stdout e stderr para o log_path
        with open(log_path, 'w') as log_file:
            if HAS_PSUTIL:
                proc = psutil.Popen(
                    cmd,
                    stdout=log_file,
                    stderr=log_file,
                    preexec_fn=os.setsid
                )
            else:
                proc = subprocess.Popen(
                    cmd,
                    stdout=log_file,
                    stderr=log_file,
                    preexec_fn=os.setsid
                )
            
            # Track PGID for cleanup
            try:
                _active_pgid = os.getpgid(proc.pid)
            except: pass

        monitor_thread = threading.Thread(target=profiler.monitor_process, args=(proc,))
        start_time = time.time()
        start_load = os.getloadavg()[0] if hasattr(os, 'getloadavg') else 0.0
        start_memory = psutil.virtual_memory().percent if HAS_PSUTIL else 0.0

        monitor_started = False
        if HAS_PSUTIL:
            try:
                monitor_thread.start()
                monitor_started = True
            except Exception as e:
                logger.debug(f"Falha ao iniciar monitor_thread: {e}")

        try:
            exit_code = proc.wait(timeout=timeout)
            status = "success" if exit_code == 0 else f"exit_code_{exit_code}"
            error_message = None
        except subprocess.TimeoutExpired:
            # killing process logic...
            try:
                pgid = os.getpgid(proc.pid)
                os.killpg(pgid, signal.SIGTERM)
                time.sleep(1)
                os.killpg(pgid, signal.SIGKILL)
            except:
                pass
            
            status = "timeout"
            exit_code = -1
            error_message = f"Timeout de {timeout}s atingido."
            logger.warning(f"⏳ Processo {proc.pid} (cmd: {' '.join(cmd)}) estourou o tempo e foi encerrado.")
        finally:
            profiler.stop_event.set()
            elapsed = time.time() - start_time
            if monitor_started:
                monitor_thread.join(timeout=2.0)
            
            # Reset tracking
            _active_pgid = None

        # Coleta de métricas via 'resource'
        try:
            rusage = resource.getrusage(resource.RUSAGE_CHILDREN)
            profiler.metrics.user_time = rusage.ru_utime
            profiler.metrics.system_time = rusage.ru_stime
            profiler.metrics.page_faults_minor = rusage.ru_minflt
            profiler.metrics.page_faults_major = rusage.ru_majflt
        except Exception as e:
            logger.debug(f"Não foi possível obter rusage: {e}")

        # Monta o objeto de resultado final
        result = RunResult(
            config="", approach="", wrapper="", run=0,
            elapsed_time=elapsed,
            exit_code=exit_code,
            status=status,
            error_message=error_message,
            max_rss_mb=max(profiler.metrics.rss_samples or [0]),
            avg_rss_mb=sum(profiler.metrics.rss_samples) / len(profiler.metrics.rss_samples or [1]),
            peak_vms_mb=max(profiler.metrics.vms_samples or [0]),
            cpu_percent_avg=sum(profiler.metrics.cpu_samples) / len(profiler.metrics.cpu_samples or [1]),
            read_ops=profiler.metrics.read_ops,
            write_ops=profiler.metrics.write_ops,
            page_faults_minor=profiler.metrics.page_faults_minor,
            page_faults_major=profiler.metrics.page_faults_major,
            system_load_initial=start_load,
            memory_pressure_initial=start_memory,
        )
        return result

    except Exception as e:
        logger.error(f"❌ Erro fatal durante a execução do comando '{' '.join(cmd)}': {e}", exc_info=True)
        return RunResult(config="", approach="", wrapper="", run=0,
                         elapsed_time=0.0, exit_code=-1, status="error", error_message=str(e))