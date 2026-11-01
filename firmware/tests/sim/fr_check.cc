/* Blackfin-simulator verification of the fractional-MAC DSP primitives against a portable int64
 * reference, so we trust what runs on the device, not just the host build. Build + run with
 * tests/sim/run_fr_check.sh (bfin-elf-g++ -mcpu=bf524 + bfin-elf-run). Not a device artifact. */
#include <stdio.h>
#include <stdint.h>
#include "coop_fixed.h"     /* on __bfin__: coop_mul_fr == hardware MAC builtin */
#include "coop_master.h"

/* portable int64 reference, identical math to the host build of coop_mul_fr */
static int32_t ref_mul_fr(int32_t a, int32_t b){
    int64_t r = ((int64_t)a * (int64_t)b) >> 31;
    return r > 2147483647LL ? 2147483647 : (int32_t)r;
}
static int64_t ref_eq_prod(int32_t c, int32_t x){ return (int64_t)ref_mul_fr(c, x) << (31 - COOP_EQ_FRAC); }
static int32_t ref_biquad(coop_biquad_state *st, int32_t x,
                          int32_t b0,int32_t b1,int32_t b2,int32_t a1,int32_t a2){
    int64_t y = ref_eq_prod(b0,x) + st->s1;
    int32_t yo = coop_sat32(y);
    st->s1 = (int32_t)(ref_eq_prod(b1,x) - ref_eq_prod(a1,yo) + st->s2);
    st->s2 = (int32_t)(ref_eq_prod(b2,x) - ref_eq_prod(a2,yo));
    return yo;
}

static uint32_t rng = 0x12345678u;
static int32_t rnd(){ rng = rng*1664525u + 1013904223u; return (int32_t)rng; }

int main(){
    /* 1. coop_mul_fr vs reference over random + edge vectors */
    int32_t edges[] = { 0, 1, -1, 0x7fffffff, (int32_t)0x80000000, 0x40000000, (int32_t)0xc0000000 };
    long mul_mismatch = 0, mul_n = 0;
    for (unsigned i=0;i<sizeof(edges)/4;i++) for (unsigned j=0;j<sizeof(edges)/4;j++){
        if (coop_mul_fr(edges[i],edges[j]) != ref_mul_fr(edges[i],edges[j])) mul_mismatch++;
        mul_n++;
    }
    for (int k=0;k<200000;k++){
        int32_t a=rnd(), b=rnd();
        if (coop_mul_fr(a,b) != ref_mul_fr(a,b)) mul_mismatch++;
        mul_n++;
    }
    printf("mul_fr: %ld vectors, %ld mismatch vs int64 ref\n", mul_n, mul_mismatch);
    printf("  (-1)*(-1) saturates: builtin=%d ref=%d (want 2147483647)\n",
           coop_mul_fr((int32_t)0x80000000,(int32_t)0x80000000), ref_mul_fr((int32_t)0x80000000,(int32_t)0x80000000));

    /* 2. biquad: run a signal through builtin and reference, compare sample-by-sample.
       Use a +6dB-ish peaking-style coeff set (coeffs can exceed 1) in Q28. */
    int32_t b0=(int32_t)(1.20*(1<<COOP_EQ_FRAC)), b1=(int32_t)(-1.80*(1<<COOP_EQ_FRAC)),
            b2=(int32_t)(0.85*(1<<COOP_EQ_FRAC)), a1=(int32_t)(-1.78*(1<<COOP_EQ_FRAC)),
            a2=(int32_t)(0.83*(1<<COOP_EQ_FRAC));
    coop_biquad_state sf={0,0}, sr={0,0};
    long bq_max_abs_diff=0, bq_n=0;
    for (int k=0;k<100000;k++){
        int32_t x = rnd() >> 1;   /* keep in range */
        int32_t yf = coop_biquad_sample(&sf,x,b0,b1,b2,a1,a2);
        int32_t yr = ref_biquad(&sr,x,b0,b1,b2,a1,a2);
        long d = (long)yf - (long)yr; if (d<0) d=-d;
        if (d>bq_max_abs_diff) bq_max_abs_diff=d;
        bq_n++;
    }
    printf("biquad: %ld samples, max |builtin-ref| = %ld LSB (Q31)\n", bq_n, bq_max_abs_diff);

    int ok = (mul_mismatch==0) && (bq_max_abs_diff<=2);
    printf("%s\n", ok ? "FR_CHECK: OK" : "FR_CHECK: FAIL");
    return ok?0:1;
}
