// gen_tones — one-shot generator for the NotificationService alert-tone WAVs.
//
// Run once:
//   ./build/tools/gen_tones [--force] <output_dir>
//
// Writes one WAV file per NotificationEvent into <output_dir>. Each file
// is 22.05 kHz 16-bit mono PCM. Refuses to overwrite existing files unless
// --force is passed, so committed sounds are not silently clobbered by a
// routine `cmake --build build`.
//
// Per-event timbres (see plan §"Event catalogue"):
//   OrderFilled         — 880 Hz sine, 220 ms, ADSR(5/30 ms / 60% / 100 ms)
//   OrderPartialFill    — same as filled at 60% volume
//   OrderRejected       — descending sweep 440→220 Hz, 380 ms + 100 ms tail
//   OrderCancelled      — two-tone 660 Hz / 440 Hz, 280 ms
//   IbError             — same sweep as rejected at 80% volume
//   ConnectionLost      — 330 Hz warble (5 Hz LFO), 400 ms
//   ConnectionRestored  — C5–E5–G5 arpeggio, 320 ms
//   LongSetup           — 1320 Hz exp-decay, 280 ms
//   ShortSetup          —  660 Hz exp-decay, 280 ms
//   UnguardedPosition   — two-tone 880 Hz / 660 Hz, 320 ms
//   Test                — 1100 Hz sine, 200 ms
//
// Hand-rolled — no miniaudio / no NotificationService.h dependency. The event
// list is duplicated here on purpose so the generator stays self-contained
// and runnable without the rest of the project compiling.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── WAV writer (RIFF / fmt / data) ──────────────────────────────────────────
static bool writeWavMono16(const std::string& path,
                           const std::vector<int16_t>& samples,
                           int sampleRate)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t dataBytes  = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const uint32_t fmtChunkSz = 16;
    const uint16_t numCh      = 1;
    const uint16_t bitsPer    = 16;
    const uint32_t byteRate   = static_cast<uint32_t>(sampleRate) * numCh * (bitsPer / 8);
    const uint16_t blockAlign = numCh * (bitsPer / 8);
    const uint32_t riffSz     = 4 + (8 + fmtChunkSz) + (8 + dataBytes);

    auto put32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto put16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    put32(riffSz);
    std::fwrite("WAVE", 1, 4, f);

    std::fwrite("fmt ", 1, 4, f);
    put32(fmtChunkSz);
    put16(1);                 // PCM
    put16(numCh);
    put32(static_cast<uint32_t>(sampleRate));
    put32(byteRate);
    put16(blockAlign);
    put16(bitsPer);

    std::fwrite("data", 1, 4, f);
    put32(dataBytes);
    std::fwrite(samples.data(), sizeof(int16_t), samples.size(), f);

    return std::fclose(f) == 0;
}

// ── DSP helpers ──────────────────────────────────────────────────────────────
constexpr int    kSampleRate = 22050;
constexpr double kPI         = 3.14159265358979323846;

// Linear ADSR envelope (4 segments, normalised time in [0..1] of total dur).
// attackS / decayS / releaseS in seconds; sustain ∈ [0..1]; durS = total length.
static double adsr(double tS, double durS, double attackS, double decayS,
                   double sustain, double releaseS)
{
    const double sustainEnd = std::max(0.0, durS - releaseS);
    if (tS < attackS) {
        return tS / std::max(1e-9, attackS);
    } else if (tS < attackS + decayS) {
        const double k = (tS - attackS) / std::max(1e-9, decayS);
        return 1.0 + k * (sustain - 1.0);
    } else if (tS < sustainEnd) {
        return sustain;
    } else if (tS < durS) {
        const double k = (tS - sustainEnd) / std::max(1e-9, releaseS);
        return sustain * (1.0 - k);
    }
    return 0.0;
}

// Exponential decay envelope: e^(-tau * t/dur).
static double expDecay(double tS, double durS, double tau = 6.0) {
    return std::exp(-tau * tS / std::max(1e-9, durS));
}

// Convert a buffer of doubles in [-1, 1] to 16-bit PCM with mild peak normalise.
static std::vector<int16_t> toPCM(const std::vector<double>& buf, double targetPeak = 0.85) {
    double peak = 0.0;
    for (double s : buf) peak = std::max(peak, std::fabs(s));
    const double gain = (peak > 1e-9) ? (targetPeak / peak) : 1.0;
    std::vector<int16_t> out;
    out.reserve(buf.size());
    for (double s : buf) {
        double v = s * gain;
        v = std::max(-1.0, std::min(1.0, v));
        out.push_back(static_cast<int16_t>(std::lround(v * 32767.0)));
    }
    return out;
}

