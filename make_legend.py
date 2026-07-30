import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# ==========================================
# 1. STYLE CONFIGURATION (Exact match to your script)
# ==========================================
plt.rcParams.update({
    "font.family": "serif",
    "font.size": 10,
    "legend.fontsize": 9,
    "lines.linewidth": 1.2,
    "lines.markersize": 5, 
    "figure.dpi": 300,
    "pdf.fonttype": 42,
    "ps.fonttype": 42
})

DS_COLOR_MAP = {
    "Linden": "#2ca02c", "PIPQ": "#9467bd", "Spray": "#ff7f0e", "SMQ": "#8c564b",
    "MBQ-Batch=1": "#1f77b4", "MBQ-Batch=B": "#1f77b4",
    "QPID-Batch=1": "#d62728", "QPID-Batch=C": "#d62728",
}

DS_MARKER_MAP = {
    "Linden": "o", "PIPQ": "s", "Spray": "x", "SMQ": "+",
    "MBQ-Batch=1": "^", "MBQ-Batch=B": "^",
    "QPID-Batch=1": "D", "QPID-Batch=C": "D"
}

LEGEND_ORDER = [
    "QPID-Batch=1", "QPID-Batch=C", "Linden", "PIPQ",
    "MBQ-Batch=1", "MBQ-Batch=B", "Spray", "SMQ"
]

# ==========================================
# 2. LEGEND ELEMENT CREATION
# ==========================================
legend_elements = []

# Column 1: Data Structures
for ds in LEGEND_ORDER:
    color = DS_COLOR_MAP.get(ds, "#000000")
    marker = DS_MARKER_MAP.get(ds, "o")
    
    # Matching your variant logic: B/C variants get open markers and dash-dot lines
    if ("=B" in ds) or ("=C" in ds):
        mfc = "none"
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

# Column 2: 8 Invisible spacers to force the 8-row vertical fill
for _ in range(8):
    legend_elements.append(Line2D([0], [0], color='none', label=' '))

# ==========================================
# 3. GENERATE AND SAVE PDF
# ==========================================
fig, ax = plt.subplots(figsize=(4, 5))
ax.axis('off')

# ncol=2 combined with 16 total elements ensures 8 rows
ax.legend(
    handles=legend_elements,
    loc='center',
    ncol=2,
    columnspacing=1.5,
    handletextpad=0.5,
    frameon=True,
    edgecolor='black'
)

# Using .pdf and bbox_inches='tight' for a clean crop
plt.savefig("legend.pdf", bbox_inches='tight', pad_inches=0.01)
plt.close()

print("Legend saved as 'legend.pdf' (2 columns, 8 rows).")

# import matplotlib.pyplot as plt
# from matplotlib.lines import Line2D

# # ==========================================
# # 1. CONFIGURATION (Direct from your script)
# # ==========================================
# DS_COLOR_MAP = {
#     "Linden": "#2ca02c",
#     "PIPQ": "#9467bd",  
#     "Spray": "#ff7f0e",
#     "SMQ": "#8c564b",
#     "MBQ-Batch=1": "#1f77b4", # Blue family
#     "MBQ-Batch=B": "#1f77b4", # Blue family
#     "QPID-Batch=1": "#d62728", # Red family
#     "QPID-Batch=C": "#d62728", # Red family
# }

# DS_MARKER_MAP = {
#     "Linden": "o",
#     "PIPQ": "s",
#     "Spray": "x",
#     "SMQ": "+",
#     "MBQ-Batch=1": "^", # Triangle family
#     "MBQ-Batch=B": "^", # Triangle family
#     "QPID-Batch=1": "D", # Diamond family
#     "QPID-Batch=C": "D"  # Diamond family
# }

# # This is the strict order you requested
# LEGEND_ORDER = [
#     "QPID-Batch=1",
#     "QPID-Batch=C",
#     "Linden",
#     "PIPQ",
#     "MBQ-Batch=1",
#     "MBQ-Batch=B",
#     "Spray",
#     "SMQ"
# ]

# # ==========================================
# # 2. CREATE LEGEND HANDLES (With recommended clarity improvements)
# # ==========================================
# legend_elements = []

# for ds in LEGEND_ORDER:
#     color = DS_COLOR_MAP[ds]
#     marker = DS_MARKER_MAP[ds]
    
#     # Define parameters for the "Open" (unfilled) marker variant
#     # In your original script, these were QPID-Batch=C and MBQ-Batch=B.
#     # If the DS name contains "=B" or "=C", make it an open marker.
#     if ("=B" in ds) or ("=C" in ds):
#         mfc = "none"          # Marker Face Color = transparent
#         mec = color           # Marker Edge Color matches line color
#         mew = 2.0             # Marker Edge Width (thicker for visibility)
#     else:
#         # For standard/standard DS, use a standard filled marker
#         mfc = color           # Filled with the color
#         mec = color           # Edge same as color
#         mew = 1.0             # Normal edge width
    
#     line = Line2D(
#         [0], [0], 
#         color=color,          # Line color
#         marker=marker,        # Shape
#         linestyle="-",        # We use standard solid lines for all now (clearer!)
#         label=ds,
#         markersize=9,         # Slightly larger standard marker size
#         markerfacecolor=mfc,  # Set the fill logic
#         markeredgecolor=mec,  # Set the outline logic
#         markeredgewidth=mew   # Set the outline thickness logic
#     )
#     legend_elements.append(line)

# # ==========================================
# # 3. PLOT AND SAVE LEGEND
# # ==========================================
# # Create a blank figure and axis
# # Using a horizontal aspect ratio appropriate for the final layout
# fig, ax = plt.subplots(figsize=(10, 1.5)) 
# ax.axis('off') # Hide the main axis so only the legend shows

# # Create the legend:
# # - ncol=4 with 8 items total forces precisely 2 rows of data structures.
# # - frameon=False removes the box entirely.
# legend = ax.legend(
#     handles=legend_elements, 
#     loc='center', 
#     ncol=4, 
#     frameon=False,        # <-- No box around the legend
#     handlelength=2.5,     # Adjust line segment length
#     handletextpad=0.5,    # Space between icon and text
#     columnspacing=1.5,    # Space between columns
# )

# # Save the figure tightly bounded around the legend content
# fig.savefig("improved_legend.png", bbox_inches='tight', dpi=300)
# print("Legend saved as 'improved_legend.png'.")

# # Optional: Display the legend
# plt.show()