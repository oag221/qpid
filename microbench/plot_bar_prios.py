import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import os

# 1. Load data
# file_path = 'data/master_data_n_buckets.csv'
file_path = 'outputs-MASTER-6-1-26/summary.csv'
df = pd.read_csv(file_path)

# Clean column names and data to avoid case/whitespace mismatches
df.columns = [c.strip() for c in df.columns]
df['ds'] = df['ds'].dropna().str.strip()
if 'dist' in df.columns:
    df['dist'] = df['dist'].dropna().str.strip()
else:
    # Fallback if column name has a slightly different casing
    dist_col = [c for c in df.columns if c.lower() == 'dist'][0]
    df['dist'] = df[dist_col].dropna().str.strip()

# Standardize display names mapping: QPID-STRICT -> QPID-NB
df['ds_display'] = df['ds'].replace({'QPID-STRICT': 'QPID-NB'})

# Ensure output directory exists
os.makedirs('plots', exist_ok=True)

# Define constants & discover distributions (Only focusing on 1M prefill)
TARGET_PREFILL = 1000000
unique_dists = df['dist'].dropna().unique()

# Ensure 'flat' distribution is always placed first (on the left)
unique_dists = sorted(unique_dists, key=lambda x: 0 if x.lower() == 'flat' else 1)
num_dists = len(unique_dists)

# Increase Global Font Sizes
plt.rcParams.update({
    'font.size': 14,          # Global default font size
    'axes.labelsize': 16,     # X and Y axis labels
    'axes.titlesize': 18,     # Subplot titles
    'xtick.labelsize': 14,    # X-axis tick labels
    'ytick.labelsize': 12,    # Y-axis tick labels
    'legend.fontsize': 14     # Legend font size
})

# Statically define explicit color/marker/linestyle styles for up to 8 data structures.
# STYLE_MAP = {
#     'QPID-NB':    {'color': 'C0'},
#     'QPID-BATCH': {'color': 'C1'},
#     'MBQ':        {'color': 'C2'},
#     'PIPQ':  {'color': 'C3'},
#     'Spraylist':  {'color': 'C4'},
#     'Linden':  {'color': 'C5'},
#     'SMQ':  {'color': 'C6'},
#     'Baseline5':  {'color': 'C7'}
# }

STYLE_MAP = {
    'QPID-NB':    {'color': 'C0', 'marker': 'X', 'linestyle': '-'},
    'QPID': {'color': 'C1', 'marker': 'o', 'linestyle': '-'},
    'MBQ':        {'color': 'C2', 'marker': 's', 'linestyle': '-'},
    'PIPQ':  {'color': 'C3', 'marker': '^', 'linestyle': '-'},
    'Spraylist':  {'color': 'C4', 'marker': 'v', 'linestyle': '-'},
    'Linden':  {'color': 'C5', 'marker': 'd', 'linestyle': '-'},
    'SMQ':  {'color': 'C6', 'marker': 'p', 'linestyle': '-'},
    'Baseline5':  {'color': 'C7', 'marker': '*', 'linestyle': '-'}
}

# Safely extract any other structures in the CSV not pre-defined above and give them deterministic styles
all_unique_ds = df['ds_display'].dropna().unique()
all_unique_ds.sort()

# Dynamically populate empty slots for any other structures found
style_index = 3 
for ds in all_unique_ds:
    if ds not in STYLE_MAP:
        STYLE_MAP[ds] = {'color': f'C{style_index % 10}'}
        style_index += 1

known_ds = ['QPID-STRICT', 'QPID-BATCH', 'MBQ']
other_ds = [ds for ds in df['ds'].dropna().unique() if ds not in known_ds]

# Define custom categories and target maps for x-axis mapping
categories = ['Large', 'Medium', 'Small']
n_buckets_map = {'Large': 1000, 'Medium': 100, 'Small': 15}

# Track all unique display structures to determine bar positioning width
active_ds = []
for ds_name in ['QPID-STRICT', 'QPID-BATCH', 'MBQ'] + other_ds:
    if ds_name == 'QPID-STRICT':
        display_name = 'QPID-NB'
    elif ds_name == 'QPID-BATCH':
        display_name = 'QPID'
    else:
        display_name = ds_name
    if display_name not in active_ds:
        active_ds.append(display_name)

# Create figure with independent scales
fig, axes = plt.subplots(1, num_dists, figsize=(7 * num_dists, 6.5))

if num_dists == 1:
    axes = [axes]

