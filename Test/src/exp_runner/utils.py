# exp_runner/utils.py
import hashlib
import os
import logging
from pathlib import Path

logger = logging.getLogger(__name__)

def ensure_directory_exists(path: Path) -> Path:
    """Cria diretório se não existir"""
    try:
        path.mkdir(parents=True, exist_ok=True)
        return path
    except Exception as e:
        logger.error(f"Erro ao criar diretório: {path} - {e}")
        raise

def hash_file_sha256(file_path: Path, block_size: int = 65536) -> str:
    """Gera SHA-256 de um arquivo (útil para garantir reprodutibilidade)"""
    sha256 = hashlib.sha256()
    try:
        with open(file_path, "rb") as f:
            while chunk := f.read(block_size):
                sha256.update(chunk)
        return sha256.hexdigest()
    except Exception as e:
        logger.error(f"Erro ao calcular hash de {file_path}: {e}")
        return "ERROR"

def format_log_separator(title: str = "") -> str:
    """Retorna um separador formatado com título"""
    separator = "=" * 80
    if title:
        return f"{separator}\n{title.upper()}\n{separator}\n"
    return f"{separator}\n"

def sanitize_filename(name: str) -> str:
    """Remove caracteres problemáticos para nomes de arquivos"""
    return "".join(c if c.isalnum() or c in "_-." else "_" for c in name)

def format_experiment_name(config_name: str, approach: str) -> str:
    """Cria um nome único padronizado para uma combinação de experimento"""
    return sanitize_filename(f"{config_name}_{approach}")

def get_cpu_thread_count(logical: bool = True) -> int:
    """Retorna o número de threads do sistema"""
    try:
        import psutil
        return psutil.cpu_count(logical=logical)
    except ImportError:
        return os.cpu_count()