// ── per-event synthesisers ──────────────────────────────────────────────────

// Pure-sine ping: f Hz, dur ms, ADSR.
static std::vector<double> makeSinePing(double freq, double durMs,
                                        double attMs = 5, double decMs = 30,
                                        double sustain = 0.6, double relMs = 100,
                                        double levelGain = 1.0)
{
    const int N = static_cast<int>(durMs * kSampleRate / 1000.0);
    std::vector<double> buf(N, 0.0);
    const double durS = durMs / 1000.0;
    for (int i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double env = adsr(t, durS, attMs / 1000.0, decMs / 1000.0,
                                sustain, relMs / 1000.0);
        buf[i] = levelGain * env * std::sin(2.0 * kPI * freq * t);
    }
    return buf;
}

// Frequency sweep f0 → f1, exp-decay envelope. Adds a soft tail.
static std::vector<double> makeSweep(double f0, double f1, double durMs,
                                     double tailMs, double levelGain = 1.0)
{
    const int N      = static_cast<int>((durMs + tailMs) * kSampleRate / 1000.0);
    const int Nbody  = static_cast<int>(durMs * kSampleRate / 1000.0);
    std::vector<double> buf(N, 0.0);
    const double durS = durMs / 1000.0;
    double phase = 0.0;
    for (int i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double k = std::min(1.0, t / durS);
        const double f = f0 + (f1 - f0) * k;       // linear sweep
        phase += 2.0 * kPI * f / kSampleRate;
        const double env = (i < Nbody)
                         ? adsr(t, durS, 5.0/1000.0, 20.0/1000.0, 0.7, 60.0/1000.0)
                         : 0.7 * expDecay(t - durS, tailMs / 1000.0, 5.0);
        buf[i] = levelGain * env * std::sin(phase);
    }
    return buf;
}

// Two-tone (a, gap, b). gapMs of silence between.
static std::vector<double> makeTwoTone(double freqA, double freqB,
                                       double aMs, double gapMs, double bMs,
                                       double levelGain = 1.0)
{
    auto a = makeSinePing(freqA, aMs, 4, 25, 0.6, 50, levelGain);
    auto b = makeSinePing(freqB, bMs, 4, 25, 0.6, 50, levelGain);
    const int gapN = static_cast<int>(gapMs * kSampleRate / 1000.0);
    std::vector<double> buf;
    buf.reserve(a.size() + gapN + b.size());
    buf.insert(buf.end(), a.begin(), a.end());
    buf.insert(buf.end(), gapN, 0.0);
    buf.insert(buf.end(), b.begin(), b.end());
    return buf;
}

// Warble: f0 Hz with sinusoidal LFO modulation at lfoHz (depth in Hz).
static std::vector<double> makeWarble(double freq, double lfoHz, double depthHz,
                                      double durMs)
{
    const int N = static_cast<int>(durMs * kSampleRate / 1000.0);
    std::vector<double> buf(N, 0.0);
    const double durS = durMs / 1000.0;
    double phase = 0.0;
    for (int i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double f = freq + depthHz * std::sin(2.0 * kPI * lfoHz * t);
        phase += 2.0 * kPI * f / kSampleRate;
        const double env = adsr(t, durS, 8.0/1000.0, 30.0/1000.0, 0.7, 80.0/1000.0);
        buf[i] = env * std::sin(phase);
    }
    return buf;
}

// Arpeggio: equal-duration notes back-to-back, brief overlap for legato feel.
static std::vector<double> makeArpeggio(std::vector<double> freqs, double totalMs)
{
    if (freqs.empty()) return {};
    const double per = totalMs / freqs.size();
    std::vector<double> out;
    for (size_t i = 0; i < freqs.size(); ++i) {
        auto note = makeSinePing(freqs[i], per, 4, 20, 0.7, 40, 1.0);
        // 10% overlap with previous note for smoother attack.
        if (!out.empty()) {
            const int overlap = static_cast<int>(0.10 * per * kSampleRate / 1000.0);
            const int start   = static_cast<int>(out.size()) - overlap;
            if (start > 0) {
                for (int k = 0; k < overlap && k < (int)note.size(); ++k) {
                    out[start + k] += note[k] * 0.5;
                }
                out.insert(out.end(), note.begin() + overlap, note.end());
                continue;
            }
        }
        out.insert(out.end(), note.begin(), note.end());
    }
    return out;
}

