import pandas as pd
import matplotlib.pyplot as plt
import os

# 1. Load data
#file_path = 'data/all_compet_data.csv'
# file_path = 'data/outputs-opt-5-25-26/summary.csv'
file_path = 'outputs/summary.csv'
df = pd.read_csv(file_path)

# Clean column names
df.columns = [c.strip() for c in df.columns]

# Ensure output directory exists
os.makedirs('plots', exist_ok=True)

# Define constants & discover data structures
prefills = [100000, 1000000]
unique_n_buckets = df['n_buckets'].dropna().unique()
unique_n_buckets.sort()

# Identify "other" data structures dynamically
all_ds = df['ds'].dropna().unique()
known_ds = ['QPID-BATCH', 'MBQ', 'QPID-STRICT']
other_ds = [ds for ds in all_ds if ds not in known_ds]

# Calculate layout: 1 row for each unique n_buckets value, 2 columns for prefills
num_rows = len(unique_n_buckets)

# Create the figure grid
fig, axes = plt.subplots(num_rows, 2, figsize=(16, 5 * num_rows), squeeze=False)

# Loop through each unique n_buckets value (Rows)
for row_idx, nb in enumerate(unique_n_buckets):
    # Loop through each prefill configuration (Columns)
    for col_idx, prefill in enumerate(prefills):
        ax = axes[row_idx, col_idx]
        has_data = False
        
        # -----------------------------------------------------
        # 1. QPID Data Structure Line (Special logic)
        # -----------------------------------------------------
        qpid_df = df[(df['ds'] == 'QPID-BATCH') & (df['prefill'] == prefill) & (df['n_buckets'] == nb)]
        qpid_96 = qpid_df[qpid_df['threads'] == 96]
        
        if not qpid_96.empty:
            best_qpid_row = qpid_96.loc[qpid_96['thpt'].idxmax()]
            best_q = best_qpid_row['n_queues']
            best_c_qpid = best_qpid_row['chunk_size']
            
            qpid_plot_data = qpid_df[(qpid_df['n_queues'] == best_q) & (qpid_df['chunk_size'] == best_c_qpid)]
            qpid_plot_data = qpid_plot_data.groupby('threads')['thpt'].max().reset_index().sort_values('threads')
            
            ax.plot(qpid_plot_data['threads'], qpid_plot_data['thpt'], 
                    marker='o', linestyle='-', linewidth=2,
                    label=f'QPID-BATCH (q={int(best_q)}, c={int(best_c_qpid)})')
            has_data = True

        qpid_strict_df = df[(df['ds'] == 'QPID-STRICT') & (df['prefill'] == prefill) & (df['n_buckets'] == nb)]
        qpid_strict_96 = qpid_strict_df[qpid_strict_df['threads'] == 96]
        
        if not qpid_strict_96.empty:
            best_qpid_row = qpid_strict_96.loc[qpid_strict_96['thpt'].idxmax()]
            best_q = best_qpid_row['n_queues']
            best_c_qpid = best_qpid_row['chunk_size']
            
            qpid_plot_data = qpid_strict_df[(qpid_strict_df['n_queues'] == best_q) & (qpid_strict_df['chunk_size'] == best_c_qpid)]
            qpid_plot_data = qpid_plot_data.groupby('threads')['thpt'].max().reset_index().sort_values('threads')
            
            ax.plot(qpid_plot_data['threads'], qpid_plot_data['thpt'], 
                    marker='o', linestyle='-', linewidth=2,
                    label=f'QPID-STRICT (q={int(best_q)}, c={int(best_c_qpid)})')
            has_data = True

        # -----------------------------------------------------
        # 2. MBQ Data Structure Line (Special logic)
        # -----------------------------------------------------
        mbq_df = df[(df['ds'] == 'MBQ') & (df['prefill'] == prefill) & (df['n_buckets'] == nb)]
        mbq_96 = mbq_df[mbq_df['threads'] == 96]
        
        if not mbq_96.empty:
            best_mbq_row = mbq_96.loc[mbq_96['thpt'].idxmax()]
            best_c_mbq = best_mbq_row['chunk_size']
            
            mbq_plot_data = mbq_df[mbq_df['chunk_size'] == best_c_mbq]
            mbq_plot_data = mbq_plot_data.groupby('threads')['thpt'].max().reset_index().sort_values('threads')
            
            ax.plot(mbq_plot_data['threads'], mbq_plot_data['thpt'], 
                    marker='s', linestyle='--', linewidth=2,
                    label=f'MBQ (c={int(best_c_mbq)})')
            has_data = True

        # -----------------------------------------------------
        # 3. Other Data Structures (Generic processing logic)
        # -----------------------------------------------------
        for ds_name in other_ds:
            ds_df = df[(df['ds'] == ds_name) & (df['prefill'] == prefill) & (df['n_buckets'] == nb)]
            
            if not ds_df.empty:
                # Group by threads and take max throughput just in case there are duplicate rows
                ds_plot_data = ds_df.groupby('threads')['thpt'].max().reset_index().sort_values('threads')
                
                ax.plot(ds_plot_data['threads'], ds_plot_data['thpt'], 
                        marker='^', linestyle='-.', alpha=0.8,
                        label=ds_name)
                has_data = True

        # -----------------------------------------------------
        # Subplot Styling & Metadata
        # -----------------------------------------------------
        if has_data:
            ax.set_xlabel('Threads')
            ax.set_ylabel('Throughput (thpt)')
            ax.set_title(f'Threads vs Throughput\nPrefill = {prefill:,}, n_buckets = {int(nb)}')
            ax.grid(True, linestyle=':', alpha=0.6)
            ax.legend(loc='best')
        else:
            ax.set_title(f'Prefill = {prefill:,}, n_buckets = {int(nb)}\n(No Data Available)')
            ax.axis('off')

# Clean up layout margins and save to a unified plot
plt.tight_layout()
output_filepath = 'plots/ds_comparison_by_threads.png'
plt.savefig(output_filepath, dpi=200)
plt.close()

print(f"Updated data structure comparison plot successfully saved to '{output_filepath}'.")
print(f"Dynamically mapped other structures: {other_ds if other_ds else 'None found'}")