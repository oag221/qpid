import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import sys
import os
from matplotlib.lines import Line2D
from matplotlib.ticker import LogLocator, FormatStrFormatter  # <-- Updated to use FormatStrFormatter

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
    "soc-LiveJournal1": "LiveJournal",
    "twitter-2010": "TW",
    "soc-livejournal.adj": "LiveJournal",
    "twitter-2010.adj": "TW"
}

# --- USER CONFIG: Define your Plot Order Here ---
ALG_ORDER = [
    "BFS",
    "SSSP",
    "PPSP",
    "Set Cover",
    "PageRank",
    "RBP",
    "A-star"
]

GRAPH_ORDER = [
    "LiveJournal",
    "TW",
    "ORK",
    "RCA"
]

DS_COLOR_MAP = {
    "Lindén-Jonsson": "#2ca02c",
    "PIPQ": "#9467bd",  
    "Spray": "#ff7f0e",
    "SMQ": "#FF69B4",
    "MBQ-Batch": "#1f77b4",
    "MBQ": "#1f77b4",
    "QPID-Batch": "#d62728",
    "QPID": "#d62728",
}

LEGEND_ORDER = [
    "QPID",
    "QPID-Batch",
    "Lindén-Jonsson",
    "PIPQ",
    "MBQ",
    "MBQ-Batch",
    "Spray",
    "SMQ"
]

DS_MARKER_MAP = {
    "Lindén-Jonsson": "o",
    "PIPQ": "s",
    "Spray": "x",
    "SMQ": "+",
    "MBQ-Batch": "^",
    "MBQ": "^",
    "QPID-Batch": "D",
    "QPID": "D"
}

DATA_THREADS = [1, 12, 24, 48, 96]
VISIBLE_TICKS = [1, 24, 48, 72, 96]

# ==========================================
# 2. DATA LOADING & SORTING
# ==========================================
if len(sys.argv) < 2:
    print("Usage: python make_plots.py <your_data.csv> [--log]")
    sys.exit(1)

csv_filename = sys.argv[1]

# Check if the optional --log flag is passed as the second argument
use_log_scale = len(sys.argv) > 2 and sys.argv[2] == "--log"

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

