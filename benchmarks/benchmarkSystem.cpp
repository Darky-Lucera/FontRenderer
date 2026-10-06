#include "benchmarkSystem.h"

#if defined(_WIN32)
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <TargetConditionals.h>
    #include <sys/sysctl.h>
#elif defined(__x86_64__) || defined(__i386__)
    #include <cpuid.h>
#endif
#if defined(__ANDROID__)
    #include <sys/system_properties.h>
#endif
#if defined(__linux__)
    #include <sched.h>
#endif
//-------------------------------------
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>

//-------------------------------------
namespace Benchmark {

    //---------------------------------
    namespace {

        //-----------------------------
        std::string
        Trim(const std::string &text) {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return {};
            }
            const size_t last = text.find_last_not_of(" \t\r\n");
            return text.substr(first, last - first + 1);
        }

#if defined(_WIN32)
        // Windows keeps the name in the registry on every processor, also on ARM, where there is no cpuid.
        //-----------------------------
        std::string
        GetSystemProcessor() {
            char  name[256] = {};
            DWORD size      = sizeof(name);
            if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                            RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) {
                return {};
            }
            return Trim(name);
        }

#elif defined(__APPLE__)
        //-----------------------------
        std::string
        GetSystemProcessor() {
            char   name[256] = {};
            size_t size      = sizeof(name);
            if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) != 0) {
                return {};
            }
            return Trim(name);
        }

#elif defined(__x86_64__) || defined(__i386__)
        // The brand string of cpuid, which every x86 processor of this century has.
        //-----------------------------
        std::string
        GetSystemProcessor() {
            if (unsigned(__get_cpuid_max(0x80000000u, nullptr)) < 0x80000004u) {
                return {};
            }
            unsigned int registers[12] = {};
            for (unsigned int i = 0; i < 3; ++i) {
                __get_cpuid(0x80000002u + i, &registers[4 * i], &registers[4 * i + 1], &registers[4 * i + 2], &registers[4 * i + 3]);
            }
            char name[49] = {};
            memcpy(name, registers, 48);
            return Trim(name);
        }

#elif defined(__linux__)
        // The value of the first "key : value" line of a file with that key.
        //-----------------------------
        std::string
        ReadField(const char *path, const char *key) {
            std::ifstream file(path);
            std::string   line;
            while (std::getline(file, line)) {
                const size_t colon = line.find(':');
                if (colon != std::string::npos && Trim(line.substr(0, colon)) == key) {
                    return Trim(line.substr(colon + 1));
                }
            }
            return {};
        }

        // The ARM cores that phones and boards have, by the part number of /proc/cpuinfo.
        //-----------------------------
        const char *
        GetArmCore(unsigned long part) {
            switch (part) {
                case 0xd03:
                    return "Cortex-A53";
                case 0xd04:
                    return "Cortex-A35";
                case 0xd05:
                    return "Cortex-A55";
                case 0xd07:
                    return "Cortex-A57";
                case 0xd08:
                    return "Cortex-A72";
                case 0xd09:
                    return "Cortex-A73";
                case 0xd0a:
                    return "Cortex-A75";
                case 0xd0b:
                    return "Cortex-A76";
                case 0xd0d:
                    return "Cortex-A77";
                case 0xd41:
                    return "Cortex-A78";
                case 0xd44:
                    return "Cortex-X1";
                case 0xd46:
                    return "Cortex-A510";
                case 0xd47:
                    return "Cortex-A710";
                case 0xd48:
                    return "Cortex-X2";
                case 0xd4d:
                    return "Cortex-A715";
                case 0xd4e:
                    return "Cortex-X3";
                case 0xd80:
                    return "Cortex-A520";
                case 0xd81:
                    return "Cortex-A720";
                case 0xd82:
                    return "Cortex-X4";
                default:
                    return nullptr;
            }
        }

        // The type of the core with the given number, from its block in /proc/cpuinfo. Only cores designed by ARM.
        //-----------------------------
        std::string
        GetCoreType(int current) {
            std::ifstream file("/proc/cpuinfo");
            std::string   line;
            int           processor   = -1;
            unsigned long implementer = 0;
            while (std::getline(file, line)) {
                const size_t colon = line.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const std::string key   = Trim(line.substr(0, colon));
                const std::string value = Trim(line.substr(colon + 1));
                if (key == "processor") {
                    processor = atoi(value.c_str());
                }
                else if (key == "CPU implementer") {
                    implementer = strtoul(value.c_str(), nullptr, 16);
                }
                else if (key == "CPU part" && processor == current && implementer == 0x41) {
                    const char *core = GetArmCore(strtoul(value.c_str(), nullptr, 16));
                    return core != nullptr ? core : "";
                }
            }
            return {};
        }

        // The type and the maximum frequency of the core the program runs on. A chip can have two groups of cores of the
        // same type at different frequencies, and only the frequency tells them apart.
        //-----------------------------
        std::string
        GetCurrentCore() {
            const int     current = sched_getcpu();
            std::string   core    = GetCoreType(current);
            std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(current) + "/cpufreq/cpuinfo_max_freq");
            long          kHz     = 0;
            if (file >> kHz && kHz > 0) {
                char frequency[32];
                snprintf(frequency, sizeof(frequency), "%.2f GHz", kHz / 1e6);
                core += (core.empty() ? "" : " at ") + std::string(frequency);
            }
            return core;
        }

        // ARM Linux does not name the processor in one place. Android names the chip in a property since Android 12, and
        // older versions name its platform. A board like the Raspberry Pi names itself in /proc/cpuinfo or in the device tree.
        //-----------------------------
        std::string
        GetSystemProcessor() {
            std::string chip;
    #if defined(__ANDROID__)
            char value[PROP_VALUE_MAX] = {};
            if (__system_property_get("ro.soc.model", value) > 0 || __system_property_get("ro.board.platform", value) > 0) {
                chip = value;
            }
    #else
            chip = ReadField("/proc/cpuinfo", "Model");
            if (chip.empty()) {
                std::ifstream file("/sys/firmware/devicetree/base/model");
                std::getline(file, chip, '\0');
            }
    #endif
            chip = Trim(chip);

            const std::string core = GetCurrentCore();
            if (chip.empty() || core.empty()) {
                return chip.empty() ? core : chip;
            }
            return chip + ", " + core;
        }

