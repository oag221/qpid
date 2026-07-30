import pandas as pd
import matplotlib.pyplot as plt
import os

# 1. Load data
#file_path = 'data/master_data_n_buckets.csv'
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
df['ds_display'] = df['ds'].replace({'QPID-BATCH': 'QPID'})

# Ensure output directory exists
os.makedirs('plots', exist_ok=True)

# Define constants & discover distributions (Only focusing on 1M prefill)
TARGET_PREFILL = 1000000
unique_dists = df['dist'].dropna().unique()

# Ensure 'flat' distribution is always placed first (on the left)
unique_dists = sorted(unique_dists, key=lambda x: 0 if x.lower() == 'flat' else 1)
num_cols = len(unique_dists)

# --- MODIFIED: Reversed category order to be Small -> Medium -> Large ---
categories = ['Small', 'Medium', 'Large']
n_buckets_map = {'Large': 1000, 'Medium': 100, 'Small': 15}
num_rows = len(categories)

# Increase Global Font Sizes
plt.rcParams.update({
    'font.size': 14,          
    'axes.labelsize': 16,     
    'axes.titlesize': 16,     
    'xtick.labelsize': 12,    
    'ytick.labelsize': 12,    
    'legend.fontsize': 14     
})

# Statically define explicit color/marker/linestyle styles
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

# Dynamically populate empty slots for any other structures found
all_unique_ds = df['ds_display'].dropna().unique()
all_unique_ds.sort()

style_index = 3 
for ds in all_unique_ds:
    if ds not in STYLE_MAP:
        STYLE_MAP[ds] = {
            'color': f'C{style_index % 10}',
            'marker': '^',
            'linestyle': '-.'
        }
        style_index += 1

# List of all raw data structures to plot
all_raw_ds = ['QPID-STRICT', 'QPID-BATCH', 'MBQ'] + [ds for ds in df['ds'].dropna().unique() if ds not in ['QPID-STRICT', 'QPID-BATCH', 'MBQ']]

# Create a 3x2 grid of subplots
fig, axes = plt.subplots(num_rows, num_cols, figsize=(7 * num_cols, 5 * num_rows))

# Keep track of handles for a single unified bottom legend
legend_handles_labels = {}

# Iterate through grid: Rows = Categories (n_buckets), Columns = Distributions
for i, cat in enumerate(categories):
    bucket_val = n_buckets_map[cat]
    
    for j, dist_val in enumerate(unique_dists):
        ax = axes[i, j]
        
        # Base subset for this specific subplot
        plot_df_base = df[(df['dist'] == dist_val) & 
                          (df['prefill'] == TARGET_PREFILL) & 
                          (df['n_buckets'] == bucket_val)]
        
        for ds_name in all_raw_ds:
            # display_name = 'QPID-NB' if ds_name == 'QPID-STRICT' else ds_name
            # display_name = 'QPID' if ds_name == 'QPID-BATCH' else ds_name
            if ds_name == 'QPID-STRICT':
                display_name = 'QPID-NB'
            elif ds_name == 'QPID-BATCH':
                display_name = 'QPID'
            else:
                display_name = ds_name
            ds_df = plot_df_base[plot_df_base['ds'] == ds_name]
            
            # Find the best configuration at 96 threads
            df_96 = ds_df[ds_df['threads'] == 96]
            
            if not df_96.empty:
                # Get the exact row defining the peak configuration
                best_idx = df_96['thpt'].idxmax()
                best_row = df_96.loc[best_idx]
                
                # Adjust matching columns based on the data structure
                cols_to_match = ['n_queues', 'chunk_size']
                if ds_name == 'MBQ':
                    cols_to_match = ['chunk_size']  # Ignore n_queues for MBQ
                
                # Filter ALL threads to explicitly match this peak configuration
                mask = pd.Series(True, index=ds_df.index)
                for col in cols_to_match:
                    if col in ds_df.columns:
                        val = best_row[col]
                        if pd.notna(val):
                            mask &= (ds_df[col] == val)
                        else:
                            mask &= ds_df[col].isna()
                
                # Apply config mask and aggregate (max thpt) in case there are remaining degrees of freedom
                config_df = ds_df[mask]
                plot_data = config_df.groupby('threads')['thpt'].max().reset_index().sort_values('threads')
                
                # Plot the line
                style = STYLE_MAP[display_name]
                line, = ax.plot(plot_data['threads'], plot_data['thpt'], 
                                marker=style['marker'], linestyle=style['linestyle'], 
                                linewidth=2.5, alpha=0.9, color=style['color'], 
                                label=display_name)
                
                if display_name not in legend_handles_labels:
                    legend_handles_labels[display_name] = line

        # Assign lowercase letter label based on subplot index
        subplot_idx = i * num_cols + j
        letter_label = f"({chr(ord('a') + subplot_idx)})"
        
        # Subplot Labels and Styling
        display_dist = 'uniform' if dist_val.lower() == 'flat' else dist_val
        ax.set_title(f'{letter_label} Distribution = {display_dist} | Priorities = {cat}')
        
        # Thread counts are explicitly listed for clarity
        ax.set_xticks([1, 12, 24, 48, 96])
        ax.grid(True, linestyle=':', alpha=0.6)
        
        # Only set X-axis label on the bottom row
        if i == num_rows - 1:
            ax.set_xlabel('Threads')
            
        # Only set Y-axis label on the leftmost column
        if j == 0:
            ax.set_ylabel('Throughput (ops/s)')

# Global Title for the entire compiled PNG
fig.suptitle('Throughput Scalability vs. Threads\nPrefill = 1M (Best Configs at 96 Threads)', 
             fontsize=20, fontweight='bold', y=0.98)

# Add External Legend at the bottom
if legend_handles_labels:
    fig.legend(
        legend_handles_labels.values(), 
        legend_handles_labels.keys(), 
        loc='lower center', 
        bbox_to_anchor=(0.5, 0.02), 
        ncol=len(legend_handles_labels),  
        frameon=True
    )

# Use tight_layout padding adjusted for text sizes and legend space
plt.tight_layout(rect=[0, 0.08, 1, 0.93])

# Save the combined grid file
output_filepath = 'plots/threads_vs_throughput_grid_labeled_reversed.png'
plt.savefig(output_filepath, dpi=200, bbox_inches='tight')
plt.close()

print(f"Updated plot saved successfully to '{output_filepath}'.")