# Filter for threads
df = df[df['threads'].isin(DATA_THREADS)]

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
print(f"Log Scale Enabled: {use_log_scale}")
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
    "lines.markersize": 5, 
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
        ds_data = current_data[current_data['ds'] == ds].sort_values('threads')
        color = DS_COLOR_MAP.get(ds, "#000000")
        marker = DS_MARKER_MAP.get(ds, "o")
        
        # --- UPDATED MARKER & LINE LOGIC ---
        if ds.endswith("-Batch"):
            mfc = "white"      # Hollow marker (not filled with competitor color)
            mec = color
            mew = 1.2
            ls = "-."          # Dash-dot line for variant DS
        else:
            mfc = color        # Filled marker
            mec = color
            mew = 1.0
            ls = "-"           # Solid line for standard DS
        
        # Divide by 1000 for Seconds
        ax.plot(ds_data['threads'], ds_data['time_ms'] / 1000.0, 
                label=ds, 
                color=color, 
                marker=marker, 
                linestyle=ls,
                markerfacecolor=mfc,
                markeredgecolor=mec,
                markeredgewidth=mew)

    # Title: Algorithm first, then Graph
    ax.set_title(f"{alg_name}\n({graph_name})")
    
    # --- CONDITIONAL AXIS LABELS ---
    if i % cols == 0:
        ax.set_ylabel("Runtime (s)") 
    
    if i + cols >= total_plots:
        ax.set_xlabel("Threads")

    # (a), (b) labels
    label_letter = chr(97 + i)
    ax.text(0, 1.01, f"({label_letter})", transform=ax.transAxes,
            fontsize=10, fontweight='normal', va='bottom', ha='left')
    
    # --- UPDATED Y-AXIS & LOG LOGIC ---
    if use_log_scale:
        ax.set_yscale('log')
        
        # On a log scale, we don't force bottom=0. We only set the top ceilings.
        if alg_name == "A-star":
            ax.set_ylim(top=2)
        elif alg_name == "Set Cover" and graph_name in ["LiveJournal", "ORK"]:
            ax.set_ylim(top=4)
        elif alg_name == "Set Cover" and graph_name == "RCA":
            ax.set_ylim(top=2)
        elif alg_name == "RBP":
            ax.set_ylim(top=300)
        elif alg_name == "SSSP":
            if graph_name == "LiveJournal": ax.set_ylim(top=7)
            elif graph_name == "ORK": ax.set_ylim(top=6)
            elif graph_name == "TW": ax.set_ylim(top=80)
        elif alg_name == "PPSP":
            if graph_name == "LiveJournal": ax.set_ylim(top=4)
            elif graph_name == "ORK": ax.set_ylim(top=6)
            elif graph_name == "TW": ax.set_ylim(top=70)

        # 1. Force intermediate ticks at multiples of 1, 2, and 5 within decades
        ax.yaxis.set_major_locator(LogLocator(base=10.0, subs=(1.0, 2.0, 5.0)))
        
        # 2. FIX: Format string '%g' automatically scales precision per tick item,
        # preventing rounding overlaps (e.g., stops 0.05 from rounding up to 0.1)
        ax.yaxis.set_major_formatter(FormatStrFormatter('%g'))
    else:
        # Standard Linear Scale Logic
        if alg_name == "A-star":
            ax.set_ylim(bottom=0, top=2)
        elif alg_name == "Set Cover" and graph_name == "LiveJournal":
            ax.set_ylim(bottom=0, top=4)
        elif alg_name == "Set Cover" and graph_name == "ORK":
            ax.set_ylim(bottom=0, top=4)
        elif alg_name == "Set Cover" and graph_name == "RCA":
            ax.set_ylim(bottom=0, top=2)
        elif alg_name == "RBP":
            ax.set_ylim(bottom=0, top=300)
        elif alg_name == "SSSP":
            if graph_name == "LiveJournal":
                ax.set_ylim(bottom=0, top=7)
            elif graph_name == "ORK":
                ax.set_ylim(bottom=0, top=6)
            elif graph_name == "TW":
                ax.set_ylim(bottom=0, top=80)
        elif alg_name == "PPSP":
            if graph_name == "LiveJournal":
                ax.set_ylim(bottom=0, top=4)
            elif graph_name == "ORK":
                ax.set_ylim(bottom=0, top=6)
            elif graph_name == "TW":
                ax.set_ylim(bottom=0, top=70)
        else:
            ax.set_ylim(bottom=0)
    
    ax.set_xticks(VISIBLE_TICKS)
    ax.grid(True, linestyle=':', alpha=0.7)

# Cleanup unused axes
for j in range(i + 1, len(axes_flat)):
    fig.delaxes(axes_flat[j])

# ==========================================
# 5. LEGEND & SAVE
# ==========================================
output_dir = "plots"
os.makedirs(output_dir, exist_ok=True)

base_name = os.path.basename(csv_filename)

suffix = "_ordered_plots_log.pdf" if use_log_scale else "_ordered_plots.pdf"
output_filename = os.path.splitext(base_name)[0] + suffix
output_path = os.path.join(output_dir, output_filename)

# --- UPDATED LEGEND CREATION LOGIC ---
legend_elements = []
for ds in unique_ds_in_data:
    color = DS_COLOR_MAP.get(ds, "#000000")
    marker = DS_MARKER_MAP.get(ds, "o")
    
    if ds.endswith("-Batch"):
        mfc = "white"
        mec = color
        mew = 1.2
        ls = "-."
    else:
        mfc = color
        mec = color
        mew = 1.0
        ls = "-"
        
    line = Line2D([0], [0], 
                  color=color, 
                  label=ds, 
                  linestyle=ls, 
                  marker=marker, 
                  markerfacecolor=mfc, 
                  markeredgecolor=mec, 
                  markeredgewidth=mew)
    legend_elements.append(line)

# Standard legend placement
fig.legend(handles=legend_elements, loc='upper center', bbox_to_anchor=(0.5, 1.02),
           ncol=min(len(legend_elements), 4), frameon=False)

plt.tight_layout(rect=[0, 0, 1, 0.9])
plt.savefig(output_path, bbox_inches='tight')
print(f"\nSuccessfully generated: {output_path}")


