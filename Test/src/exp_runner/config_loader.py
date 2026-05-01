# exp_runner/config_loader.py
from pathlib import Path
from typing import List, Dict
import configparser
import json
import logging

logger = logging.getLogger(__name__)

def load_ini_config(file_path: Path) -> Dict[str, str]:
    """Carrega configuração a partir de um arquivo INI"""
    config = configparser.ConfigParser()
    try:
        config.read(file_path)
        parsed = {k: v for section in config.sections() for k, v in config[section].items()}
        logger.debug(f"✅ Carregado INI: {file_path}")
        return parsed
    except Exception as e:
        logger.error(f"Erro ao carregar INI: {file_path} - {e}")
        return {}

def load_json_config(file_path: Path) -> Dict[str, str]:
    """Carrega configuração a partir de um arquivo JSON"""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        logger.debug(f"✅ Carregado JSON: {file_path}")
        return data
    except Exception as e:
        logger.error(f"Erro ao carregar JSON: {file_path} - {e}")
        return {}

def load_configs_from_directory(config_dir: Path, extension=".ini") -> List[Path]:
    """Lista e ordena todos os arquivos de configuração de um diretório"""
    files = sorted(config_dir.glob(f"*{extension}"))
    if not files:
        logger.warning(f"⚠️ Nenhum arquivo de configuração '{extension}' encontrado em: {config_dir}")
    else:
        logger.info(f"📁 {len(files)} arquivos carregados de {config_dir}")
    return files

def extract_config_metadata(file_path: Path) -> str:
    """Extrai o nome identificador da configuração (sem extensão)"""
    return file_path.stem
