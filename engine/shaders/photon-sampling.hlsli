// Nested uniform (Owen) scrambling of the 24-bit aperture coordinate. A distinct
// random flip at every binary-tree node preserves dyadic strata without retaining
// the repeating 2D lattice of a digitally shifted Sobol net. The leading one in
// prefix distinguishes tree depths, including runs of zero input bits.
float scramblePhotonAperture(float coordinate,uint seed) {
    uint bits=uint(coordinate*16777216.0),prefix=1,result=0;
    [loop]for(uint bit=24;bit>0;--bit) {
        uint input=(bits>>(bit-1))&1;
        result=(result<<1)|(input^(hash(prefix^seed)&1));
        prefix=(prefix<<1)|input;
    }
    return float(result)*(1.0/16777216.0);
}
