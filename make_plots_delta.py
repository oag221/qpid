import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import sys
import os
from matplotlib.lines import Line2D

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
    "orkut": "ORK",
    "orkut.adj": "ORK",
    "roadnetCA": "RCA",
    "roadnetCA.adj": "RCA",
    "germany.bin": "Germany Roads",
    "germany": "Germany Roads",
    "soc-LiveJournal1": "LJ",
    "twitter-2010": "TW",
    "soc-livejournal.adj": "LJ",
    "twitter-2010.adj": "TW"
}

# --- USER CONFIG: Define your Plot Order Here ---
ALG_ORDER = [
    "BFS",
    "SSSP",
    "PPSP",
    "PageRank",
    "Set Cover",
    "RBP",
    "A-star"
]

GRAPH_ORDER = [
    "LJ",
    "TW",
    "ORK",
    "RCA"
]

DS_COLOR_MAP = {
    "Linden": "#2ca02c",
    "PIPQ": "#9467bd",  
    "Spray": "#ff7f0e",
    "SMQ": "#8c564b",
    "MBQ-Batch=1": "#1f77b4",
    "MBQ-Batch=B": "#1f77b4",
    "Us-Batch=1": "#d62728",
    "Us-Batch=C": "#d62728",
}

LEGEND_ORDER = [
    "Us-Batch=1",
    "Us-Batch=C",
    "Linden",
    "PIPQ",
    "MBQ-Batch=1",
    "MBQ-Batch=B",
    "Spray",
    "SMQ"
]

DS_MARKER_MAP = {
    "Linden": "o",
    "PIPQ": "s",
    "Spray": "x",
    "SMQ": "+",
    "MBQ-Batch=1": "^",
    "MBQ-Batch=B": "^",
    "Us-Batch=1": "D",
    "Us-Batch=C": "D"
}

DS_LINE_MAP = {
    "Us-Batch=C": "--",
    "MBQ-Batch=B": "--"
}

# ==========================================
# 2. DATA LOADING & SORTING
# ==========================================
if len(sys.argv) < 2:
    print("Usage: python make_plots.py <your_data.csv>")
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

# --- APPLY RENAMING ---
df['alg'] = df['alg'].replace(ALG_NAME_MAP)
df['graph'] = df['graph'].replace(GRAPH_NAME_MAP)

# --- SORTING LOGIC ---
def get_alg_priority(alg_name):
    try:
        return ALG_ORDER.index(alg_name)
    except ValueError:
        return 999 

def get_graph_priority(graph_name):
    try:
        return GRAPH_ORDER.index(graph_name)
    except ValueError:
        return 999 

def get_legend_priority(ds_name):
    try:
        return LEGEND_ORDER.index(ds_name)
    except ValueError:
        return 999

# 1. Identify all unique (Graph, Alg) pairs
plot_combinations = df[['graph', 'alg']].drop_duplicates()

# 2. Assign the rank based on your lists
plot_combinations['alg_rank'] = plot_combinations['alg'].apply(get_alg_priority)
plot_combinations['graph_rank'] = plot_combinations['graph'].apply(get_graph_priority) 

# 3. SORT: Prioritize Algorithm Rank, then Graph Rank
plot_combinations = plot_combinations.sort_values(by=['alg_rank', 'graph_rank']) 

plot_combinations_list = list(plot_combinations[['graph', 'alg']].itertuples(index=False, name=None))
total_plots = len(plot_combinations_list)

print(f"Total Plots: {total_plots}")
print("Plot Order will be:")
for idx, (g, a) in enumerate(plot_combinations_list):
    print(f"  {idx+1}. Alg: {a} | Graph: {g}")

unique_ds_in_data = sorted(df['ds'].unique(), key=get_legend_priority)

# ==========================================
# 3. PLOTTING SETUP
# ==========================================
plt.rcParams.update({
    "font.family": "serif",
    "font.size": 10,
    "axes.labelsize": 10,
    "axes.titlesize": 10,
    "xtick.labelsize": 9,
    "ytick.labelsize": 9,
    "legend.fontsize": 9,
    "lines.linewidth": 1.2,
    "lines.markersize": 4,
    "figure.dpi": 300,
    "pdf.fonttype": 42,
    "ps.fonttype": 42
})

cols = 4
rows = (len(plot_combinations_list) + cols - 1) // cols

fig, axes = plt.subplots(rows, cols, figsize=(cols * 3.5, rows * 3), squeeze=False)
axes_flat = axes.flatten()

# ==========================================
# 4. PLOTTING LOOP
# ==========================================
for i, (graph_name, alg_name) in enumerate(plot_combinations_list):
    ax = axes_flat[i]
    
    # Filter specific data
    mask = (df['graph'] == graph_name) & (df['alg'] == alg_name)
    current_data = df[mask]
    
    present_ds = sorted(current_data['ds'].unique())
    
    for ds in present_ds:
        # Sort by delta for the x-axis plotting
        ds_data = current_data[current_data['ds'] == ds].sort_values('delta')
        color = DS_COLOR_MAP.get(ds, "#000000")
        marker = DS_MARKER_MAP.get(ds, "o")
        linestyle = DS_LINE_MAP.get(ds, "-")
        
        ax.plot(ds_data['delta'], ds_data['time(ms)'], 
                label=ds, 
                color=color, 
                marker=marker, 
                linestyle=linestyle)

    # Title: Algorithm first, then Graph
    ax.set_title(f"{alg_name}\n({graph_name})")
    
    # --- CONDITIONAL AXIS LABELS ---
    # Y-Label: Only on left-most column (index 0, 3, 6...)
    if i % cols == 0:
        ax.set_ylabel("Time (ms)") 
    
    # X-Label: Only on the bottom-most plot of each column
    if i + cols >= total_plots:
        ax.set_xlabel("Delta")

    # (a), (b) labels
    label_letter = chr(97 + i)
    ax.text(0, 1.01, f"({label_letter})", transform=ax.transAxes,
            fontsize=10, fontweight='normal', va='bottom', ha='left')
    
    # Y-Axis Logic (Kept from template; adjust bounds if your delta times fall outside these ranges)
    # if alg_name == "A-star":
    #     ax.set_ylim(bottom=0, top=2)
    
    ax.grid(True, linestyle=':', alpha=0.7)

# Cleanup unused axes in the grid
for j in range(i + 1, len(axes_flat)):
    fig.delaxes(axes_flat[j])

# ==========================================
# 5. LEGEND & SAVE
# ==========================================
# Create 'plots' directory if it doesn't exist
output_dir = "plots"
os.makedirs(output_dir, exist_ok=True)

base_name = os.path.basename(csv_filename)
output_filename = os.path.splitext(base_name)[0] + "_ordered_plots.pdf"
output_path = os.path.join(output_dir, output_filename)

# Create custom legend elements
legend_elements = [
    Line2D([0], [0], color=DS_COLOR_MAP.get(ds, "#000000"), label=ds, 
           linestyle=DS_LINE_MAP.get(ds, "-"), marker=DS_MARKER_MAP.get(ds, "o")) 
    for ds in unique_ds_in_data
]

# Standard legend placement on top of the figure
fig.legend(handles=legend_elements, loc='upper center', bbox_to_anchor=(0.5, 1.02),
           ncol=min(len(legend_elements), 4), frameon=False)

plt.tight_layout(rect=[0, 0, 1, 0.95])
plt.savefig(output_path, bbox_inches='tight')
print(f"\nSuccessfully generated: {output_path}")