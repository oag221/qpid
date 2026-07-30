import os
import re
import statistics
from collections import defaultdict

def process_log_runtimes(directory_path):
    # Regex to extract the runtime_ms value
    runtime_pattern = re.compile(r'runtime_ms["\'\s:=]+([0-9]*\.?[0-9]+)')

    # Dictionary to group files by their prepended integer
    experiments = defaultdict(list)

    # 1. Group files by prepended integer AND filter for "SkipHashPQ"
    for filename in os.listdir(directory_path):
        file_path = os.path.join(directory_path, filename)
        
        # Skip if it is a directory OR if "SkipHashPQ" is not in the filename
        if not os.path.isfile(file_path) or "SkipHashPQ" not in filename or "soc-LiveJournal1" in filename:
            continue

        # Extract the prepended integer at the start of the filename
        match = re.match(r'^(\d+)', filename)
        if match:
            exp_id = int(match.group(1))
            experiments[exp_id].append(file_path)

    if not experiments:
        print("No valid SkipHashPQ log files starting with an integer found.")
        return

    std_devs = []
    cvs = []
    robust_cvs = []

    print("Variance per Experimental Setting (SkipHashPQ only):")
    print("-" * 110)
    print(f"{'ID':<5} | {'Runs':<5} | {'Mean':<9} | {'SD':<9} | {'CV':<8} || {'Median':<9} | {'MAD':<9} | {'Robust CV':<8}")
    print("-" * 110)
    
    # 2. Process each group
    for exp_id, files in sorted(experiments.items()):
        runtimes = []
        for file_path in files:
            try:
                with open(file_path, 'r', encoding='utf-8') as f:
                    content = f.read()
                    matches = runtime_pattern.findall(content)
                    runtimes.extend([float(m) for m in matches])
            except Exception as e:
                print(f"  [!] Error reading {file_path}: {e}")

        # Variance calculations require at least 2 data points
        if len(runtimes) >= 2:
            # Standard Stats
            mean_val = statistics.mean(runtimes)
            std_dev = statistics.stdev(runtimes)
            std_devs.append(std_dev)
            
            cv = (std_dev / mean_val) * 100 if mean_val != 0 else 0
            cvs.append(cv)

            # Robust Stats
            median_val = statistics.median(runtimes)
            mad = statistics.median([abs(x - median_val) for x in runtimes])
            
            robust_cv = (mad / median_val) * 100 if median_val != 0 else 0
            robust_cvs.append(robust_cv)

            print(f"{exp_id:<5} | {len(runtimes):<5} | {mean_val:<6.2f} ms | {std_dev:<6.2f} ms | {cv:>5.2f}%   || "
                  f"{median_val:<6.2f} ms | {mad:<6.2f} ms | {robust_cv:>5.2f}%")
                  
        elif len(runtimes) == 1:
            print(f"{exp_id:<5} | 1     | N/A (requires >= 2 runs for variance)")
        else:
            print(f"{exp_id:<5} | 0     | N/A")

    print("\nAggregate Statistics Across All Settings:")
    print("=" * 60)
    
    # 3. Calculate and print aggregate stats for SD, CV, and Robust CV
    if std_devs:
        # Standard Deviation (SD)
        print("Standard Deviation (SD):")
        print(f"  Minimum : {min(std_devs):8.4f} ms")
        print(f"  Average : {statistics.mean(std_devs):8.4f} ms")
        print(f"  Median  : {statistics.median(std_devs):8.4f} ms")
        print(f"  Maximum : {max(std_devs):8.4f} ms")
        print("-" * 30)

        # Coefficient of Variation (CV)
        print("Standard Coefficient of Variation (CV):")
        print(f"  Minimum : {min(cvs):8.2f}%")
        print(f"  Average : {statistics.mean(cvs):8.2f}%")
        print(f"  Median  : {statistics.median(cvs):8.2f}%")
        print(f"  Maximum : {max(cvs):8.2f}%")
        print("-" * 30)

        # Robust Coefficient of Variation (Robust CV)
        print("Robust Coefficient of Variation:")
        print(f"  Minimum : {min(robust_cvs):8.2f}%")
        print(f"  Average : {statistics.mean(robust_cvs):8.2f}%")
        print(f"  Median  : {statistics.median(robust_cvs):8.2f}%")
        print(f"  Maximum : {max(robust_cvs):8.2f}%")
    else:
        print("No valid statistics could be calculated (need >= 2 runtimes per setting).")

if __name__ == "__main__":
    # Replace with the path to your log files directory
    TARGET_DIRECTORY = "o_outputs-ALL-v2/outputs_ppsp-ALL/logs" 
    
    if not os.path.exists(TARGET_DIRECTORY):
        print(f"Please update TARGET_DIRECTORY or ensure '{TARGET_DIRECTORY}' exists.")
    else:
        process_log_runtimes(TARGET_DIRECTORY)