import sys

def make_symmetric(input_file, output_file):
    print(f"Reading from {input_file}...")
    
    # Read all tokens (PBBS get_tokens splits by any whitespace)
    with open(input_file, 'r') as f:
        tokens = f.read().split()
        
    if not tokens:
        print("Empty file!")
        return

    header = tokens[0]
    n = int(tokens[1])
    m = int(tokens[2])
    
    print(f"Original Graph: {n} vertices, {m} edges")

    # Extract offsets and add the final 'm' bound at the end
    offsets = [int(x) for x in tokens[3:3+n]]
    offsets.append(m)
    
    # Extract edges
    edges = [int(x) for x in tokens[3+n : 3+n+m]]

    # 1. Build symmetric adjacency sets
    # Using sets automatically handles duplicate edges
    adj = [set() for _ in range(n)]
    
    for u in range(n):
        start = offsets[u]
        end = offsets[u+1]
        for i in range(start, end):
            v = edges[i]
            adj[u].add(v)
            adj[v].add(u) # Add the reverse edge to guarantee symmetry

    # 2. Rebuild the CSR arrays
    new_offsets = []
    new_edges = []
    current_offset = 0
    
    for u in range(n):
        new_offsets.append(current_offset)
        # PBBS often expects sorted neighbor lists
        neighbors = sorted(list(adj[u]))
        new_edges.extend(neighbors)
        current_offset += len(neighbors)
        
    new_m = current_offset
    print(f"Symmetric Graph: {n} vertices, {new_m} edges")

    # 3. Write back to the PBBS format
    print(f"Writing to {output_file}...")
    with open(output_file, 'w') as f:
        f.write(f"{header}\n")
        f.write(f"{n}\n")
        f.write(f"{new_m}\n")
        
        # Write the n offsets
        for offset in new_offsets:
            f.write(f"{offset}\n")
            
        # Write the new_m edges
        for edge in new_edges:
            f.write(f"{edge}\n")
            
    print("Done!")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 make_symmetric.py <input_graph> <output_graph>")
        sys.exit(1)
        
    make_symmetric(sys.argv[1], sys.argv[2])