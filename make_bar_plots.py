import pandas as pd
import sys
import os

# ==========================================
# 1. STATIC CONFIGURATION
# ==========================================

# --- RENAME MAPPINGS ---
ALG_NAME_MAP = {
    "bfs": "BFS",
    "sssp": "SSSP",
    "ppsp": "PPSP",
    "SetCover_MQ": "Set Cover",
    "astar": "A-star",
    "rbp": "RBP",
    "pagerank": "PageRank"
}

GRAPH_NAME_MAP = {
    "livejournal": "LiveJournal",
    "livejournal.adj": "LiveJournal",
    "orkut": "Orkut",
    "orkut.adj": "Orkut",
    "roadnetCA": "RoadnetCA",
    "roadnetCA.adj": "RoadnetCA",
    "germany.bin": "Germany Roads",
    "germany": "Germany Roads"
}

# --- USER CONFIG: Define your Sort Order Here ---
ALG_ORDER = [
    "BFS",
    "SSSP",
    "PPSP",
    "PageRank",
    "Set Cover",
    "RBP",
    "A-star"
]

LEGEND_ORDER = [
    "Us-Batch=1",
    "Us-Batch=X",
    "Linden",
    "PIPQ",
    "MQBucket-Batch=1",
    "MQBucket-Batch=128",
    "Spray",
    "SMQ"
]

DATA_THREADS = [1, 12, 24, 48, 72, 96]

# ==========================================
# 2. DATA LOADING & CLEANING
# ==========================================
if len(sys.argv) < 2:
    print("Usage: python make_table.py <your_data.csv>")
    sys.exit(1)

csv_filename = sys.argv[1]

try:
    df = pd.read_csv(csv_filename)
except Exception as e:
    print(f"Error reading file: {e}")
    sys.exit(1)

# Clean string columns
df['alg'] = df['alg'].astype(str).str.strip()
df['graph'] = df['graph'].astype(str).str.strip()

# Apply renaming
df['alg'] = df['alg'].replace(ALG_NAME_MAP)
df['graph'] = df['graph'].replace(GRAPH_NAME_MAP)

# Filter for threads
df = df[df['threads'].isin(DATA_THREADS)]

# ==========================================
# 3. SORTING LOGIC & PIVOTING
# ==========================================
def get_alg_priority(alg_name):
    try:
        return ALG_ORDER.index(alg_name)
    except ValueError:
        return 999 

def get_ds_priority(ds_name):
    try:
        return LEGEND_ORDER.index(ds_name)
    except ValueError:
        return 999

# Add sorting helper columns
df['alg_rank'] = df['alg'].apply(get_alg_priority)
df['ds_rank'] = df['ds'].apply(get_ds_priority)

# Create the Pivot Table
# Rows: Algorithm, Graph, Data Structure
# Columns: Threads
# Values: Wasted Work (Empty Work)
table = pd.pivot_table(
    df,
    values='wasted_work',
    index=['alg_rank', 'alg', 'graph', 'ds_rank', 'ds'],
    columns='threads',
    aggfunc='mean' # Uses mean if there are multiple runs/exps for the same configuration
)

# Sort the table by the hidden rank columns, then drop the rank columns to clean it up
table = table.sort_index()
table.index = table.index.droplevel(['alg_rank', 'ds_rank'])

# Rename axes for better readability in the final output
table.index.names = ['Algorithm', 'Graph', 'Data Structure']
table.columns.name = 'Threads'

# Add a suffix to thread column names for clarity (e.g., "1" -> "1 Thread")
table.columns = [f"{col} Threads" if col != 1 else "1 Thread" for col in table.columns]

# ==========================================
# 4. EXPORT AND CONSOLE OUTPUT
# ==========================================
output_dir = "tables"
os.makedirs(output_dir, exist_ok=True)

base_name = os.path.basename(csv_filename)
output_filename = os.path.splitext(base_name)[0] + "_empty_work_table.csv"
output_path = os.path.join(output_dir, output_filename)

# Save to CSV
table.to_csv(output_path)

print("\n--- DATA PREVIEW ---")
# Print a preview to the terminal (limits to first 20 rows for readability)
print(table.head(20).to_string())
print("\n" + "="*50)
print(f"Successfully generated full table: {output_path}")