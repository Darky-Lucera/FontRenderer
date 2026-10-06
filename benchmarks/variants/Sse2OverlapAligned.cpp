// Sse2Overlap.cpp built again, with flags from CMakeLists.txt that align its loops to 32 bytes and keep its jumps from
// crossing or ending on a 32-byte boundary. Where a loop starts and where its jumps fall change how the processor fetches
// and caches the decoded instructions, so the same code can run at another speed at another address. On Intel
// processors from Skylake to Comet Lake, the microcode for the JCC erratum keeps the 32-byte block that holds such a jump
// out of the cache of decoded instructions, and the block is decoded again on every pass.
//
// The macro gives the namespace another name, so that these functions do not clash with those of Sse2Overlap.
#define Sse2Overlap Sse2OverlapAligned
#include "Sse2Overlap.cpp"