// Exp-decay sine — bell/chime-like.
static std::vector<double> makeBell(double freq, double durMs, double tau = 6.0)
{
    const int N = static_cast<int>(durMs * kSampleRate / 1000.0);
    std::vector<double> buf(N, 0.0);
    const double durS = durMs / 1000.0;
    for (int i = 0; i < N; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double env = expDecay(t, durS, tau) * std::min(1.0, t / 0.005);   // 5 ms attack
        buf[i] = env * std::sin(2.0 * kPI * freq * t);
    }
    return buf;
}

// ── catalogue (event filename → synthesiser) ────────────────────────────────
struct ToneSpec {
    const char* name;
    std::vector<double> (*build)();
};

static std::vector<double> buildOrderFilled()        { return makeSinePing(880.0, 220.0); }
static std::vector<double> buildOrderPartialFill()   { return makeSinePing(880.0, 220.0, 5, 30, 0.6, 100, 0.6); }
static std::vector<double> buildOrderRejected()      { return makeSweep(440.0, 220.0, 380.0, 100.0); }
static std::vector<double> buildOrderCancelled()     { return makeTwoTone(660.0, 440.0, 130.0, 30.0, 130.0); }
static std::vector<double> buildIbError()            { return makeSweep(440.0, 220.0, 380.0, 100.0, 0.8); }
static std::vector<double> buildConnectionLost()     { return makeWarble(330.0, 5.0, 60.0, 400.0); }
static std::vector<double> buildConnectionRestored() { return makeArpeggio({523.25, 659.26, 783.99}, 320.0); }
static std::vector<double> buildLongSetup()          { return makeBell(1320.0, 280.0); }
static std::vector<double> buildShortSetup()         { return makeBell(660.0, 280.0); }
static std::vector<double> buildUnguardedPosition()  { return makeTwoTone(880.0, 660.0, 150.0, 30.0, 150.0); }
static std::vector<double> buildTest()               { return makeSinePing(1100.0, 200.0, 5, 25, 0.7, 80); }
static std::vector<double> buildOrderWorking()       { return makeSinePing(720.0, 140.0, 4, 20, 0.55, 60); }
static std::vector<double> buildOrderHeld()          { return makeTwoTone(540.0, 420.0, 180.0, 40.0, 180.0); }

constexpr ToneSpec kTones[] = {
    { "order_filled",        buildOrderFilled        },
    { "order_partial_fill",  buildOrderPartialFill   },
    { "order_rejected",      buildOrderRejected      },
    { "order_cancelled",     buildOrderCancelled     },
    { "ib_error",            buildIbError            },
    { "connection_lost",     buildConnectionLost     },
    { "connection_restored", buildConnectionRestored },
    { "long_setup",          buildLongSetup          },
    { "short_setup",         buildShortSetup         },
    { "unguarded_position",  buildUnguardedPosition  },
    { "test",                buildTest               },
    { "order_working",       buildOrderWorking       },
    { "order_held",          buildOrderHeld          },
};

// ── main ────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    bool force = false;
    std::string outDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--force") force = true;
        else if (a == "-h" || a == "--help") {
            std::printf("Usage: %s [--force] <output_dir>\n", argv[0]);
            return 0;
        }
        else outDir = a;
    }
    if (outDir.empty()) {
        std::fprintf(stderr, "Usage: %s [--force] <output_dir>\n", argv[0]);
        return 2;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr, "ERROR: cannot create %s: %s\n",
                     outDir.c_str(), ec.message().c_str());
        return 1;
    }

    int written = 0, skipped = 0;
    for (const auto& spec : kTones) {
        const std::string path = outDir + "/" + spec.name + ".wav";
        if (fs::exists(path) && !force) {
            std::printf("[skip]  %s (use --force to overwrite)\n", path.c_str());
            ++skipped;
            continue;
        }
        const auto buf = spec.build();
        const auto pcm = toPCM(buf);
        if (!writeWavMono16(path, pcm, kSampleRate)) {
            std::fprintf(stderr, "ERROR: failed to write %s\n", path.c_str());
            return 1;
        }
        std::printf("[write] %s  (%zu samples, %.0f ms)\n",
                    path.c_str(), pcm.size(),
                    pcm.size() * 1000.0 / kSampleRate);
        ++written;
    }
    std::printf("\nDone: %d written, %d skipped.\n", written, skipped);
    return 0;
}