# import pandas as pd
# import matplotlib.pyplot as plt
# import numpy as np
# import sys
# import os
# from matplotlib.lines import Line2D
# from matplotlib.ticker import LogLocator, FormatStrFormatter  # <-- Updated to use FormatStrFormatter

# # ==========================================
# # 1. STATIC CONFIGURATION
# # ==========================================

# # --- RENAME MAPPINGS ---
# ALG_NAME_MAP = {
#     "bfs": "BFS",
#     "sssp": "SSSP",
#     "ppsp": "PPSP",
#     "SetCover_MQ": "Set Cover",
#     "astar": "A-star",
#     "rbp": "RBP",
#     "pagerank": "PageRank"
# }

# GRAPH_NAME_MAP = {
#     "livejournal": "LiveJournal",
#     "livejournal.adj": "LiveJournal",
#     "orkut": "ORK",
#     "orkut.adj": "ORK",
#     "roadnetCA": "RCA",
#     "roadnetCA.adj": "RCA",
#     "germany.bin": "Germany Roads",
#     "germany": "Germany Roads",
#     "soc-LiveJournal1": "LiveJournal",
#     "twitter-2010": "TW",
#     "soc-livejournal.adj": "LiveJournal",
#     "twitter-2010.adj": "TW"
# }

# # --- USER CONFIG: Define your Plot Order Here ---
# ALG_ORDER = [
#     "BFS",
#     "SSSP",
#     "PPSP",
#     "Set Cover",
#     "PageRank",
#     "RBP",
#     "A-star"
# ]

# GRAPH_ORDER = [
#     "LiveJournal",
#     "TW",
#     "ORK",
#     "RCA"
# ]

# DS_COLOR_MAP = {
#     "Lindén-Jonsson": "#2ca02c",
#     "PIPQ": "#9467bd",  
#     "Spray": "#ff7f0e",
#     "SMQ": "#FF69B4",
#     "MBQ-Batch": "#1f77b4",
#     "MBQ": "#1f77b4",
#     "QPID-Batch": "#d62728",
#     "QPID": "#d62728",
# }

# LEGEND_ORDER = [
#     "QPID",
#     "QPID-Batch",
#     "Lindén-Jonsson",
#     "PIPQ",
#     "MBQ",
#     "MBQ-Batch",
#     "Spray",
#     "SMQ"
# ]

# DS_MARKER_MAP = {
#     "Lindén-Jonsson": "o",
#     "PIPQ": "s",
#     "Spray": "x",
#     "SMQ": "+",
#     "MBQ-Batch": "^",
#     "MBQ": "^",
#     "QPID-Batch": "D",
#     "QPID": "D"
# }

# DATA_THREADS = [1, 12, 24, 48, 96]
# VISIBLE_TICKS = [1, 24, 48, 72, 96]

# # ==========================================
# # 2. DATA LOADING & SORTING
# # ==========================================
# if len(sys.argv) < 2:
#     print("Usage: python make_plots.py <your_data.csv> [--log]")
#     sys.exit(1)

# csv_filename = sys.argv[1]

# # Check if the optional --log flag is passed as the second argument
# use_log_scale = len(sys.argv) > 2 and sys.argv[2] == "--log"

# try:
#     df = pd.read_csv(csv_filename)
# except Exception as e:
#     print(f"Error reading file: {e}")
#     sys.exit(1)

# # Clean string columns
# df['alg'] = df['alg'].astype(str).str.strip()
# df['graph'] = df['graph'].astype(str).str.strip()

# # --- APPLY RENAMING ---
# df['alg'] = df['alg'].replace(ALG_NAME_MAP)
# df['graph'] = df['graph'].replace(GRAPH_NAME_MAP)

# # Filter for threads
# df = df[df['threads'].isin(DATA_THREADS)]

# # --- SORTING LOGIC ---
# def get_alg_priority(alg_name):
#     try:
#         return ALG_ORDER.index(alg_name)
#     except ValueError:
#         return 999 

# def get_graph_priority(graph_name):
#     try:
#         return GRAPH_ORDER.index(graph_name)
#     except ValueError:
#         return 999

# def get_legend_priority(ds_name):
#     try:
#         return LEGEND_ORDER.index(ds_name)
#     except ValueError:
#         return 999

# # 1. Identify all unique (Graph, Alg) pairs
# plot_combinations = df[['graph', 'alg']].drop_duplicates()

