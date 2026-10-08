// X64v2.cpp built again for x86-64-v3, which adds AVX and AVX2, among others, to x86-64-v2. The code still works on 4
// pixels at a time, but the compiler encodes every instruction with VEX: three operands, so it needs fewer copies of
// registers. This tells how much of the gain of Avx2.cpp comes from the level alone.
//
// The macro gives the namespace another name, so that these functions do not clash with those of X64v2.
#define X64v2 X64v3Vex
#include "X64v2.cpp"
