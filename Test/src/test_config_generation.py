"""
Script de Teste para Geração de Configurações

Testa a funcionalidade de geração dinâmica de configs sem executar os benchmarks.
Útil para validar que os configs estão sendo criados corretamente.
"""

import logging
import tempfile
import configparser
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(levelname)s: %(message)s"
)
logger = logging.getLogger(__name__)

ALL_METHODS = [
    "RKGRAPH", "CONTEXTRR", "CORGRAPH", "CPRR", "RECKNNGRAPH", 
    "RLRECOM", "RLSIM", "LHRR", "BFSTREE", "RDPAC", "RFE"
]


def create_temp_config_with_method(base_config_path: Path, method_name: str, temp_dir: Path) -> Path:
    """Cria um arquivo de configuração temporário modificando apenas o campo UDL_METHOD."""
    config = configparser.ConfigParser()
    config.optionxform = str
    config.read(base_config_path)
    
    config['DEFAULT']['UDL_METHOD'] = method_name
    
    base_name = base_config_path.stem
    temp_config_path = temp_dir / f"{base_name}__{method_name}.ini"
    
    with open(temp_config_path, 'w') as f:
        config.write(f)
    
    return temp_config_path


def validate_config(config_path: Path, expected_method: str) -> bool:
    """Valida que o config foi criado corretamente."""
    config = configparser.ConfigParser()
    config.optionxform = str
    config.read(config_path)
    
    actual_method = config['DEFAULT'].get('UDL_METHOD', '')
    
    if actual_method != expected_method:
        logger.error(f"❌ Método incorreto: esperado '{expected_method}', obtido '{actual_method}'")
        return False
    
    return True


def test_config_generation():
    """Testa a geração de configs para todos os métodos."""
    logger.info("="*80)
    logger.info("🔬 TESTE DE GERAÇÃO DE CONFIGURAÇÕES")
    logger.info("="*80)
    logger.info("")
    
    # Procura configs base
    config_dir = Path("config")
    if not config_dir.exists():
        logger.error(f"❌ Diretório de config não encontrado: {config_dir}")
        return False
    
    config_files = list(config_dir.glob("*.ini"))
    if not config_files:
        logger.error(f"❌ Nenhum arquivo .ini encontrado em {config_dir}")
        return False
    
    logger.info(f"📁 Configs base encontrados: {len(config_files)}")
    for cf in config_files:
        logger.info(f"   - {cf.name}")
    logger.info("")
    
    # Testa com o primeiro config
    base_config = config_files[0]
    logger.info(f"🧪 Testando com: {base_config.name}")
    logger.info("")
    
    all_passed = True
    
    with tempfile.TemporaryDirectory(prefix="config_test_") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        logger.info(f"📦 Diretório temporário: {temp_dir}")
        logger.info("")
        
        for method in ALL_METHODS:
            logger.info(f"🔧 Gerando config para: {method}")
            
            try:
                # Gera config temporário
                temp_config = create_temp_config_with_method(base_config, method, temp_dir)
                
                # Valida
                if validate_config(temp_config, method):
                    logger.info(f"   ✅ {temp_config.name} - OK")
                    
                    # Mostra primeiras linhas do config
                    with open(temp_config, 'r') as f:
                        lines = f.readlines()[:5]
                    logger.info(f"   Primeiras linhas:")
                    for line in lines:
                        logger.info(f"      {line.rstrip()}")
                else:
                    logger.error(f"   ❌ {temp_config.name} - FALHOU")
                    all_passed = False
                
                logger.info("")
                
            except Exception as e:
                logger.error(f"   ❌ Erro ao gerar config: {e}")
                all_passed = False
                logger.info("")
    
    # Resumo
    logger.info("="*80)
    if all_passed:
        logger.info("✅ TODOS OS TESTES PASSARAM")
        logger.info(f"   {len(ALL_METHODS)} configs gerados com sucesso")
    else:
        logger.error("❌ ALGUNS TESTES FALHARAM")
    logger.info("="*80)
    
    return all_passed


def test_config_parsing():
    """Testa o parsing reverso do nome do config."""
    logger.info("\n" + "="*80)
    logger.info("🔬 TESTE DE PARSING DE NOMES")
    logger.info("="*80)
    logger.info("")
    
    import re
    
    test_cases = [
        ("config_cifar_10k__RKGRAPH", "config_cifar_10k", "RKGRAPH"),
        ("config_dataset_small__CPRR", "config_dataset_small", "CPRR"),
        ("my_config__RLRECOM", "my_config", "RLRECOM"),
    ]
    
    all_passed = True
    
    for config_name, expected_dataset, expected_method in test_cases:
        logger.info(f"🧪 Testando: {config_name}")
        
        match = re.match(r'(.+?)__(.+)$', config_name)
        if match:
            dataset = match.group(1)
            method = match.group(2)
            
            if dataset == expected_dataset and method == expected_method:
                logger.info(f"   ✅ Dataset: {dataset}, Método: {method}")
            else:
                logger.error(f"   ❌ Esperado: {expected_dataset}/{expected_method}")
                logger.error(f"      Obtido: {dataset}/{method}")
                all_passed = False
        else:
            logger.error(f"   ❌ Regex não bateu")
            all_passed = False
        
        logger.info("")
    
    logger.info("="*80)
    if all_passed:
        logger.info("✅ TODOS OS TESTES DE PARSING PASSARAM")
    else:
        logger.error("❌ ALGUNS TESTES DE PARSING FALHARAM")
    logger.info("="*80)
    
    return all_passed


def main():
    """Executa todos os testes."""
    logger.info("\n\n")
    logger.info("╔" + "="*78 + "╗")
    logger.info("║" + " "*25 + "TESTES DE CONFIGURAÇÃO" + " "*30 + "║")
    logger.info("╚" + "="*78 + "╝")
    logger.info("")
    
    test1_passed = test_config_generation()
    test2_passed = test_config_parsing()
    
    logger.info("\n\n")
    logger.info("="*80)
    logger.info("📊 RESUMO GERAL")
    logger.info("="*80)
    logger.info(f"Teste de Geração de Configs: {'✅ PASS' if test1_passed else '❌ FAIL'}")
    logger.info(f"Teste de Parsing de Nomes:   {'✅ PASS' if test2_passed else '❌ FAIL'}")
    logger.info("")
    
    if test1_passed and test2_passed:
        logger.info("✅ TODOS OS TESTES PASSARAM - Sistema pronto para uso!")
        return 0
    else:
        logger.error("❌ ALGUNS TESTES FALHARAM - Verifique os erros acima")
        return 1


if __name__ == "__main__":
    import sys
    sys.exit(main())
