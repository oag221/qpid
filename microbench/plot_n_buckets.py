import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os

# 1. Load data
file_path = 'data/master_data_n_buckets.csv'
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

# --- MODIFIED: Ensure 'flat' distribution is always placed first (on the left) ---
unique_dists = sorted(unique_dists, key=lambda x: 0 if x.lower() == 'flat' else 1)
num_dists = len(unique_dists)

# Increase Global Font Sizes
plt.rcParams.update({
    'font.size': 14,          # Global default font size
    'axes.labelsize': 16,     # X and Y axis labels
    'axes.titlesize': 18,     # Subplot titles
    'xtick.labelsize': 12,    # X-axis tick labels
    'ytick.labelsize': 12,    # Y-axis tick labels
    'legend.fontsize': 14     # Legend font size
})

# Statically define explicit color/marker/linestyle styles for up to 8 data structures.
STYLE_MAP = {
    'QPID-NB':    {'color': 'C0', 'marker': 'X', 'linestyle': '-'},
    'QPID-BATCH': {'color': 'C1', 'marker': 'o', 'linestyle': '-'},
    'MBQ':        {'color': 'C2', 'marker': 's', 'linestyle': '--'},
    'Baseline1':  {'color': 'C3', 'marker': '^', 'linestyle': '-.'},
    'Baseline2':  {'color': 'C4', 'marker': 'v', 'linestyle': '-.'},
    'Baseline3':  {'color': 'C5', 'marker': 'd', 'linestyle': ':'},
    'Baseline4':  {'color': 'C6', 'marker': 'p', 'linestyle': ':'},
    'Baseline5':  {'color': 'C7', 'marker': '*', 'linestyle': '-.'}
}

# Safely extract any other structures in the CSV not pre-defined above and give them deterministic styles
all_unique_ds = df['ds_display'].dropna().unique()
all_unique_ds.sort()

# Dynamically populate empty slots for any other structures found
style_index = 3 
for ds in all_unique_ds:
    if ds not in STYLE_MAP:
        STYLE_MAP[ds] = {
            'color': f'C{style_index % 10}',
            'marker': '^',
            'linestyle': '-.'
        }
        style_index += 1

known_ds = ['QPID-STRICT', 'QPID-BATCH', 'MBQ']
other_ds = [ds for ds in df['ds'].dropna().unique() if ds not in known_ds]

# Create one overarching figure with independent subplot scales
fig, axes = plt.subplots(1, num_dists, figsize=(7 * num_dists, 6.5))

# Ensure axes is always iterable even if there is only 1 distribution
if num_dists == 1:
    axes = [axes]

# Keep track of labels to construct a single unified legend later
lines_labels_map = {}

# Iterate through each unique distribution value to plot on its respective subplot axis
for idx, dist_val in enumerate(unique_dists):
    ax = axes[idx]
    dist_df = df[df['dist'] == dist_val]
    has_data = False
    
    # -----------------------------------------------------
    # 1. QPID-STRICT / QPID-NB (Best configuration per point)
    # -----------------------------------------------------
    strict_df = dist_df[(dist_df['ds'] == 'QPID-STRICT') & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
    
    if not strict_df.empty:
        strict_plot = (strict_df.sort_values('thpt', ascending=False)
                       .drop_duplicates('n_buckets')
                       .sort_values('n_buckets'))
        
        style = STYLE_MAP['QPID-NB']
        ax.plot(strict_plot['n_buckets'], strict_plot['thpt'], 
                marker=style['marker'], linestyle=style['linestyle'], linewidth=2.5, color=style['color'],
                label='QPID-NB')
        has_data = True

    # -----------------------------------------------------
    # 2. QPID-BATCH (Best configuration per point)
    # -----------------------------------------------------
    batch_df = dist_df[(dist_df['ds'] == 'QPID-BATCH') & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
    
    if not batch_df.empty:
        batch_plot = (batch_df.sort_values('thpt', ascending=False)
                      .drop_duplicates('n_buckets')
                      .sort_values('n_buckets'))
        
        style = STYLE_MAP['QPID-BATCH']
        ax.plot(batch_plot['n_buckets'], batch_plot['thpt'], 
                marker=style['marker'], linestyle=style['linestyle'], linewidth=2.5, color=style['color'],
                label='QPID-BATCH')
        has_data = True

    # -----------------------------------------------------
    # 3. MBQ Data Structure Line (Best chunk_size per point)
    # -----------------------------------------------------
    mbq_df = dist_df[(dist_df['ds'] == 'MBQ') & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
    
    if not mbq_df.empty:
        mbq_plot = mbq_df.groupby('n_buckets')['thpt'].max().reset_index().sort_values('n_buckets')
        
        style = STYLE_MAP['MBQ']
        ax.plot(mbq_plot['n_buckets'], mbq_plot['thpt'], 
                marker=style['marker'], linestyle=style['linestyle'], linewidth=2.5, color=style['color'],
                label='MBQ')
        has_data = True

    # -----------------------------------------------------
    # 4. Other Data Structures Line (Fallback)
    # -----------------------------------------------------
    for ds_name in other_ds:
        ds_df = dist_df[(dist_df['ds'] == ds_name) & (dist_df['prefill'] == TARGET_PREFILL) & (dist_df['threads'] == 96)]
        
        if not ds_df.empty:
            ds_plot = ds_df.groupby('n_buckets')['thpt'].max().reset_index().sort_values('n_buckets')
            
            display_name = 'QPID-NB' if ds_name == 'QPID-STRICT' else ds_name
            style = STYLE_MAP[display_name]
            
            ax.plot(ds_plot['n_buckets'], ds_plot['thpt'], 
                    marker=style['marker'], linestyle=style['linestyle'], alpha=0.8, color=style['color'],
                    label=display_name)
            has_data = True

    # -----------------------------------------------------
    # Plot Styling & Metadata
    # -----------------------------------------------------
    if has_data:
        ax.set_xlabel('Number of Active Priorities')
        
        # Only label the y-axis for the first (leftmost) plot
        if idx == 0:
            ax.set_ylabel('Throughput (ops/s)')
            
        ax.set_xscale('log') 
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        
        ax.set_title(f'Dist = {dist_val}')
        ax.grid(True, linestyle=':', alpha=0.6, which='both')
        
        # Collect handles and labels for the global legend
        handles, labels = ax.get_legend_handles_labels()
        for handle, label in zip(handles, labels):
            if label not in lines_labels_map:
                lines_labels_map[label] = handle
    else:
        ax.set_title(f'Dist = {dist_val}\n[No Data Available]')
        ax.axis('off')

# Global Title for the entire compiled PNG
fig.suptitle('Throughput vs Priorities\nPrefill = 1M | Threads = 96', fontsize=20, fontweight='bold', y=0.98)

# Add External Legend at the bottom
if lines_labels_map:
    fig.legend(
        lines_labels_map.values(), 
        lines_labels_map.keys(), 
        loc='lower center', 
        bbox_to_anchor=(0.5, -0.08), 
        ncol=len(lines_labels_map),  
        frameon=True
    )

# Use tight_layout padding adjusted for the text sizes
plt.tight_layout(rect=[0, 0.04, 1, 0.90])

# Save the combined grid file
output_filepath = 'plots/priorities_vs_throughput_96_threads.png'
plt.savefig(output_filepath, dpi=200, bbox_inches='tight')
plt.close()

print(f"Updated plot saved successfully to '{output_filepath}'. 'flat' is forced on the left.")