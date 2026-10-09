#!/usr/bin/env python3
"""gen_cc_fixtures.py -- Generate structured fixtures for CC testing.

Generates rank lists from structured data (gaussian mixtures) that produce
meaningful connected components, unlike the random permutation fixture.
"""

import sys
import os
import hashlib
import struct
import numpy as np

def gaussian_mixture_fixture(seed, N, L, k, n_components=5, separation=3.0):
    """Generate rank lists from Gaussian mixtures in embedding space.
    
    Creates n_components clusters in embedding space, then derives
    rank lists by nearest neighbor search in the embedding space.
    This produces meaningful connected components.
    """
    np.random.seed(seed)
    
    # Generate cluster centers in k-dimensional space
    centers = np.random.randn(n_components, k) * separation
    
    # Assign each point to a cluster
    labels = np.random.randint(0, n_components, N)
    
    # Generate embeddings: cluster center + noise
    embeddings = centers[labels] + np.random.randn(N, k) * 0.5
    
    # Compute rank lists: for each point, find L nearest neighbors
    # using cosine similarity (since RFE uses cosine-like similarity)
    from sklearn.metrics.pairwise import cosine_similarity
    sims = cosine_similarity(embeddings)
    
    # For each point, get top L neighbors (including self at pos 0)
    rank_lists = np.zeros((N, L), dtype=np.int32)
    for i in range(N):
        # Get similarity scores
        sims_i = sims[i]
        # Exclude self from sorting, then add back at position 0
        idx = np.argsort(-sims_i)  # descending
        # Ensure self is at position 0
        idx = np.delete(idx, np.where(idx == i)[0][0])
        rank_lists[i, 0] = i
        rank_lists[i, 1:L] = idx[:L-1]
    
    return rank_lists.flatten(), labels

def mixture_fixture(seed, N, L, n_components=5, separation=3.0):
    """Legacy fixture generator using k=10 for embedding dim."""
    return mixture_fixture(seed, N, L, 10, n_components, separation)

def write_binary(path, data, N, L):
    """Write rank lists as binary int32 LE."""
    with open(path, 'wb') as f:
        f.write(struct.pack('<%di' % len(data), *data))
    return N, L

def write_provenance(path, label, src, src_md5, N, L):
    """Write provenance file."""
    md5 = hashlib.md5(open(path, 'rb').read()).hexdigest()
    with open(path + '.provenance', 'w') as f:
        f.write(f"DERIVED dataset\n")
        f.write(f"label      = {label}\n")
        f.write(f"source     = {src}\n")
        f.write(f"source_md5 = {src_md5}\n")
        f.write(f"N          = {N}\n")
        f.write(f"L          = {L}\n")
        f.write("transform  = gaussian mixture -> cosine sim -> rank lists\n")
        f.write(f"output_md5 = {md5}\n")

def displace_self(rank_lists, N, L, mode):
    """H19 fixtures: swap slot 0 with slot p so self sits OFF index 0.

    A swap (not a rotate) keeps the row's id multiset identical to the base
    fixture, so the ONLY difference vs cc_structured is the self position.
    mode: 'early' (p=1, all rows), 'late' (p=L-1, all rows), 'mixed'
    (p=1 for even rows, p=L-1 for odd rows -- near the start AND near the
    end, both directions).
    """
    out = rank_lists.copy()
    for i in range(N):
        p = 1 if mode == 'early' else (L - 1 if mode == 'late' else
                                       (1 if i % 2 == 0 else L - 1))
        a, b = i * L, i * L + p
        out[a], out[b] = out[b], out[a]
    return out


def main():
    out_dir = "Data/fixtures/cc"
    os.makedirs(out_dir, exist_ok=True)
    
    fixtures = [
        # (name, seed, N, L, k, n_components, separation)
        ("cc_degenerate", 42, 200, 40, 10, 1, 1.0),      # all in one CC
        ("cc_structured", 123, 500, 100, 10, 5, 3.0),    # 5 clear CCs
        ("cc_th_cc_0", 456, 500, 100, 10, 5, 3.0),       # th_cc = 0
        ("cc_th_cc_05", 456, 500, 100, 10, 5, 3.0),      # th_cc = 0.05
        ("cc_th_cc_50", 456, 500, 100, 10, 5, 3.0),      # th_cc = 0.5
        ("cc_th_cc_100", 456, 500, 100, 10, 5, 3.0),     # th_cc = 1.0
        ("cc_k2", 789, 500, 100, 2, 5, 3.0),             # k=2
        ("cc_k20", 789, 500, 100, 20, 5, 3.0),           # k=20
        ("cc_k50", 789, 500, 100, 50, 5, 3.0),           # k=50
        ("cc_k100", 789, 500, 100, 100, 5, 3.0),         # k=100 (k=l)
    ]
    
    for name, seed, N, L, k, n_comp, sep in fixtures:
        txt_path = os.path.join(out_dir, f"{name}.txt")
        bin_path = os.path.join(out_dir, f"{name}.bin")
        
        # Generate using k=10 for embedding dim (internal), but we can vary k for rank list width
        rank_lists, labels = gaussian_mixture_fixture(seed, N, L, k, n_comp, sep)
        
        # Write text format (for mk_rk_from_leading_id compatibility)
        with open(txt_path, 'w') as f:
            for q in range(N):
                row = rank_lists[q*L:(q+1)*L]
                f.write(' '.join(map(str, row)) + '\n')
        
        # Write binary
        write_binary(bin_path, rank_lists, N, L)
        
        # Provenance
        txt_md5 = hashlib.md5(open(txt_path, 'rb').read()).hexdigest()
        write_provenance(bin_path, name, "gaussian_mixture", 
                         hashlib.md5(f"{seed},{N},{L},{k},{n_comp},{sep}".encode()).hexdigest(),
                         N, L)
        
        print(f"Generated {name}: N={N} L={L} k={k} txt_md5={txt_md5}")

    # H19 self-displaced variants of cc_structured (same base call, same seed;
    # ONLY the self position differs). See FINDINGS "H19".
    base, _ = gaussian_mixture_fixture(123, 500, 100, 10, 5, 3.0)
    for name, mode in [("cc_selfoff_early", "early"),
                       ("cc_selfoff_late", "late"),
                       ("cc_selfoff_mixed", "mixed")]:
        disp = displace_self(base, 500, 100, mode)
        txt_path = os.path.join(out_dir, f"{name}.txt")
        bin_path = os.path.join(out_dir, f"{name}.bin")
        with open(txt_path, 'w') as f:
            for q in range(500):
                row = disp[q * 100:(q + 1) * 100]
                f.write(' '.join(map(str, row)) + '\n')
        write_binary(bin_path, disp, 500, 100)
        write_provenance(bin_path, name,
                         f"gaussian_mixture+swap0to_{mode}",
                         hashlib.md5(b"123,500,100,10,5,3.0").hexdigest(),
                         500, 100)
        # Invariants: self present somewhere, NOT at slot 0 for every row.
        off0 = sum(1 for q in range(500) if disp[q * 100] != q)
        noself = sum(1 for q in range(500)
                     if q not in disp[q * 100:(q + 1) * 100])
        print(f"Generated {name}: mode={mode} off0={off0}/500 no_self={noself}")
        assert off0 == 500 and noself == 0, "displacement invariant broken"

if __name__ == "__main__":
    main()