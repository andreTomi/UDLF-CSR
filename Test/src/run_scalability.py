import sys
import subprocess
import os
import argparse
from pathlib import Path

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False

import signal

# Track current child process
current_child_pgid = None

def cleanup_handler(signum, frame):
    global current_child_pgid
    if current_child_pgid:
        try:
            print(f"\n🛑 Cleanup: Killing child process group {current_child_pgid}...")
            os.killpg(current_child_pgid, signal.SIGKILL)
        except: pass
    sys.exit(1)

# Register signals
signal.signal(signal.SIGINT, cleanup_handler)
signal.signal(signal.SIGTERM, cleanup_handler)

def main():
    global current_child_pgid
    parser = argparse.ArgumentParser(description="Wrapper for scalability experiments")
    parser.add_argument("--script", type=str, default="src/run_experiments_updated.py", help="Script to run")
    parser.add_argument("--timeout-file", type=str, default="scale_timeouts.json", help="File to store timeouts")
    parser.add_argument("--threads", type=int, nargs='+', default=[12, 8, 4, 1], help="List of thread counts to test descending order is recommended")
    args = parser.parse_args()

    timeout_file_path = Path(args.timeout_file).resolve()
    script_path = Path(args.script).resolve()

    for t in args.threads:
        print(f"\n{'='*80}")
        print(f"🚀 RUNNING SCALABILITY EXPERIMENT FOR {t} THREAD(S)")
        print(f"{'='*80}\n")
        
        # CPU binding from 0 to t-1
        cpus = f"0-{t-1}" if t > 1 else "0"
        
        # Output directory for this thread count
        output_dir = f"benchmark_outputs_t{t}"
        
        # Prepare environment
        current_env = os.environ.copy()
        current_env["OMP_NUM_THREADS"] = str(t)
        current_env["OMP_PROC_BIND"] = "true"
        current_env["OMP_PLACES"] = "cores"
        
        cmd = [
            "numactl", f"--physcpubind={cpus}", "--membind=0",
            "python", str(script_path),
            "--output-dir", output_dir,
            "--timeout-file", str(timeout_file_path)
        ]
        
        # Define log_path for this run
        log_path = Path(f"scalability_log_t{t}.log").resolve()

        print(f"Executing: {' '.join(cmd)}")
        print(f"Output and errors will be logged to: {log_path}")
        
        try:
            # Inicia o processo redirecionando stdout e stderr para o log_path
            with open(log_path, 'w') as log_file:
                if HAS_PSUTIL:
                    # Use psutil.Popen for better process management if available
                    proc = psutil.Popen(
                        cmd,
                        stdout=log_file,
                        stderr=log_file,
                        preexec_fn=os.setsid, # Detach from parent process group
                        env=current_env
                    )
                else:
                    # Fallback to subprocess.Popen
                    proc = subprocess.Popen(
                        cmd,
                        stdout=log_file,
                        stderr=log_file,
                        preexec_fn=os.setsid, # Detach from parent process group
                        env=current_env
                    )
                
                # Track PGID for cleanup
                try:
                    current_child_pgid = os.getpgid(proc.pid)
                except: pass
                
                # Wait for the process to complete
                return_code = proc.wait()
                
                # Reset tracking
                current_child_pgid = None

                if return_code != 0:
                    print(f"❌ Error executing for {t} threads (returned {return_code}). Check log file for details: {log_path}")
                else:
                    print(f"✅ Experiment for {t} threads completed successfully. Details in: {log_path}")

        except FileNotFoundError:
            print(f"❌ Error: Command not found. Make sure 'numactl' and 'python' are in your PATH.")
        except Exception as e:
            print(f"❌ An unexpected error occurred: {e}")
        except KeyboardInterrupt:
            print("\n⚠️ Interrompido pelo usuário. Finalizando escalabilidade.")
            break

if __name__ == "__main__":
    main()