# # 2. Assign the rank based on your lists
# plot_combinations['alg_rank'] = plot_combinations['alg'].apply(get_alg_priority)
# plot_combinations['graph_rank'] = plot_combinations['graph'].apply(get_graph_priority) 

# # 3. SORT: Prioritize Algorithm Rank, then Graph Rank
# plot_combinations = plot_combinations.sort_values(by=['alg_rank', 'graph_rank']) 

# plot_combinations_list = list(plot_combinations[['graph', 'alg']].itertuples(index=False, name=None))
# total_plots = len(plot_combinations_list)

# print(f"Total Plots: {total_plots}")
# print(f"Log Scale Enabled: {use_log_scale}")
# print("Plot Order will be:")
# for idx, (g, a) in enumerate(plot_combinations_list):
#     print(f"  {idx+1}. Alg: {a} | Graph: {g}")

# unique_ds_in_data = sorted(df['ds'].unique(), key=get_legend_priority)

# # ==========================================
# # 3. PLOTTING SETUP
# # ==========================================
# plt.rcParams.update({
#     "font.family": "serif",
#     "font.size": 10,
#     "axes.labelsize": 10,
#     "axes.titlesize": 10,
#     "xtick.labelsize": 9,
#     "ytick.labelsize": 9,
#     "legend.fontsize": 9,
#     "lines.linewidth": 1.2,
#     "lines.markersize": 5, 
#     "figure.dpi": 300,
#     "pdf.fonttype": 42,
#     "ps.fonttype": 42
# })

# cols = 4
# rows = (len(plot_combinations_list) + cols - 1) // cols

# fig, axes = plt.subplots(rows, cols, figsize=(cols * 3.5, rows * 3), squeeze=False)
# axes_flat = axes.flatten()

# # ==========================================
# # 4. PLOTTING LOOP
# # ==========================================
# for i, (graph_name, alg_name) in enumerate(plot_combinations_list):
#     ax = axes_flat[i]
    
#     # Filter specific data
#     mask = (df['graph'] == graph_name) & (df['alg'] == alg_name)
#     current_data = df[mask]
    
#     present_ds = sorted(current_data['ds'].unique())
    
#     for ds in present_ds:
#         ds_data = current_data[current_data['ds'] == ds].sort_values('threads')
#         color = DS_COLOR_MAP.get(ds, "#000000")
#         marker = DS_MARKER_MAP.get(ds, "o")
        
#         # --- UPDATED MARKER & LINE LOGIC ---
#         if ("-Batch" in ds):
#             mfc = "none"       # Open marker
#             mec = color
#             mew = 1.2
#             ls = "-."          # Dash-dot line for variant DS
#         else:
#             mfc = color        # Filled marker
#             mec = color
#             mew = 1.0
#             ls = "-"           # Solid line for standard DS
        
#         # Divide by 1000 for Seconds
#         ax.plot(ds_data['threads'], ds_data['time_ms'] / 1000.0, 
#                 label=ds, 
#                 color=color, 
#                 marker=marker, 
#                 linestyle=ls,
#                 markerfacecolor=mfc,
#                 markeredgecolor=mec,
#                 markeredgewidth=mew)

#     # Title: Algorithm first, then Graph
#     ax.set_title(f"{alg_name}\n({graph_name})")
    
#     # --- CONDITIONAL AXIS LABELS ---
#     if i % cols == 0:
#         ax.set_ylabel("Runtime (s)") 
    
#     if i + cols >= total_plots:
#         ax.set_xlabel("Threads")

#     # (a), (b) labels
#     label_letter = chr(97 + i)
#     ax.text(0, 1.01, f"({label_letter})", transform=ax.transAxes,
#             fontsize=10, fontweight='normal', va='bottom', ha='left')
    
#     # --- UPDATED Y-AXIS & LOG LOGIC ---
#     if use_log_scale:
#         ax.set_yscale('log')
        
