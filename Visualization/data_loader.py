import pandas as pd
import glob
from pathlib import Path
import logging

logger = logging.getLogger(__name__)

def load_summary_results(base_path: Path):
    """
    Search recursively for .csv files in the directory and load them.
    Specifically targets Evaluation_Aggregated_Summary.csv and Unified_Accuracy_Results.csv
    """
    summary_path = base_path / "Evaluation_Aggregated_Summary.csv"
    accuracy_path = base_path / "Unified_Accuracy_Results.csv"
    
    df_summary = None
    df_accuracy = None
    
    if summary_path.exists():
        df_summary = pd.read_csv(summary_path)
    else:
        logger.warning(f"{summary_path} not found.")
        
    if accuracy_path.exists():
        df_accuracy = pd.read_csv(accuracy_path)
    else:
        logger.warning(f"{accuracy_path} not found.")
        
    return df_summary, df_accuracy

def preprocess_summary(df):
    """
    Cleans and prepares the summary dataframe.
    """
    if df is None:
        return None
        
    # Simplify dataset names from config
    def simplify_dataset(config):
        if 'aloi' in config.lower(): return 'ALOI (72K)'
        if 'sun397' in config.lower(): return 'SUN397 (108K)'
        if 'vegfru' in config.lower(): return 'VegFru (160K)'
        return config

    df['Dataset'] = df['config'].apply(simplify_dataset)
    
    # Map Hardware_Config to readable names
    hw_map = {
        '192gb_RAM_full_memory_intel_xeon_gold_18_Threads_Singlecore': 'Env A: 192GB Server',
        '16gb_RAM_reduced_memory_intel_xeon_gold_12_Threads_singlecore': 'Env B: 16GB Limited',
        '8gb_Commodity_hardware_intel_i5_12_threads': 'Env C: 8GB Consumer'
    }
    df['Environment'] = df['Hardware_Config'].map(hw_map)
    
    # Standardize Approach
    df['Method'] = df['Approach_Type'].replace({'CSR': 'Sparse', 'Dense': 'Dense'})
    
    # Extract algorithm name (CPRR or LHRR) from config
    def extract_algorithm(config):
        if 'LHRR' in config.upper(): return 'LHRR'
        if 'CPRR' in config.upper(): return 'CPRR'
        return 'Unknown'
    df['Algorithm'] = df['config'].apply(extract_algorithm)
    
    return df

def preprocess_accuracy(df):
    """
    Cleans and prepares the accuracy dataframe.
    """
    if df is None:
        return None
        
    # Simplify dataset names
    df['Dataset'] = df['Dataset'].replace({
        'aloi': 'ALOI (72K)',
        'sun397': 'SUN397 (108K)',
        'vegfru': 'VegFru (160K)'
    })
    
    return df

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    # Test loading
    base = Path(__file__).resolve().parent.parent / "Data"
    s, a = load_summary_results(base)
    if s is not None:
        s = preprocess_summary(s)
        logger.info(f"Summary loaded. Columns: {list(s.columns)}")
    if a is not None:
        a = preprocess_accuracy(a)
        logger.info(f"Accuracy loaded. Rows: {len(a)}")
