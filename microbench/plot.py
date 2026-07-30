import pandas as pd
import matplotlib.pyplot as plt
import os

# 1. Load data
file_path = 'outputs/summary.csv'
df = pd.read_csv(file_path)

# Strip any leading/trailing spaces from column names just in case
df.columns = [c.strip() for c in df.columns]

# Ensure output directory for plots exists
os.makedirs('plots', exist_ok=True)

# Define our constants
prefills = [100000, 1000000]

# Helper functions to find the combinations producing highest throughput
def get_best_qc(sub_df):
    if sub_df.empty: return None, None
    best_row = sub_df.loc[sub_df['thpt'].idxmax()]
    return best_row['n_queues'], best_row['chunk_size']

def get_best_q(sub_df):
    if sub_df.empty: return None
    best_row = sub_df.loc[sub_df['thpt'].idxmax()]
    return best_row['n_queues']

def get_best_c(sub_df):
    if sub_df.empty: return None
    best_row = sub_df.loc[sub_df['thpt'].idxmax()]
    return best_row['chunk_size']

# Prepare unique values of n_buckets to figure out the number of subplots needed
unique_n_buckets = df['n_buckets'].dropna().unique()
unique_n_buckets.sort()

# Calculate total number of rows:
# 1 row for the main 'n_buckets' plot
# + 1 row for 'n_queues' for each unique n_buckets
# + 1 row for 'chunk_size' for each unique n_buckets
# + 1 row for 'threads' for each unique n_buckets
num_rows = 1 + 3 * len(unique_n_buckets)

# Create the figure and axes (2 columns: left for 100k, right for 1M)
# Height is dynamically set to 4 inches per row to ensure nothing is squished
fig, axes = plt.subplots(num_rows, 2, figsize=(14, 4 * num_rows))

# Loop through our prefills (col 0 = 100000, col 1 = 1000000)
for col_idx, prefill in enumerate(prefills):
    row_idx = 0
    
    # =========================================================
    # Row block 1: X-axis = n_buckets
    # =========================================================
    ax = axes[row_idx, col_idx]
    sub = df[(df['prefill'] == prefill) & (df['threads'] == 96)]
    
    if not sub.empty:
        # Sort by throughput descending, then drop duplicates by n_buckets 
        # to keep the row with the max thpt for each n_buckets value
        best_rows = sub.sort_values('thpt', ascending=False).drop_duplicates('n_buckets')
        best_rows = best_rows.sort_values('n_buckets')
        
        ax.plot(best_rows['n_buckets'], best_rows['thpt'], marker='o', linestyle='-', color='b')
        ax.set_xlabel('n_buckets')
        ax.set_ylabel('Throughput (thpt)')
        ax.set_title(f'n_buckets vs Throughput\nprefill={prefill}, threads=96')
        ax.grid(True)
        
        # Annotate each point with its specific (q, c) combination
        for _, row in best_rows.iterrows():
            nb = row['n_buckets']
            thpt = row['thpt']
            q = int(row['n_queues'])
            c = int(row['chunk_size'])
            
            # Offset text slightly above the marker so it's readable
            ax.annotate(f'({q}, {c})', 
                        (nb, thpt), 
                        textcoords="offset points", 
                        xytext=(0,10), 
                        ha='center', 
                        fontsize=9,
                        color='darkblue')
    else:
        ax.set_title(f'n_buckets vs Throughput\nprefill={prefill} (No Data)')
        ax.axis('off')
        
    row_idx += 1

    # =========================================================
    # Row block 2: X-axis = n_queues
    # =========================================================
    for nb in unique_n_buckets:
        ax = axes[row_idx, col_idx]
        sub = df[(df['prefill'] == prefill) & (df['threads'] == 96) & (df['n_buckets'] == nb)]
        
        if not sub.empty:
            c = get_best_c(sub)
            plot_data = sub[sub['chunk_size'] == c]
            plot_data = plot_data.groupby('n_queues')['thpt'].max().reset_index()
            plot_data = plot_data.sort_values('n_queues')
            
            ax.plot(plot_data['n_queues'], plot_data['thpt'], marker='s', linestyle='-', color='g')
            ax.set_xlabel('n_queues')
            ax.set_ylabel('Throughput (thpt)')
            ax.set_title(f'n_queues vs Throughput\nprefill={prefill}, nb={nb}, threads=96 (c={c})')
            ax.grid(True)
        else:
            ax.set_title(f'n_queues vs Throughput\nprefill={prefill}, nb={nb} (No Data)')
            ax.axis('off')
            
        row_idx += 1

    # =========================================================
    # Row block 3: X-axis = chunk_size
    # =========================================================
    for nb in unique_n_buckets:
        ax = axes[row_idx, col_idx]
        sub = df[(df['prefill'] == prefill) & (df['threads'] == 96) & (df['n_buckets'] == nb)]
        
        if not sub.empty:
            q = get_best_q(sub)
            plot_data = sub[sub['n_queues'] == q]
            plot_data = plot_data.groupby('chunk_size')['thpt'].max().reset_index()
            plot_data = plot_data.sort_values('chunk_size')
            
            ax.plot(plot_data['chunk_size'], plot_data['thpt'], marker='^', linestyle='-', color='r')
            ax.set_xlabel('chunk_size')
            ax.set_ylabel('Throughput (thpt)')
            ax.set_title(f'chunk_size vs Throughput\nprefill={prefill}, nb={nb}, threads=96 (q={q})')
            ax.grid(True)
        else:
            ax.set_title(f'chunk_size vs Throughput\nprefill={prefill}, nb={nb} (No Data)')
            ax.axis('off')
            
        row_idx += 1

    # =========================================================
    # Row block 4: X-axis = threads
    # =========================================================
    for nb in unique_n_buckets:
        ax = axes[row_idx, col_idx]
        sub = df[(df['prefill'] == prefill) & (df['n_buckets'] == nb)]
        
        if not sub.empty:
            q, c = get_best_qc(sub)
            plot_data = sub[(sub['n_queues'] == q) & (sub['chunk_size'] == c)]
            plot_data = plot_data.groupby('threads')['thpt'].max().reset_index()
            plot_data = plot_data.sort_values('threads')
            
            ax.plot(plot_data['threads'], plot_data['thpt'], marker='d', linestyle='-', color='purple')
            ax.set_xlabel('threads')
            ax.set_ylabel('Throughput (thpt)')
            ax.set_title(f'threads vs Throughput\nprefill={prefill}, nb={nb} (q={q}, c={c})')
            ax.grid(True)
        else:
            ax.set_title(f'threads vs Throughput\nprefill={prefill}, nb={nb} (No Data)')
            ax.axis('off')
            
        row_idx += 1

# Apply tight layout to prevent label overlap
plt.tight_layout()

# Save the unified plot
output_filepath = 'plots/all_parameters_combined.png'
plt.savefig(output_filepath)
plt.close()

print(f"Combined plot successfully generated and saved to '{output_filepath}'.")