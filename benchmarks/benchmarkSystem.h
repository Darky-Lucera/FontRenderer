#pragma once

//-------------------------------------
#include <string>

// What the results of a run depend on besides the code: the compiler, the system and the processor. It has a file of its
// own because windows.h, which it needs, defines DrawText as a macro.

//-------------------------------------
namespace Benchmark {

    // Like "GCC 15.2.0, 64 bits".
    std::string     GetCompiler();

    // The name the system gives the processor, like "Intel(R) Core(TM) i7-10850H CPU @ 2.70GHz". On ARM Linux and Android
    // it adds the type and the frequency of the core the program runs on, because a phone mixes big and small cores, like
    // "SM8350, Cortex-A78 at 2.42 GHz". Empty if unknown.
    std::string     GetProcessor();

    // A short name for the files of a run: system, compiler, architecture and processor, like
    // "windows_gcc15_x64_i7-10850h". Lowercase letters, digits, '-' and '_', so that it works as a file name anywhere.
    std::string     GetRunName();

    // Asks Windows not to sleep while the program runs: a long run on a laptop can outlast the sleep timer. On macOS,
    // benchmark.sh runs the program under caffeinate instead.
    void            KeepSystemAwake();

} // end of namespace