#else
        //-----------------------------
        std::string
        GetSystemProcessor() {
            return {};
        }
#endif

        //-----------------------------
        const char *
        GetSystemName() {
#if defined(_WIN32)
            return "windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
            return "ios";
#elif defined(__APPLE__)
            return "macos";
#elif defined(__ANDROID__)
            return "android";
#elif defined(__linux__)
            return "linux";
#elif defined(__EMSCRIPTEN__)
            return "web";
#elif defined(__DJGPP__)
            return "dos";
#else
            return "unknown";
#endif
        }

        //-----------------------------
        std::string
        GetCompilerName() {
#if defined(__clang__) && defined(__apple_build_version__)
            return "appleclang" + std::to_string(__clang_major__);
#elif defined(__clang__)
            return "clang" + std::to_string(__clang_major__);
#elif defined(_MSC_VER)
            return "msvc" + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
            return "gcc" + std::to_string(__GNUC__);
#else
            return "unknown";
#endif
        }

        //-----------------------------
        const char *
        GetArchitecture() {
#if defined(_M_X64) || defined(__x86_64__)
            return "x64";
#elif defined(_M_IX86) || defined(__i386__)
            return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
            return "arm64";
#elif defined(_M_ARM) || defined(__arm__)
            return "arm32";
#elif defined(__wasm__)
            return "wasm";
#else
            return "unknown";
#endif
        }

        // The words of the processor name that tell it apart, in lowercase and joined by '-': "Intel(R) Core(TM)
        // i7-10850H CPU @ 2.70GHz" gives "i7-10850h", and "AMD Ryzen 7 5800X 8-Core Processor" gives "ryzen-7-5800x".
        //-----------------------------
        std::string
        ShortenProcessor(const std::string &processor) {
            static const char *const kDropped[] = { "intel", "amd", "core", "cpu", "processor", "at", "ghz" };

            std::string lower;
            for (char c : processor) {
                lower += char(tolower(static_cast<unsigned char>(c)));
            }
            for (const char *mark : { "(r)", "(tm)" }) {
                size_t found;
                while ((found = lower.find(mark)) != std::string::npos) {
                    lower.replace(found, strlen(mark), " ");
                }
            }

            std::istringstream words(lower);
            std::string        word;
            std::string        result;
            while (words >> word) {
                // What follows is the clock speed or the integrated graphics.
                if (word == "@" || word == "with") {
                    break;
                }
                bool skip = word.size() > 5 && word.compare(word.size() - 5, 5, "-core") == 0;
                for (const char *dropped : kDropped) {
                    skip = skip || word == dropped;
                }
                if (skip) {
                    continue;
                }
                for (char c : word) {
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.') {
                        result += c;
                    }
                    else if (result.empty() == false && result.back() != '-') {
                        result += '-';
                    }
                }
                if (result.empty() == false && result.back() != '-') {
                    result += '-';
                }
            }
            while (result.empty() == false && result.back() == '-') {
                result.pop_back();
            }
            return result.empty() ? "unknown" : result;
        }

    } // end of namespace

    //---------------------------------
    std::string
    GetCompiler() {
#if defined(__clang__)
        const std::string compiler = "Clang " + Trim(__clang_version__);
#elif defined(_MSC_VER)
        const std::string compiler = "MSVC " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
        const std::string compiler = std::string("GCC ") + __VERSION__;
#else
        const std::string compiler = "Unknown compiler";
#endif
        return compiler + ", " + std::to_string(sizeof(void *) * 8) + " bits";
    }

    //---------------------------------
    std::string
    GetProcessor() {
        return GetSystemProcessor();
    }

    //---------------------------------
    std::string
    GetRunName() {
        return std::string(GetSystemName()) + "_" + GetCompilerName() + "_" + GetArchitecture() + "_" + ShortenProcessor(GetProcessor());
    }

    //---------------------------------
    void
    KeepSystemAwake() {
#if defined(_WIN32)
        // With Modern Standby, the system can enter standby when the display turns off, even with ES_SYSTEM_REQUIRED.
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
    }

} // end of namespace
