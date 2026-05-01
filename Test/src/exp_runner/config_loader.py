# exp_runner/config_loader.py
from pathlib import Path
from typing import List, Dict
import configparser
import json
import logging

logger = logging.getLogger(__name__)

def load_ini_config(file_path: Path) -> Dict[str, str]:
    """Loads configuration from an INI file"""
    config = configparser.ConfigParser()
    try:
        config.read(file_path)
        parsed = {k: v for section in config.sections() for k, v in config[section].items()}
        logger.debug(f"✅ Loaded INI: {file_path}")
        return parsed
    except Exception as e:
        logger.error(f"Error loading INI: {file_path} - {e}")
        return {}

def load_json_config(file_path: Path) -> Dict[str, str]:
    """Loads configuration from a JSON file"""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        logger.debug(f"✅ Loaded JSON: {file_path}")
        return data
    except Exception as e:
        logger.error(f"Error loading JSON: {file_path} - {e}")
        return {}

def load_configs_from_directory(config_dir: Path, extension=".ini") -> List[Path]:
    """Lists and sorts all configuration files in a directory"""
    files = sorted(config_dir.glob(f"*{extension}"))
    if not files:
        logger.warning(f"⚠️ No configuration files '{extension}' found in: {config_dir}")
    else:
        logger.info(f"📁 Found {len(files)} config files in {config_dir}")
    return files

def extract_config_metadata(file_path: Path) -> str:
    """Extracts the identifier name of the configuration (without extension)"""
    return file_path.stem