# Configure positions for grouped bar clusters
x_indices = np.arange(len(categories))
total_width = 0.8
bar_width = total_width / len(active_ds)

# Keep track of handles to construct a single unified bottom legend
legend_handles_labels = {}

# Iterate through each unique distribution value to plot on its respective subplot axis
for idx, dist_val in enumerate(unique_dists):
    ax = axes[idx]
    dist_df = df[df['dist'] == dist_val]
    
    # Structure a 2D map to cleanly extract data points per data structure and category
    data_matrix = {ds: {cat: 0.0 for cat in categories} for ds in active_ds}
    
    # 1. QPID-STRICT / QPID-NB (Best configuration per point)
    strict_df = dist_df[(dist_df['ds'] == 'QPID-STRICT') & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
    if not strict_df.empty:
        for cat in categories:
            val = strict_df[strict_df['n_buckets'] == n_buckets_map[cat]]['thpt']
            if not val.empty:
                data_matrix['QPID-NB'][cat] = val.max()
                
    # 2. QPID-BATCH (Best configuration per point)
    batch_df = dist_df[(dist_df['ds'] == 'QPID-BATCH') & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
    if not batch_df.empty:
        for cat in categories:
            val = batch_df[batch_df['n_buckets'] == n_buckets_map[cat]]['thpt']
            if not val.empty:
                data_matrix['QPID'][cat] = val.max()
                
    # 3. MBQ Data Structure (Best chunk_size per point)
    mbq_df = dist_df[(dist_df['ds'] == 'MBQ') & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
    if not mbq_df.empty:
        for cat in categories:
            val = mbq_df[mbq_df['n_buckets'] == n_buckets_map[cat]]['thpt']
            if not val.empty:
                data_matrix['MBQ'][cat] = val.max()
                
    # 4. Other Data Structures (Fallback)
    for ds_name in other_ds:
        ds_df = dist_df[(dist_df['ds'] == ds_name) & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
        if not ds_df.empty:
            for cat in categories:
                val = ds_df[ds_df['n_buckets'] == n_buckets_map[cat]]['thpt']
                if not val.empty:
                    data_matrix[ds_name][cat] = val.max()

    # Plot Grouped Bar Clusters
    for ds_idx, ds_name in enumerate(active_ds):
        heights = [data_matrix[ds_name][cat] for cat in categories]
        # Dynamically offset each bar around the category center position
        offset = (ds_idx - (len(active_ds) - 1) / 2) * bar_width
        style = STYLE_MAP[ds_name]
        
        bars = ax.bar(x_indices + offset, heights, width=bar_width, 
                      color=style['color'], edgecolor='black', alpha=0.9, label=ds_name)
        
        # Populate global legend handles once
        if ds_name not in legend_handles_labels:
            legend_handles_labels[ds_name] = bars[0]

    # Plot Styling & Labels
    ax.set_xlabel('Distinct Priorities')
    
    # Only label the y-axis for the first (leftmost) plot
    if idx == 0:
        ax.set_ylabel('Throughput (ops/s)')
        
    ax.set_xticks(x_indices)
    
    # --- MODIFIED: Removed the parenthesis, leaving clean strings ---
    ax.set_xticklabels(categories)
    
    # --- MODIFIED: Adjust names from 'Dist' to 'Distribution' and 'flat' to 'uniform' ---
    display_dist = 'uniform' if dist_val.lower() == 'flat' else dist_val
    ax.set_title(f'Distribution = {display_dist}')
    ax.grid(True, linestyle=':', alpha=0.6, axis='y')

# Global Title for the entire compiled PNG
fig.suptitle('Throughput vs Priorities\nPrefill = 1M | Threads = 96', fontsize=20, fontweight='bold', y=0.98)

# Add External Legend at the bottom
if legend_handles_labels:
    fig.legend(
        legend_handles_labels.values(), 
        legend_handles_labels.keys(), 
        loc='lower center', 
        bbox_to_anchor=(0.5, -0.08), 
        ncol=len(legend_handles_labels),  
        frameon=True
    )

# Clean up layout margins and save
plt.tight_layout(rect=[0, 0.04, 1, 0.90])

# Save the combined grid file
output_filepath = 'plots/priorities_vs_throughput_96_threads_bars_final.png'
plt.savefig(output_filepath, dpi=200, bbox_inches='tight')
plt.close()

print(f"Updated bar chart plot successfully saved to '{output_filepath}'.")