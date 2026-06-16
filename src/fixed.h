/* fixed.h -- numeric abstraction for the A/B fixed-point port.
 *
 * `real` is float by default (USE_FIXED=0) so the host build is byte-identical
 * to the original and serves as the reference. Build with -DUSE_FIXED=1 to swap
 * in 16.16 fixed-point (what the 68000 will run) and compare the rendered frame.
 *
 * Rule for converting code: + - < > stay as-is (correct for fixed too); only
 * * and / become rmul/rdiv; float storage entering math gets rf(); ints get ri();
 * results going to pixel coords get rint().
 */
#ifndef FIXED_H
#define FIXED_H
#include <stdint.h>

#ifndef USE_FIXED
#define USE_FIXED 0
#endif

#if USE_FIXED
typedef int32_t real;                                  /* Q16.16 */
static inline real rf(float x){ return (real)(x*65536.0f + (x<0.0f?-0.5f:0.5f)); }
static inline real ri(int i){ return (real)i << 16; }
static inline int  r2i(real a){ return (int)(a >> 16); }
static inline float rtof(real a){ return (float)a / 65536.0f; }
/* 16.16 multiply WITHOUT int64: the 68000 has no 64-bit (or 32-bit) multiply, so the
 * int64 form compiles to __muldi3 (~4-5 mul-equivalents). This 16x16-part decomposition
 * compiles to ~2 __mulsi3 -- ~2x faster -- and matches the int64 result to <=1 LSB. */
static inline real rmul(real a,real b){
    int neg = (a^b) < 0;
    uint32_t ua = a<0 ? -(uint32_t)a : (uint32_t)a, ub = b<0 ? -(uint32_t)b : (uint32_t)b;
    uint32_t al=ua&0xFFFF, ah=ua>>16, bl=ub&0xFFFF, bh=ub>>16;
    uint32_t r = (al*bl>>16) + ah*bl + al*bh + (ah*bh<<16);
    return neg ? -(real)r : (real)r;
}
static inline real rdiv(real a,real b){ return (real)((((int64_t)a) << 16) / b); }
/* 16.16 reciprocal (1/x) without int64: 2^32/|x| via a 32/32 divide (__udivsi3, far
 * cheaper than the 64-bit __divdi3). Exact to <=1 LSB because the result IS the
 * reciprocal -- use only for rdiv(ri(1),x), not general a/b. */
static inline real recip(real x){
    if(x==0) return 0;
    uint32_t ux = x<0 ? -(uint32_t)x : (uint32_t)x;
    real r = (real)(0xFFFFFFFFu / ux);
    return x<0 ? -r : r;
}
#else
typedef float real;
static inline real rf(float x){ return x; }
static inline real ri(int i){ return (real)i; }
static inline int  r2i(real a){ return (int)a; }
static inline float rtof(real a){ return a; }
static inline real rmul(real a,real b){ return a*b; }
static inline real rdiv(real a,real b){ return a/b; }
static inline real recip(real x){ return 1.0f/x; }
#endif

#endif
