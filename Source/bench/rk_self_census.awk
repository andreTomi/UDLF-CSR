# rk_self_census.awk -- census of the INPUT CONTRACT that the BFSTree pipeline
# assumes about real rank lists, row by row:
#
#   self_at_0   : the query id is the first token (the form production reaches
#                 after kernelSortRankedLists' pin step)
#   self_in_L   : the query id appears among the first L tokens
#   self_in_all : the query id appears anywhere in the row
#
# The legacy pin step (BFSTree.cpp: kernelSortRankedLists swaps the self entry
# to position 0, then re-sorts) only needs self_in_all; but the A2/A3/A4
# "pin unconditional" argument in the bench header is documented as relying on
# "self in the list", and the real rkLists may or may not contain self at all
# (an ANN index built excluding self would not). This decides whether the real
# data can be fed to the pipeline verbatim, without inventing a pin.
#
# Row identity: rkLists.txt has no explicit query id, so row number r == id r
# (verified against the first token of row 0 being 0).
#
# Uso: awk -f rk_self_census.awk -v L=1400 rkLists.txt
BEGIN { nrows = 0 }
{
    nrows++;
    want = nrows - 1;
    at0 = 0; inL = 0; inAll = 0;
    if ($1 + 0 == want) { at0 = 1; inL = 1; inAll = 1; }
    else {
        lim = (NF < L) ? NF : L;
        for (i = 1; i <= lim; i++) if ($i + 0 == want) { inL = 1; inAll = 1; break; }
    }
    if (!inAll) for (i = 1; i <= NF; i++) if ($i + 0 == want) { inAll = 1; break; }
    n_at0 += at0; n_inL += inL; n_inAll += inAll;
}
END {
    printf "RKSELF rows=%d L=%d self_at_0=%d self_in_L=%d self_in_all=%d\n",
           nrows, L, n_at0, n_inL, n_inAll;
}
