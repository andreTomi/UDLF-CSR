"""
Config Generation Test Script

Tests the dynamic configuration generation functionality without executing benchmarks.
Useful for validating that configs are being created correctly.
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
    """Creates a temporary config file modifying only the UDL_METHOD field."""
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
    """Validates that the config was created correctly."""
    config = configparser.ConfigParser()
    config.optionxform = str
    config.read(config_path)
    
    actual_method = config['DEFAULT'].get('UDL_METHOD', '')
    
    if actual_method != expected_method:
        logger.error(f"❌ Incorrect method: expected '{expected_method}', got '{actual_method}'")
        return False
    
    return True


def test_config_generation():
    """Tests config generation for all methods."""
    logger.info("="*80)
    logger.info("🔬 CONFIG GENERATION TEST")
    logger.info("="*80)
    logger.info("")
    
    # Search for base configs
    config_dir = Path("config")
    if not config_dir.exists():
        logger.error(f"❌ Config directory not found: {config_dir}")
        return False
    
    config_files = list(config_dir.glob("*.ini"))
    if not config_files:
        logger.error(f"❌ No .ini file found in {config_dir}")
        return False
    
    logger.info(f"📁 Base configs found: {len(config_files)}")
    for cf in config_files:
        logger.info(f"   - {cf.name}")
    logger.info("")
    
    # Test with the first config
    base_config = config_files[0]
    logger.info(f"🧪 Testing with: {base_config.name}")
    logger.info("")
    
    all_passed = True
    
    with tempfile.TemporaryDirectory(prefix="config_test_") as temp_dir_str:
        temp_dir = Path(temp_dir_str)
        logger.info(f"📦 Temporary directory: {temp_dir}")
        logger.info("")
        
        for method in ALL_METHODS:
            logger.info(f"🔧 Generating config for: {method}")
            
            try:
                # Generate temporary config
                temp_config = create_temp_config_with_method(base_config, method, temp_dir)
                
                # Validate
                if validate_config(temp_config, method):
                    logger.info(f"   ✅ {temp_config.name} - OK")
                    
                    # Show first lines of the config
                    with open(temp_config, 'r') as f:
                        lines = f.readlines()[:5]
                    logger.info(f"   First lines:")
                    for line in lines:
                        logger.info(f"      {line.rstrip()}")
                else:
                    logger.error(f"   ❌ {temp_config.name} - FAILED")
                    all_passed = False
                
                logger.info("")
                
            except Exception as e:
                logger.error(f"   ❌ Error generating config: {e}")
                all_passed = False
                logger.info("")
    
    # Summary
    logger.info("="*80)
    if all_passed:
        logger.info("✅ ALL TESTS PASSED")
        logger.info(f"   {len(ALL_METHODS)} configs generated successfully")
    else:
        logger.error("❌ SOME TESTS FAILED")
    logger.info("="*80)
    
    return all_passed


def test_config_parsing():
    """Tests reverse parsing of the config name."""
    logger.info("\n" + "="*80)
    logger.info("🔬 NAME PARSING TEST")
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
        logger.info(f"🧪 Testing: {config_name}")
        
        match = re.match(r'(.+?)__(.+)$', config_name)
        if match:
            dataset = match.group(1)
            method = match.group(2)
            
            if dataset == expected_dataset and method == expected_method:
                logger.info(f"   ✅ Dataset: {dataset}, Method: {method}")
            else:
                logger.error(f"   ❌ Expected: {expected_dataset}/{expected_method}")
                logger.error(f"      Got: {dataset}/{method}")
                all_passed = False
        else:
            logger.error(f"   ❌ Regex did not match")
            all_passed = False
        
        logger.info("")
    
    logger.info("="*80)
    if all_passed:
        logger.info("✅ ALL PARSING TESTS PASSED")
    else:
        logger.error("❌ SOME PARSING TESTS FAILED")
    logger.info("="*80)
    
    return all_passed


def main():
    """Runs all tests."""
    logger.info("\n\n")
    logger.info("╔" + "="*78 + "╗")
    logger.info("║" + " "*25 + "CONFIGURATION TESTS" + " "*32 + "║")
    logger.info("╚" + "="*78 + "╝")
    logger.info("")
    
    test1_passed = test_config_generation()
    test2_passed = test_config_parsing()
    
    logger.info("\n\n")
    logger.info("="*80)
    logger.info("📊 OVERALL SUMMARY")
    logger.info("="*80)
    logger.info(f"Config Generation Test: {'✅ PASS' if test1_passed else '❌ FAIL'}")
    logger.info(f"Name Parsing Test:      {'✅ PASS' if test2_passed else '❌ FAIL'}")
    logger.info("")
    
    if test1_passed and test2_passed:
        logger.info("✅ ALL TESTS PASSED - System is ready for use!")
        return 0
    else:
        logger.error("❌ SOME TESTS FAILED - Check the errors above")
        return 1


if __name__ == "__main__":
    import sys
    sys.exit(main())
