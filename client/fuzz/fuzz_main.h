// Standalone driver for fuzz targets, used when the target is not linked with libFuzzer (-fsanitize=fuzzer), for
// example under GCC on MinGW, which has no libFuzzer. Two modes:
//   fuzz_x FILE...                         replay each file (corpus replay, AFL++ file mode); stdin when no file
//   fuzz_x -runs=N [-seed=S] [-max_len=L] [-dict=DICT_FILE] SEED_FILE...   (dictionary in libFuzzer/AFL format)
//                                          mutation fuzzing from the seed files: byte flips, inserts, deletes,
//                                          duplications, splices between seeds and dictionary tokens. A crash
//                                          saves the input to fuzz-crash.bin first; an input slower than 2 s fails.
#pragma once

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

#ifndef SKOWORK_LIBFUZZER
namespace fuzz_driver {

using Bytes = std::vector<std::uint8_t>;

inline Bytes read_stream(std::FILE* f) {
    Bytes data;
    std::uint8_t buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    return data;
}

inline bool read_file(const char* path, Bytes& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    out = read_stream(f);
    std::fclose(f);
    return true;
}

inline const Bytes* g_current = nullptr;

extern "C" inline void on_crash(int sig) {
    if (g_current) {
        if (std::FILE* f = std::fopen("fuzz-crash.bin", "wb")) {
            std::fwrite(g_current->data(), 1, g_current->size(), f);
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "fuzz: signal %d; input saved to fuzz-crash.bin\n", sig);
    std::_Exit(2);
}

inline void mutate(Bytes& d, const std::vector<Bytes>& seeds, const std::vector<std::string>& dict, std::mt19937_64& rng,
                   std::size_t max_len) {
    auto pick = [&](std::size_t n) { return n == 0 ? std::size_t{0} : static_cast<std::size_t>(rng() % n); };
    const int count = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < count; ++i) {
        switch (rng() % 7) {
        case 0:  // flip a bit
            if (!d.empty()) d[pick(d.size())] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            break;
        case 1:  // random byte
            if (!d.empty()) d[pick(d.size())] = static_cast<std::uint8_t>(rng());
            break;
        case 2:  // insert random bytes
            d.insert(d.begin() + static_cast<std::ptrdiff_t>(pick(d.size() + 1)), 1 + pick(8),
                     static_cast<std::uint8_t>(rng()));
            break;
        case 3:  // delete a range
            if (!d.empty()) {
                const std::size_t at = pick(d.size());
                const std::size_t n = 1 + pick(std::min<std::size_t>(d.size() - at, 32));
                d.erase(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            break;
        case 4:  // duplicate a range
            if (!d.empty()) {
                const std::size_t at = pick(d.size());
                const std::size_t n = 1 + pick(std::min<std::size_t>(d.size() - at, 64));
                const Bytes part(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + n));
                d.insert(d.begin() + static_cast<std::ptrdiff_t>(pick(d.size() + 1)), part.begin(), part.end());
            }
            break;
        case 5:  // splice in a piece of another seed
            if (!seeds.empty()) {
                const Bytes& s = seeds[pick(seeds.size())];
                if (!s.empty()) {
                    const std::size_t at = pick(s.size());
                    const std::size_t n = 1 + pick(std::min<std::size_t>(s.size() - at, 128));
                    d.insert(d.begin() + static_cast<std::ptrdiff_t>(pick(d.size() + 1)),
                             s.begin() + static_cast<std::ptrdiff_t>(at), s.begin() + static_cast<std::ptrdiff_t>(at + n));
                }
            }
            break;
        default:  // insert a dictionary token
            if (!dict.empty()) {
                const std::string& t = dict[pick(dict.size())];
                d.insert(d.begin() + static_cast<std::ptrdiff_t>(pick(d.size() + 1)), t.begin(), t.end());
            }
            break;
        }
    }
    if (d.size() > max_len) d.resize(max_len);
}

// One line of a libFuzzer/AFL dictionary: `"token"` or `name="token"`, with \\, \" and \xNN escapes; # comments.
inline void add_dict_entry(const std::string& line, std::vector<std::string>& dict) {
    const std::size_t open = line.find('"');
    const std::size_t close = line.rfind('"');
    if (line.empty() || line[0] == '#' || open == std::string::npos || close <= open) return;
    std::string token;
    for (std::size_t i = open + 1; i < close; ++i) {
        if (line[i] == '\\' && i + 1 < close) {
            const char e = line[++i];
            if (e == 'x' && i + 2 < close) {
                token.push_back(static_cast<char>(std::strtol(line.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            } else {
                token.push_back(e);
            }
        } else {
            token.push_back(line[i]);
        }
    }
    if (!token.empty()) dict.push_back(token);
}

inline long arg_value(const char* arg, const char* name, long def) {
    const std::size_t n = std::strlen(name);
    return std::strncmp(arg, name, n) == 0 ? std::strtol(arg + n, nullptr, 10) : def;
}

inline int run(int argc, char** argv) {
    long runs = 0, seed = 1, max_len = 8192;
    std::vector<std::string> dict;
    std::vector<Bytes> seeds;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "-runs=", 6) == 0) runs = arg_value(a, "-runs=", 0);
        else if (std::strncmp(a, "-seed=", 6) == 0) seed = arg_value(a, "-seed=", 1);
        else if (std::strncmp(a, "-max_len=", 9) == 0) max_len = arg_value(a, "-max_len=", 8192);
        else if (std::strncmp(a, "-dict=", 6) == 0) {
            Bytes text;
            if (!read_file(a + 6, text)) { std::fprintf(stderr, "fuzz: cannot read %s\n", a + 6); return 1; }
            std::string line;
            for (const std::uint8_t c : text) {
                if (c == '\n') { add_dict_entry(line, dict); line.clear(); }
                else if (c != '\r') line.push_back(static_cast<char>(c));
            }
            add_dict_entry(line, dict);
        } else {
            Bytes data;
            if (!read_file(a, data)) { std::fprintf(stderr, "fuzz: cannot read %s\n", a); return 1; }
            seeds.push_back(std::move(data));
        }
    }
    if (runs <= 0 && seeds.empty()) {
        const Bytes data = read_stream(stdin);
        return LLVMFuzzerTestOneInput(data.data(), data.size());
    }
    std::signal(SIGSEGV, on_crash);
    std::signal(SIGILL, on_crash);
    std::signal(SIGABRT, on_crash);
    std::signal(SIGFPE, on_crash);
    using clock = std::chrono::steady_clock;
    double slowest_ms = 0;
    auto exec = [&](const Bytes& d) {
        g_current = &d;
        const auto t0 = clock::now();
        LLVMFuzzerTestOneInput(d.data(), d.size());
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        if (ms > slowest_ms) slowest_ms = ms;
        if (ms > 2000) {
            std::fprintf(stderr, "fuzz: an input took %.0f ms\n", ms);
            on_crash(0);
        }
    };
    for (const Bytes& s : seeds) exec(s);  // replay the seeds first
    std::mt19937_64 rng(static_cast<std::uint64_t>(seed));
    const auto start = clock::now();
    for (long i = 0; i < runs; ++i) {
        Bytes d = seeds.empty() ? Bytes{} : seeds[static_cast<std::size_t>(rng() % seeds.size())];
        mutate(d, seeds, dict, rng, static_cast<std::size_t>(max_len));
        exec(d);
    }
    const double secs = std::chrono::duration<double>(clock::now() - start).count();
    std::printf("fuzz: %ld seeds, %ld mutated runs in %.1f s, slowest input %.2f ms, no crash\n",
                static_cast<long>(seeds.size()), runs, secs, slowest_ms);
    return 0;
}

}  // namespace fuzz_driver

int main(int argc, char** argv) { return fuzz_driver::run(argc, argv); }
#endif