#         # On a log scale, we don't force bottom=0. We only set the top ceilings.
#         if alg_name == "A-star":
#             ax.set_ylim(top=2)
#         elif alg_name == "Set Cover" and graph_name in ["LiveJournal", "ORK"]:
#             ax.set_ylim(top=4)
#         elif alg_name == "Set Cover" and graph_name == "RCA":
#             ax.set_ylim(top=2)
#         elif alg_name == "RBP":
#             ax.set_ylim(top=300)
#         elif alg_name == "SSSP":
#             if graph_name == "LiveJournal": ax.set_ylim(top=7)
#             elif graph_name == "ORK": ax.set_ylim(top=6)
#             elif graph_name == "TW": ax.set_ylim(top=80)
#         elif alg_name == "PPSP":
#             if graph_name == "LiveJournal": ax.set_ylim(top=4)
#             elif graph_name == "ORK": ax.set_ylim(top=6)
#             elif graph_name == "TW": ax.set_ylim(top=70)

#         # 1. Force intermediate ticks at multiples of 1, 2, and 5 within decades
#         ax.yaxis.set_major_locator(LogLocator(base=10.0, subs=(1.0, 2.0, 5.0)))
        
#         # 2. FIX: Format string '%g' automatically scales precision per tick item,
#         # preventing rounding overlaps (e.g., stops 0.05 from rounding up to 0.1)
#         ax.yaxis.set_major_formatter(FormatStrFormatter('%g'))
#     else:
#         # Standard Linear Scale Logic
#         if alg_name == "A-star":
#             ax.set_ylim(bottom=0, top=2)
#         elif alg_name == "Set Cover" and graph_name == "LiveJournal":
#             ax.set_ylim(bottom=0, top=4)
#         elif alg_name == "Set Cover" and graph_name == "ORK":
#             ax.set_ylim(bottom=0, top=4)
#         elif alg_name == "Set Cover" and graph_name == "RCA":
#             ax.set_ylim(bottom=0, top=2)
#         elif alg_name == "RBP":
#             ax.set_ylim(bottom=0, top=300)
#         elif alg_name == "SSSP":
#             if graph_name == "LiveJournal":
#                 ax.set_ylim(bottom=0, top=7)
#             elif graph_name == "ORK":
#                 ax.set_ylim(bottom=0, top=6)
#             elif graph_name == "TW":
#                 ax.set_ylim(bottom=0, top=80)
#         elif alg_name == "PPSP":
#             if graph_name == "LiveJournal":
#                 ax.set_ylim(bottom=0, top=4)
#             elif graph_name == "ORK":
#                 ax.set_ylim(bottom=0, top=6)
#             elif graph_name == "TW":
#                 ax.set_ylim(bottom=0, top=70)
#         else:
#             ax.set_ylim(bottom=0)
    
#     ax.set_xticks(VISIBLE_TICKS)
#     ax.grid(True, linestyle=':', alpha=0.7)

# # Cleanup unused axes
# for j in range(i + 1, len(axes_flat)):
#     fig.delaxes(axes_flat[j])

# # ==========================================
# # 5. LEGEND & SAVE
# # ==========================================
# output_dir = "plots"
# os.makedirs(output_dir, exist_ok=True)

# base_name = os.path.basename(csv_filename)

# suffix = "_ordered_plots_log.pdf" if use_log_scale else "_ordered_plots.pdf"
# output_filename = os.path.splitext(base_name)[0] + suffix
# output_path = os.path.join(output_dir, output_filename)

# # --- UPDATED LEGEND CREATION LOGIC ---
# legend_elements = []
# for ds in unique_ds_in_data:
#     color = DS_COLOR_MAP.get(ds, "#000000")
#     marker = DS_MARKER_MAP.get(ds, "o")
    
#     if ("-Batch" in ds):
#         mfc = "none"
#         mec = color
#         mew = 1.2
#         ls = "-."
#     else:
#         mfc = color
#         mec = color
#         mew = 1.0
#         ls = "-"
        
#     line = Line2D([0], [0], 
#                   color=color, 
#                   label=ds, 
#                   linestyle=ls, 
#                   marker=marker, 
#                   markerfacecolor=mfc, 
#                   markeredgecolor=mec, 
#                   markeredgewidth=mew)
#     legend_elements.append(line)

# # Standard legend placement
# fig.legend(handles=legend_elements, loc='upper center', bbox_to_anchor=(0.5, 1.02),
#            ncol=min(len(legend_elements), 4), frameon=False)

# plt.tight_layout(rect=[0, 0, 1, 0.9])
# plt.savefig(output_path, bbox_inches='tight')
# print(f"\nSuccessfully generated: {output_path}")