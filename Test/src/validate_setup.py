"""
Script de Validação do Setup de Benchmarking

Verifica se todos os componentes necessários estão instalados e configurados
corretamente antes de executar a matriz de testes completa.
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

# Configurações (devem ser idênticas ao run_experiments_updated.py)
ALL_METHODS = [
    "RKGRAPH", "CONTEXTRR", "CORGRAPH", "CPRR", "RECKNNGRAPH", 
    "RLRECOM", "RLSIM", "LHRR", "BFSTREE", "RDPAC", "RFE"
]

EXECUTABLES = {
    "mmap": "./bin/udlf_mmap",
    "batch": "./bin/udlf_batch",
}

BASE_CONFIGS = [Path("config")]


def check_python_packages() -> bool:
    """Verifica se os pacotes Python necessários estão instalados."""
    logger.info("🐍 Verificando pacotes Python...")
    
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
            logger.error(f"  ✗ {package} - NÃO INSTALADO")
            missing.append(package)
    
    if missing:
        logger.error(f"\n❌ Pacotes faltando: {', '.join(missing)}")
        logger.info(f"💡 Instale com: pip install {' '.join(missing)}")
        return False
    
    logger.info("✅ Todos os pacotes Python estão instalados\n")
    return True


def check_system_tools() -> bool:
    """Verifica se as ferramentas de profiling do sistema estão disponíveis."""
    logger.info("🔧 Verificando ferramentas de sistema...")
    
    tools = {
        'perf': 'Profiling de performance (eventos de hardware)',
        'time': 'Medição de tempo e recursos',
    }
    
    available = []
    missing = []
    
    for tool, description in tools.items():
        try:
            if tool == 'time':
                # Verifica GNU time especificamente
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
            logger.warning(f"  ⚠ {tool} - NÃO DISPONÍVEL ({description})")
            missing.append(tool)
    
    if not available:
        logger.error("\n❌ Nenhuma ferramenta de profiling disponível!")
        return False
    
    if missing:
        logger.warning(f"\n⚠️  Ferramentas ausentes: {', '.join(missing)}")
        logger.info("💡 Instale perf: sudo apt-get install linux-tools-generic")
        logger.info("   O benchmarking continuará apenas com as ferramentas disponíveis\n")
    else:
        logger.info("✅ Todas as ferramentas de sistema estão disponíveis\n")
    
    return True


def check_executables() -> bool:
    """Verifica se os executáveis da aplicação existem e são executáveis."""
    logger.info("⚙️  Verificando executáveis da aplicação...")
    
    all_ok = True
    for name, path in EXECUTABLES.items():
        exe_path = Path(path)
        if exe_path.exists():
            if exe_path.is_file() and exe_path.stat().st_mode & 0o111:
                logger.info(f"  ✓ {name}: {path}")
            else:
                logger.error(f"  ✗ {name}: {path} existe mas NÃO É EXECUTÁVEL")
                logger.info(f"     Execute: chmod +x {path}")
                all_ok = False
        else:
            logger.error(f"  ✗ {name}: {path} - NÃO ENCONTRADO")
            all_ok = False
    
    if not all_ok:
        logger.error("\n❌ Executáveis ausentes ou sem permissão de execução")
        logger.info("💡 Compile os executáveis antes de prosseguir")
        return False
    
    logger.info("✅ Todos os executáveis estão disponíveis\n")
    return True


def check_config_files() -> Tuple[bool, List[Path]]:
    """Verifica se existem arquivos de configuração base."""
    logger.info("📁 Verificando arquivos de configuração...")
    
    all_configs = []
    for config_dir in BASE_CONFIGS:
        if not config_dir.exists():
            logger.error(f"  ✗ Diretório não encontrado: {config_dir}")
            continue
        
        configs = list(config_dir.glob("*.ini"))
        if not configs:
            logger.warning(f"  ⚠ Nenhum arquivo .ini encontrado em {config_dir}")
        else:
            logger.info(f"  ✓ {len(configs)} arquivo(s) .ini encontrado(s) em {config_dir}")
            for config in configs:
                logger.info(f"    - {config.name}")
                all_configs.append(config)
    
    if not all_configs:
        logger.error("\n❌ Nenhum arquivo de configuração encontrado!")
        logger.info("💡 Crie arquivos .ini em config/ antes de prosseguir")
        return False, []
    
    logger.info(f"✅ Total de {len(all_configs)} configuração(ões) base encontrada(s)\n")
    return True, all_configs


def estimate_execution_time(num_configs: int) -> None:
    """Estima o tempo total de execução."""
    logger.info("⏱️  Estimando tempo de execução...")
    
    num_methods = len(ALL_METHODS)
    num_approaches = len(EXECUTABLES)
    num_wrappers = 2  # perf e time
    repetitions_per_wrapper = 5
    
    total_runs = num_configs * num_methods * num_approaches * num_wrappers * repetitions_per_wrapper
    
    # Estimativa conservadora: 2 minutos por execução (depende do dataset)
    avg_time_per_run_minutes = 2
    total_time_minutes = total_runs * avg_time_per_run_minutes
    total_time_hours = total_time_minutes / 60
    
    logger.info(f"  📊 Configurações base: {num_configs}")
    logger.info(f"  🧮 Métodos: {num_methods}")
    logger.info(f"  ⚙️  Abordagens: {num_approaches}")
    logger.info(f"  🔧 Wrappers: {num_wrappers}")
    logger.info(f"  🔄 Repetições por wrapper: {repetitions_per_wrapper}")
    logger.info(f"  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
    logger.info(f"  🔢 Total de execuções: {total_runs}")
    logger.info(f"  ⏱️  Tempo estimado: ~{total_time_hours:.1f} horas")
    logger.info(f"     (assumindo ~{avg_time_per_run_minutes} min/execução)\n")
    
    if total_time_hours > 24:
        logger.warning("⚠️  O benchmarking completo levará mais de 24 horas!")
        logger.info("💡 Considere:")
        logger.info("   - Reduzir o número de repetições em WRAPPERS")
        logger.info("   - Executar apenas um subset dos métodos inicialmente")
        logger.info("   - Usar datasets menores para testes preliminares\n")


def check_disk_space() -> bool:
    """Verifica espaço em disco disponível."""
    logger.info("💾 Verificando espaço em disco...")
    
    try:
        import shutil
        stat = shutil.disk_usage('.')
        
        free_gb = stat.free / (1024**3)
        total_gb = stat.total / (1024**3)
        used_percent = (stat.used / stat.total) * 100
        
        logger.info(f"  Total: {total_gb:.1f} GB")
        logger.info(f"  Usado: {used_percent:.1f}%")
        logger.info(f"  Livre: {free_gb:.1f} GB")
        
        # Recomendação: pelo menos 10 GB livres
        if free_gb < 10:
            logger.warning("\n⚠️  Menos de 10 GB de espaço livre!")
            logger.info("💡 Os resultados podem ocupar vários GB dependendo do número de execuções")
            return False
        
        logger.info("✅ Espaço em disco suficiente\n")
        return True
        
    except Exception as e:
        logger.warning(f"⚠️  Não foi possível verificar espaço em disco: {e}\n")
        return True


def main():
    """Executa todas as verificações."""
    logger.info("="*80)
    logger.info("🔍 VALIDAÇÃO DO SETUP DE BENCHMARKING")
    logger.info("="*80)
    logger.info("")
    
    checks = []
    
    # Executa todas as verificações
    checks.append(("Pacotes Python", check_python_packages()))
    checks.append(("Ferramentas de Sistema", check_system_tools()))
    checks.append(("Executáveis", check_executables()))
    
    config_ok, config_files = check_config_files()
    checks.append(("Arquivos de Configuração", config_ok))
    
    checks.append(("Espaço em Disco", check_disk_space()))
    
    if config_ok:
        estimate_execution_time(len(config_files))
    
    # Resumo final
    logger.info("="*80)
    logger.info("📋 RESUMO DA VALIDAÇÃO")
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
        logger.info("✅ SETUP VALIDADO COM SUCESSO!")
        logger.info("="*80)
        logger.info("Você pode prosseguir com:")
        logger.info("  1. python src/run_experiments_updated.py")
        logger.info("  2. python src/generate_plots.py")
        logger.info("")
        return 0
    else:
        logger.error("="*80)
        logger.error("❌ VALIDAÇÃO FALHOU")
        logger.error("="*80)
        logger.error("Corrija os problemas acima antes de prosseguir.")
        logger.info("Consulte BENCHMARKING_README.md para mais detalhes.")
        logger.info("")
        return 1


if __name__ == "__main__":
    sys.exit(main())
