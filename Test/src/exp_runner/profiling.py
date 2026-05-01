# exp_runner/profiling.py
import subprocess
import os
from pathlib import Path
from typing import Optional, List, Dict
import logging

logger = logging.getLogger(__name__)

TOOL_CACHE: Dict[str, Optional[str]] = {}

def find_tool_path_cached(tool_name: str) -> Optional[str]:
    if tool_name in TOOL_CACHE:
        return TOOL_CACHE[tool_name]
    path = find_tool_path(tool_name)
    TOOL_CACHE[tool_name] = path
    return path
def find_tool_path(tool_name: str) -> Optional[str]:
    # 1. Try 'which'
    try:
        result = subprocess.run(['which', tool_name], capture_output=True, text=True, timeout=5)
        if result.returncode == 0 and result.stdout.strip():
            path = result.stdout.strip()
            if os.path.isfile(path) and os.access(path, os.X_OK):
                if tool_name == 'perf':
                    try:
                        perf_check = subprocess.run([path, 'stat', '--', 'true'], capture_output=True, timeout=5)
                        if perf_check.returncode != 0:
                            logger.warning(f"perf encontrado em {path} mas falhou no teste de permissão (paranoid?).")
                            return None
                    except: return None
                return path
    except: pass

    # 2. Try 'whereis'
    try:
        result = subprocess.run(['whereis', tool_name], capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            parts = result.stdout.strip().split()
            if len(parts) > 1:
                for path in parts[1:]:
                    if os.path.isfile(path) and os.access(path, os.X_OK):
                        if tool_name == 'perf':
                            try:
                                perf_check = subprocess.run([path, 'stat', '--', 'true'], capture_output=True, timeout=5)
                                if perf_check.returncode == 0: return path
                            except: continue
                        else:
                            return path
    except: pass

    # 3. Special case for /usr/bin/time
    if tool_name == 'time':
        for p in ['/usr/bin/time', '/bin/time']:
            if os.path.isfile(p) and os.access(p, os.X_OK):
                return p

    # 4. Try direct execution (last resort)
    try:
        subprocess.run([tool_name, '--version'], capture_output=True, timeout=5)
        return tool_name
    except: pass

    return None

def build_wrapper_command(wrapper: str, exe_path: str, config_path: str, output_dir: Path, run_id: Optional[int] = None, approach: Optional[str] = None) -> List[str]:
    exe_path = str(Path(exe_path).resolve())
    config_path = str(Path(config_path).resolve())

    try:
        output_dir.mkdir(parents=True, exist_ok=True)
    except Exception as e:
        logger.error(f"Erro ao criar diretório do wrapper: {e}")
        return [exe_path, config_path]

    perf = find_tool_path_cached('perf')
    strace = find_tool_path_cached('strace')
    time_ = find_tool_path_cached('time') or '/usr/bin/time'

    # Agora inclui approach para evitar sobrescrita entre abordagens
    def make_filename(tool: str, extension: str) -> str:
        parts = [tool]
        if approach:
            parts.append(approach)
        if run_id:
            parts.append(str(run_id))
        return f"{'_'.join(parts)}.{extension}"

    wrappers = {
        "none": [exe_path, config_path]
    }

    if perf:
        output_file = output_dir / make_filename("perf_stat", "txt")
        # Remove o arquivo se existir para evitar append de execuções anteriores
        if output_file.exists():
            output_file.unlink()
        
        wrappers["perf"] = [
            perf, "stat",
            "-e", "cache-misses,cache-references,cycles,instructions",
            "-e", "page-faults,context-switches,cpu-migrations", 
            "-e", "branch-misses,branches",
            "-e", "L1-dcache-loads,L1-dcache-load-misses",
            "-e", "LLC-loads,LLC-load-misses",
            "-e", "task-clock,cpu-clock",
            "-o", str(output_file),
            # REMOVIDO: "--append" - esta era a causa do problema!
            exe_path, config_path
        ]
    
    if strace:
        output_file = output_dir / make_filename("strace", "out")
        # Remove o arquivo se existir para evitar append de execuções anteriores
        if output_file.exists():
            output_file.unlink()
            
        wrappers["strace"] = [
            strace, "-c", "-f", "-e", "trace=memory,file,process",
            "-o", str(output_file),
            exe_path, config_path
        ]
    
    if time_:
        output_file = output_dir / make_filename("time", "out")
        # Remove o arquivo se existir para evitar append de execuções anteriores
        if output_file.exists():
            output_file.unlink()
            
        wrappers["time"] = [
            time_, "-v",
            "-o", str(output_file),
            exe_path, config_path
        ]
    
    if wrapper not in wrappers:
        logger.warning(f"Wrapper '{wrapper}' não encontrado. Usando execução direta.")
        return wrappers["none"]

    return wrappers[wrapper]
    

def get_available_wrappers() -> List[str]:
    available = []
    if find_tool_path_cached("perf"): available.append("perf")
    if find_tool_path_cached("strace"): available.append("strace")
    if find_tool_path_cached("time") or Path("/usr/bin/time").exists(): available.append("time")
    return available