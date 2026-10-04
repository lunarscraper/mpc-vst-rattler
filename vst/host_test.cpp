/* Offline x86 test of rattler_vst.cpp (test.sh builds it with ASan/UBSan): silence without a
 * key, tuning (FREQ, pitch bend), SPREAD (harmonic partials, inharmonic drone), CHARACTER
 * (wavefolder, and what it does in each of the other models), the percussion voice, the LFO
 * on VCF 2, FREQ MOD from LFO and envelope, CC 1-7, the spring reverb (tail, PRE VCA, feedback
 * stays bounded), poly mode, the sequencer (own clock and locked to the host's song position),
 * both filters (LP darkens, HP removes the fundamental), the envelope (AR holds
 * and releases, AD decays under a held key), mono key handling, the 32 preset slots (program
 * list, factory sounds, SAVE/LOAD, shared bank file), output ceiling, chunk restore, a random
 * parameter/MIDI stress run, NaN/denormal-free output.
 * With "bench" as the second argument it only measures CPU load. Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4]; char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[2]; } VstEvents;

/* the host's clock (audioMasterGetTime); off = no time info at all, like a host that has none */
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
static VstTimeInfo g_ti;
static bool g_time = false, g_play = false;
static double g_ppq = 0, g_tempo = 120;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op != 7 || !g_time) return 0;
    std::memset(&g_ti, 0, sizeof g_ti);
    g_ti.ppqPos = g_ppq; g_ti.tempo = g_tempo;
    g_ti.flags = (1 << 9) | (1 << 10) | (g_play ? 1 << 1 : 0);
    return (intptr_t)&g_ti;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static const float SR = 44100;
static const int BS = 256;
static bool bad = false;
typedef std::vector<float> Buf;

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
/* set a parameter in the units of module.json (0..100 unless listed; switches by index) */
static void set(AEffect *e, const char *name, double v) {
    double lo = 0, hi = 100;
    const std::string s = name;
    if (s == "Freq") { lo = -24; hi = 24; }
    else if (s == "Model") { hi = 7; }
    else if (s == "LFO Shape") { hi = 7; }
    else if (s == "Seq Clock") { hi = 5; }
    else if (s == "Rev Pre VCA" || s == "Mode" || s == "Seq Start" || !s.compare(0, 5, "Gate ")) { hi = 1; }
    else if (!s.compare(0, 5, "Step ")) { lo = -24; hi = 24; }
    else if (s == "Freq Mod Source") { hi = 2; }
    else if (s == "VCF1 Type" || s == "Env Mode" || s == "Load" || s == "Save") { hi = 1; }
    else if (s == "Preset") { lo = 1; hi = 32; }
    e->setParameter(e, param(e, name), (float)((v - lo) / (hi - lo)));
}
static void press(AEffect *e, const char *name) { set(e, name, 1); set(e, name, 0); }
static void send(AEffect *e, int a, int b, int c, int frame = 0) {
    VstMidiEvent m; std::memset(&m, 0, sizeof m);
    m.type = 1; m.byteSize = sizeof m; m.deltaFrames = frame;
    m.midiData[0] = (unsigned char)a; m.midiData[1] = (unsigned char)b; m.midiData[2] = (unsigned char)c;
    VstEvents ev; ev.numEvents = 1; ev.reserved = 0; ev.events[0] = &m; ev.events[1] = nullptr;
    e->dispatcher(e, 25, 0, 0, &ev, 0);
}
static void on(AEffect *e, int n, int v = 100) { send(e, 0x90, n, v); }
static void off(AEffect *e, int n) { send(e, 0x80, n, 0); }
/* render; returns the left channel (right too if asked) */
static Buf run(AEffect *e, double sec, Buf *right = nullptr) {
    Buf outL, ol(BS), orr(BS);
    float *out[2] = {ol.data(), orr.data()};
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        e->processReplacing(e, nullptr, out, BS);
        if (g_time && g_play) g_ppq += BS * g_tempo / 60.0 / SR;
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
            outL.push_back(ol[i]);
            if (right) right->push_back(orr[i]);
        }
    }
    return outL;
}
static double goertzel(const Buf &a, double f, double t0 = 0.1) {
    size_t i0 = (size_t)(t0 * SR), i1 = a.size();
    double w = 2 * M_PI * f / SR, c = std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = i0; i < i1; i++) { double s0 = a[i] + 2 * c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - 2 * c * s1 * s2)) / (i1 - i0);
}
static double rms(const Buf &a, double t0 = 0.1, double t1 = -1) {
    double s = 0; size_t i0 = (size_t)(t0 * SR), i1 = t1 < 0 ? a.size() : (size_t)(t1 * SR);
    for (size_t i = i0; i < i1; i++) s += (double)a[i] * a[i];
    return std::sqrt(s / (i1 - i0));
}
static double peak(const Buf &a) { double p = 0; for (float s : a) p = std::max(p, (double)std::fabs(s)); return p; }
/* the strongest frequency near f0 (+-6 %), by a Goertzel scan */
static double freq(const Buf &a, double f0) {
    double best = 0, bf = f0;
    for (double f = f0 * 0.94; f <= f0 * 1.06; f *= 1.0005) { double g = goertzel(a, f, 0.2); if (g > best) { best = g; bf = f; } }
    return bf;
}
static double cents(double f, double ref) { return 1200 * std::log2(f / ref); }
static double db(double x) { return 20 * std::log10(x + 1e-12); }

static std::string progname(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 29, i, 0, b, 0); return b; }

/* a plain test patch: one model, no spread, no fold, both filters open, organ envelope */
static void plain(AEffect *e, int model) {
    set(e, "Model", model); set(e, "Freq", 0); set(e, "Spread", 0); set(e, "Character", 0); set(e, "Voice Vol", 50);
    set(e, "VCF1 Cutoff", 100); set(e, "VCF1 Res", 0); set(e, "VCF1 Mod", 0); set(e, "VCF1 Type", 0);
    set(e, "VCF2 Cutoff", 100); set(e, "VCF2 Res", 0);
    set(e, "Attack", 0); set(e, "Decay", 20); set(e, "Env Mode", 0); set(e, "Volume", 80);
    set(e, "Perc", 40); set(e, "Perc Freq", 50); set(e, "Perc Vol", 0);
    set(e, "LFO Speed", 40); set(e, "LFO Shape", 0); set(e, "LFO Slew", 0); set(e, "VCF2 Mod", 0);
    set(e, "Freq Mod", 0); set(e, "Freq Mod Source", 0);
    set(e, "Rev Input", 70); set(e, "Rev Level", 0); set(e, "In", 0); set(e, "Rev Pre VCA", 0);
    set(e, "Mode", 0); set(e, "Seq Start", 0); set(e, "Seq Clock", 3);
    for (int i = 1; i <= 8; i++) { set(e, ("Step " + std::to_string(i)).c_str(), 0); set(e, ("Gate " + std::to_string(i)).c_str(), 1); }
    g_time = g_play = false;
    for (int c = 1; c <= 7; c++) send(e, 0xb0, c, 0);
    send(e, 0xe0, 0, 64); send(e, 0xb0, 123, 0);
}
static Buf note(AEffect *e, int n, double sec) { on(e, n); Buf a = run(e, sec); off(e, n); run(e, 1.0); return a; }

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test plugin.so [bench]\n"); return 2; }
    const char *bank = "/tmp/rattler_test_presets.txt";
    std::remove(bank);
    setenv("RATTLER_PRESETS", bank, 1);
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::printf("FAIL: %s\n", dlerror()); return 1; }
    typedef AEffect *(*MainF)(audioMasterCallback);
    MainF mainf = (MainF)dlsym(h, "VSTPluginMain");
    if (!mainf) { std::printf("FAIL: no VSTPluginMain\n"); return 1; }
    AEffect *e = mainf(master);
    e->dispatcher(e, 10, 0, 0, nullptr, SR);
    e->dispatcher(e, 12, 0, 1, nullptr, 0);

    if (argc > 2 && !std::strcmp(argv[2], "bench")) {
        struct { const char *what; int model; double spread, chr; } B[] = {
            {"ORGAN, spread 30", 0, 30, 0}, {"STRINGS, spread 30, folded", 1, 30, 70}, {"DRONE, full spread, folded", 2, 100, 70},
            {"REED (16 VCOs), folded + reverb", 3, 30, 70}, {"METAL, FM", 4, 60, 70}, {"CHIPTUNE, PWM", 5, 30, 70}, {"GRAINS, delay", 6, 50, 70}, {"NOISE, delay", 7, 50, 70},
        };
        for (auto &b : B) {
            plain(e, b.model); set(e, "Spread", b.spread); set(e, "Character", b.chr); set(e, "VCF1 Res", 50); set(e, "VCF1 Mod", 50);
            set(e, "Perc Vol", 80); set(e, "Perc", 100); set(e, "VCF2 Mod", 50); set(e, "Freq Mod", 20);
            if (b.model == 3) { set(e, "Rev Level", 70); set(e, "In", 40); }
            on(e, 48);
            auto t0 = std::chrono::steady_clock::now();
            run(e, 20.0);
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            off(e, 48);
            std::printf("bench (this CPU, one core): %-34s %.2f %%\n", b.what, 100 * s / 20.0);
        }
        return 0;
    }

    /* 1. silence without a key */
    CHECK(peak(run(e, 0.5)) == 0, "sound without a key");

    /* 2. tuning: A4, FREQ, pitch bend (spread 0 keeps the eight within a few cents) */
    plain(e, 0);
    {
        double c = cents(freq(note(e, 69, 1.0), 440), 440);
        std::printf("  A4 %+.1f ct\n", c);
        CHECK(std::fabs(c) < 6, "A4 off by %.1f cents", c);
        set(e, "Freq", 12);
        c = cents(freq(note(e, 69, 1.0), 880), 880);
        CHECK(std::fabs(c) < 6 && display(e, "Freq") == "+12 st", "FREQ +12: %.1f cents, %s", c, display(e, "Freq").c_str());
        set(e, "Freq", 0);
        send(e, 0xe0, 127, 127);
        c = cents(freq(note(e, 69, 1.0), 493.88), 493.88);
        CHECK(std::fabs(c) < 8, "bend +2: %.1f cents", c);
        send(e, 0xe0, 0, 64);
    }

    /* 3. SPREAD: harmonic partials for ORGAN, inharmonic ones for DRONE */
    {
        plain(e, 0);
        Buf a0 = note(e, 57, 1.5);
        set(e, "Spread", 100);
        Buf a1 = note(e, 57, 1.5);
        double f = 220;
        std::printf("  organ: partial 3 %.1f dB -> %.1f dB, partial 8 %.1f dB\n", db(goertzel(a0, 3 * f)), db(goertzel(a1, 3 * f)), db(goertzel(a1, 8 * f)));
        CHECK(goertzel(a1, 3 * f) > 10 * goertzel(a0, 3 * f) && goertzel(a1, 8 * f) > 10 * goertzel(a0, 8 * f), "full spread has no partials 3 and 8");
        CHECK(goertzel(a1, 2.5 * f) < 0.1 * goertzel(a1, 3 * f), "organ spread is not harmonic");
        plain(e, 2); set(e, "Spread", 100);
        Buf d = note(e, 57, 1.5);
        double top = f * (1 + std::pow(7.0, 1.4));
        CHECK(goertzel(d, top) > 10 * goertzel(d, top * 1.03) && goertzel(d, f * (1 + std::pow(2.0, 1.4))) > 10 * goertzel(d, 3.3 * f), "drone spread is not the inharmonic series");
        plain(e, 1); set(e, "Spread", 40);
        CHECK(rms(note(e, 57, 1.0)) > 0.01, "strings silent");
    }

    /* 4. CHARACTER folds: new harmonics on a sine */
    {
        plain(e, 0); set(e, "Voice Vol", 20);
        Buf a0 = note(e, 57, 1.0);
        set(e, "Character", 80);
        Buf a1 = note(e, 57, 1.0);
        std::printf("  fold: 3rd harmonic %.1f dB -> %.1f dB\n", db(goertzel(a0, 660)), db(goertzel(a1, 660)));
        CHECK(goertzel(a1, 660) + goertzel(a1, 1100) > 8 * (goertzel(a0, 660) + goertzel(a0, 1100)), "character adds no harmonics");
    }

    /* 4b. the five other models: each sounds, and CHARACTER does what its table row says */
    {
        const char *names[] = {"REED", "METAL", "CHIPTUNE", "GRAINS", "NOISE"};
        for (int m = 3; m <= 7; m++) {
            plain(e, m); set(e, "Spread", 20);
            Buf a0 = note(e, 57, 1.5);
            set(e, "Character", 80);
            Buf a1 = note(e, 57, 1.5);
            double diff = 0;
            for (double f : {330.0, 440.0, 660.0, 880.0, 1100.0, 1760.0, 3520.0}) diff += std::fabs(db(goertzel(a1, f)) - db(goertzel(a0, f)));
            std::printf("  %-8s %6.1f dBFS, character 80: %6.1f dBFS, spectrum moves %.0f dB\n", names[m - 3], db(rms(a0)), db(rms(a1)), diff);
            CHECK(display(e, "Model") == names[m - 3], "model name %s", display(e, "Model").c_str());
            CHECK(rms(a0) > 0.01 && rms(a1) > 0.01, "%s silent", names[m - 3]);
            CHECK(diff > 12, "%s: character does nothing", names[m - 3]);
        }
        plain(e, 3);                                    /* REED: the pair beats (about 10 cents apart) */
        Buf r = note(e, 69, 1.0);
        CHECK(goertzel(r, 440 * 1.006) > 0.3 * goertzel(r, 440) && goertzel(r, 440) > 0.01, "reed has no detuned second VCO");
    }

    /* 4c. percussion: fires with every key, PERC sets its length, PERC FREQ its pitch; silent at PERC VOL 0 */
    {
        plain(e, 0); set(e, "Voice Vol", 0); set(e, "Decay", 80);
        on(e, 60); CHECK(peak(run(e, 0.5)) == 0, "sound with both mixer channels closed"); off(e, 60); run(e, 0.5);
        set(e, "Perc Vol", 80); set(e, "Perc", 20);
        on(e, 60); Buf shortp = run(e, 1.0); off(e, 60);
        set(e, "Perc", 90);
        on(e, 60); Buf longp = run(e, 1.0); off(e, 60);
        std::printf("  perc: short %.1f / %.1f dB (first 10 ms / 200-300 ms), long %.1f / %.1f dB, %s, %s\n", db(rms(shortp, 0, 0.01)), db(rms(shortp, 0.2, 0.3)),
                    db(rms(longp, 0, 0.01)), db(rms(longp, 0.2, 0.3)), display(e, "Perc").c_str(), display(e, "Perc Freq").c_str());
        CHECK(rms(shortp, 0, 0.01) > 0.05 && rms(shortp, 0.2, 0.3) < 1e-3, "short percussion wrong");
        CHECK(rms(longp, 0.2, 0.3) > 30 * rms(shortp, 0.2, 0.3) + 1e-3, "PERC does not lengthen the decay");
        set(e, "Perc", 60); set(e, "Perc Freq", 10);
        on(e, 60); Buf lowp = run(e, 0.5); off(e, 60);
        set(e, "Perc Freq", 95);
        on(e, 60); Buf highp = run(e, 0.5); off(e, 60);
        auto band = [](const Buf &a, double f0, double f1) { double s = 0; for (double f = f0; f < f1; f *= 1.06) s += goertzel(a, f, 0); return s; };
        CHECK(band(lowp, 60, 200) > 5 * band(lowp, 3000, 9000) && band(highp, 3000, 9000) > 3 * band(highp, 60, 200), "PERC FREQ does not move the percussion");
        on(e, 60); run(e, 0.3); on(e, 64); Buf again = run(e, 0.05); off(e, 64); off(e, 60);
        CHECK(rms(again, 0, 0.01) > 0.05, "a second key does not fire the percussion");
        run(e, 12.0);
    }

    /* 4d. LFO -> VCF 2, FREQ MOD (LFO / ENV / DUAL), CC 1-7 */
    {
        plain(e, 1); set(e, "Spread", 100); set(e, "VCF2 Cutoff", 45); set(e, "LFO Speed", 55);   /* about 2.2 Hz */
        on(e, 45); Buf still = run(e, 3.0);
        set(e, "VCF2 Mod", 80);
        Buf moving = run(e, 3.0); off(e, 45); run(e, 1.0);
        auto swing = [](const Buf &a) { double lo = 1e9, hi = 0; for (double t = 0.5; t < 2.9; t += 0.05) { double r = rms(a, t, t + 0.05); lo = std::min(lo, r); hi = std::max(hi, r); } return hi / (lo + 1e-9); };
        std::printf("  LFO on VCF 2: level swing %.2f -> %.2f\n", swing(still), swing(moving));
        CHECK(swing(moving) > 2 && swing(still) < 1.3, "LFO does not move VCF 2");
        for (int sh = 0; sh < 8; sh++) { set(e, "LFO Shape", sh); set(e, "LFO Slew", sh * 15); on(e, 45); CHECK(rms(run(e, 1.0)) > 1e-3, "LFO shape %d silent", sh); off(e, 45); run(e, 0.5); }
        CHECK(display(e, "LFO Shape") == "SEQ", "LFO shape name");

        plain(e, 0); set(e, "Freq Mod", 50); set(e, "Freq Mod Source", 1);   /* ENV: one octave up while the key is held (AR) */
        double c = cents(freq(note(e, 57, 1.0), 440), 440);
        CHECK(std::fabs(c) < 10 && display(e, "Freq Mod Source") == "ENV", "FREQ MOD from the envelope: %.1f cents", c);
        set(e, "Freq Mod Source", 0); set(e, "LFO Speed", 0); set(e, "LFO Shape", 3);   /* LFO: a slow ramp bends the pitch */
        on(e, 57); Buf b1 = run(e, 0.5), b2 = run(e, 2.0); off(e, 57); run(e, 1.0);
        double fa = 0, fb = 0, ga = 0, gb = 0;
        for (double f = 100; f < 500; f *= 1.002) { double g = goertzel(b1, f, 0.1); if (g > ga) { ga = g; fa = f; } g = goertzel(Buf(b2.end() - 22050, b2.end()), f, 0.1); if (g > gb) { gb = g; fb = f; } }
        CHECK(std::fabs(std::log(fb / fa)) > 0.01, "FREQ MOD from the LFO: %.1f Hz -> %.1f Hz", fa, fb);
        set(e, "Freq Mod Source", 2);
        CHECK(display(e, "Freq Mod Source") == "DUAL" && rms(note(e, 57, 0.5)) > 0.01, "FREQ MOD DUAL");

        plain(e, 0);                                    /* CC 2 adds to SPREAD: partial 3 appears */
        Buf c0 = note(e, 57, 1.0);
        send(e, 0xb0, 2, 127);
        Buf c1 = note(e, 57, 1.0);
        CHECK(goertzel(c1, 440) > 10 * goertzel(c0, 440), "CC 2 does not add to spread");
        send(e, 0xb0, 2, 0); send(e, 0xb0, 7, 0);
        set(e, "Volume", 20); set(e, "Spread", 100);
        Buf v0 = note(e, 57, 0.5);
        send(e, 0xb0, 7, 127);
        Buf v1 = note(e, 57, 0.5);
        CHECK(rms(v1) > 5 * rms(v0), "CC 7 does not add to volume");
        for (int k = 1; k <= 7; k++) send(e, 0xb0, k, 0);
    }

    /* 4e. spring reverb: a tail after the VCA has closed, none with PRE VCA; feedback stays bounded and dies */
    {
        plain(e, 1); set(e, "Env Mode", 1); set(e, "Decay", 25);
        on(e, 57); Buf dry = run(e, 1.5); off(e, 57); run(e, 1.0);
        set(e, "Rev Level", 80);
        on(e, 57); Buf wet = run(e, 1.5); off(e, 57); Buf rest = run(e, 12.0);
        std::printf("  spring: 0.5-1 s after the hit %.1f dB (dry %.1f dB), 11 s later %.1f dB\n", db(rms(wet, 0.5, 1.0)), db(rms(dry, 0.5, 1.0)), db(rms(rest, 11.0)));
        CHECK(rms(wet, 0.5, 1.0) > 1e-3 && rms(dry, 0.5, 1.0) < 1e-6, "no reverb tail");
        CHECK(peak(Buf(rest.end() - 4410, rest.end())) < 1e-4, "reverb never ends");
        set(e, "Rev Pre VCA", 1);
        on(e, 57); Buf pre = run(e, 1.5); off(e, 57); run(e, 6.0);
        CHECK(rms(pre, 0, 0.05) > 0.01 && rms(pre, 0.5, 1.0) < 1e-6, "PRE VCA: the envelope does not cut the tail");
        set(e, "Rev Pre VCA", 0); set(e, "Env Mode", 0); set(e, "In", 100); set(e, "Rev Level", 100); set(e, "Rev Input", 100); set(e, "VCF1 Res", 80);
        on(e, 45); Buf fb = run(e, 6.0); off(e, 45); Buf after = run(e, 15.0);
        std::printf("  feedback full up: peak %.3f, 15 s after release %.1f dB\n", peak(fb), db(rms(after, 14.5)));
        CHECK(peak(fb) < 0.99 && rms(fb, 5.0) > 0.01, "feedback explodes or is silent");
        CHECK(rms(after, 14.5) < 1e-4, "feedback does not die with the key released");
    }

    /* 4f. poly: every key its own VCO; releasing one of several removes it; more than 8 keys */
    {
        plain(e, 0); set(e, "Mode", 1); set(e, "Decay", 70);
        on(e, 57); on(e, 61); on(e, 64);
        Buf ch3 = run(e, 1.0);
        off(e, 61);
        Buf ch2 = run(e, 1.0);
        off(e, 57); off(e, 64);
        Buf rel = run(e, 0.2); run(e, 12.0);
        double a = goertzel(ch3, 220), cs = goertzel(ch3, 277.18), ee = goertzel(ch3, 329.63);
        std::printf("  poly: A %.1f dB, C# %.1f dB, E %.1f dB; C# 0.8 s after its release %.1f dB\n", db(a), db(cs), db(ee), db(goertzel(ch2, 277.18, 0.8)));
        CHECK(a > 0.01 && cs > 0.5 * a && ee > 0.5 * a, "poly does not play three keys");
        CHECK(goertzel(ch2, 277.18, 0.8) < 0.05 * cs && goertzel(ch2, 220, 0.3) > 0.5 * a, "poly: a released key keeps sounding among held ones");
        CHECK(goertzel(rel, 220, 0.0) > 0.1 * a && goertzel(rel, 329.63, 0.0) > 0.1 * a, "poly: the last keys do not ring through the release");
        for (int k = 0; k < 10; k++) on(e, 48 + 3 * k);
        Buf many = run(e, 0.5);
        CHECK(rms(many) > 0.01 && peak(many) < 0.99, "poly with 10 keys");
        for (int k = 0; k < 10; k++) off(e, 48 + 3 * k);
        CHECK(peak(run(e, 12.0)) > 0 && peak(run(e, 0.2)) == 0, "poly does not fall silent");
        set(e, "Mode", 0);
        double c = cents(freq(note(e, 69, 1.0), 440), 440);
        CHECK(std::fabs(c) < 6 && display(e, "Mode") == "MONO", "back in mono: %.1f cents", c);
    }

    /* 4g. sequencer. A4 held, 1/16 at 120 BPM = 5512.5 samples a step, short AD blips */
    {
        auto onsets = [](const Buf &a) { std::vector<int> v; int quiet = 1000; for (int i = 0; i < (int)a.size(); i++) { if (std::fabs(a[i]) > 0.02) { if (quiet > 800) v.push_back(i); quiet = 0; } else quiet++; } return v; };
        plain(e, 0); set(e, "Env Mode", 1); set(e, "Decay", 15); set(e, "Seq Start", 1);
        set(e, "Step 2", 12); set(e, "Step 3", 7); set(e, "Gate 4", 0);
        /* own clock (no transport): starts with the key at step 1 */
        on(e, 69); Buf a = run(e, 1.1); off(e, 69); Buf q = run(e, 1.0);
        std::vector<int> o = onsets(a);
        std::printf("  seq, own clock: %d onsets at", (int)o.size()); for (int x : o) std::printf(" %d", x); std::printf("\n");
        CHECK(o.size() == 8 && o[0] < 40 && std::abs(o[1] - 5513) < 40 && std::abs(o[3] - 4 * 5513) < 60, "sequencer steps wrong (step 4 is a rest)");
        CHECK(std::fabs(cents(freq(Buf(a.begin() + 5600, a.begin() + 7600), 880), 880)) < 15, "step 2 is not an octave up");
        CHECK(onsets(q).empty(), "sequencer runs on without a key");
        /* host clock: song at beat 0.1 (inside step 1): the key sounds at once, step 2 comes on the grid */
        g_time = g_play = true; g_ppq = 0.1; g_tempo = 120;
        on(e, 69); Buf b = run(e, 1.0); off(e, 69); run(e, 0.5);
        o = onsets(b);
        std::printf("  seq, host clock: %d onsets at", (int)o.size()); for (int x : o) std::printf(" %d", x); std::printf("\n");
        CHECK(o.size() >= 6 && o[0] < 40 && std::abs(o[1] - 3308) < 40 && std::abs(o[2] - (3308 + 5513)) < 40, "sequencer is not locked to the song position");
        CHECK(std::fabs(cents(freq(Buf(b.begin() + 3400, b.begin() + 5400), 880), 880)) < 15, "host clock: step 2 is not an octave up");
        /* a key exactly on the grid plays that step once; 1/8 halves the rate; tempo follows the host */
        g_ppq = 2.0; set(e, "Seq Clock", 1); g_tempo = 150;
        on(e, 69); Buf c = run(e, 1.0); off(e, 69); run(e, 0.5);
        o = onsets(c);
        CHECK(o.size() >= 4 && o[0] < 40 && std::abs(o[1] - 8820) < 40, "1/8 at 150 BPM: %d onsets, second at %d", (int)o.size(), o.size() > 1 ? o[1] : -1);
        /* the transport stops: the key keeps the sequence going on its own clock */
        g_play = false;
        on(e, 69); Buf d = run(e, 1.0); off(e, 69); run(e, 0.5);
        CHECK(onsets(d).size() >= 4, "sequencer stops with the transport");
        g_time = false; set(e, "Seq Start", 0);
        double cf = cents(freq(note(e, 69, 1.0), 440), 440);
        CHECK(std::fabs(cf) < 6, "sequencer off: %.1f cents", cf);
    }

    /* 5. filters */
    {
        plain(e, 1);
        Buf open = note(e, 45, 1.0);
        set(e, "VCF1 Cutoff", 30);
        Buf c1 = note(e, 45, 1.0);
        set(e, "VCF1 Cutoff", 100); set(e, "VCF2 Cutoff", 30);
        Buf c2 = note(e, 45, 1.0);
        CHECK(goertzel(c1, 2200) < 0.05 * goertzel(open, 2200) && goertzel(c2, 2200) < 0.05 * goertzel(open, 2200), "a closed filter does not darken");
        set(e, "VCF2 Cutoff", 100); set(e, "VCF1 Type", 1); set(e, "VCF1 Cutoff", 70);
        Buf hp = note(e, 45, 1.0);
        CHECK(goertzel(hp, 110) < 0.05 * goertzel(open, 110) && goertzel(hp, 5500) > 0.5 * goertzel(open, 5500), "HP does not remove the fundamental");
        CHECK(display(e, "VCF1 Type") == "HP", "VCF1 Type display");
        set(e, "VCF1 Type", 0); set(e, "VCF1 Cutoff", 20); set(e, "VCF1 Mod", 80); set(e, "Env Mode", 1); set(e, "Decay", 50);
        Buf sweep = note(e, 45, 1.5);
        CHECK(rms(sweep, 0, 0.1) > 5 * rms(sweep, 1.0, 1.4), "envelope does not open VCF 1");
    }

    /* 6. envelope: AR holds and releases, AD decays under the held key; mono key handling */
    {
        plain(e, 0); set(e, "Decay", 40); set(e, "Spread", 100);   /* full spread: a steady level, no beating */
        on(e, 60); Buf hold = run(e, 2.0); off(e, 60); Buf tail = run(e, 3.0);
        CHECK(rms(hold, 1.5) > 0.5 * rms(hold, 0.2, 0.5), "AR does not hold");
        CHECK(rms(tail, 0, 0.05) > 0.1 * rms(hold, 1.5) && peak(Buf(tail.end() - 4410, tail.end())) < 1e-4, "AR release wrong");
        set(e, "Env Mode", 1);
        on(e, 60); Buf ad = run(e, 3.0); off(e, 60);
        CHECK(rms(ad, 0, 0.05) > 0.05 && peak(Buf(ad.end() - 4410, ad.end())) < 1e-4, "AD does not decay under a held key");
        set(e, "Env Mode", 0); set(e, "Attack", 70);
        on(e, 60); Buf at = run(e, 1.0); off(e, 60); run(e, 2.0);
        CHECK(rms(at, 0, 0.05) < 0.3 * rms(at, 0.9, 1.0), "attack is not slow");
        set(e, "Attack", 0); set(e, "Spread", 0);
        on(e, 57); run(e, 0.2); on(e, 69); double hi = freq(run(e, 0.6), 440); off(e, 69); double lo = freq(run(e, 0.6), 220); off(e, 57);
        CHECK(std::fabs(cents(hi, 440)) < 8 && std::fabs(cents(lo, 220)) < 8, "mono: newest key, then back to the held one");
        CHECK(peak(run(e, 3.0)) > 0 && peak(run(e, 0.2)) == 0, "does not fall silent after the last key");
    }

    /* 7. the 32 slots: program list, factory sounds, SAVE/LOAD, bank file shared by instances */
    {
        CHECK(e->numPrograms == 32, "numPrograms %d", e->numPrograms);
        CHECK(progname(e, 0) == "STRING SWARM" && progname(e, 31) == "32 (empty)", "program names: %s / %s", progname(e, 0).c_str(), progname(e, 31).c_str());
        for (int i = 0; i < 24; i++) {
            e->dispatcher(e, 2, 0, i, nullptr, 0);
            on(e, 48); Buf a = run(e, 2.0); off(e, 48); Buf tl = run(e, 12.0);
            std::printf("  preset %-15s 2 s %6.1f dBFS, peak %.3f\n", progname(e, i).c_str(), db(rms(a, 0)), peak(a));
            CHECK(rms(a, 0) > 3e-3 && peak(a) <= 0.981, "preset %s silent or too hot", progname(e, i).c_str());
            CHECK(e->dispatcher(e, 3, 0, 0, nullptr, 0) == i, "effGetProgram");
        }
        /* turning the PRESET knob only browses */
        e->dispatcher(e, 2, 0, 1, nullptr, 0);
        std::string model = display(e, "Model");
        set(e, "Preset", 28);
        CHECK(display(e, "Preset") == "28 (empty)" && display(e, "Model") == model, "browsing changed the sound");
        /* SAVE to 28, change, LOAD */
        set(e, "Spread", 37.5); set(e, "Model", 2); set(e, "VCF1 Type", 1);
        press(e, "Save");
        CHECK(display(e, "Preset") == "Rattler 28" && progname(e, 27) == "Rattler 28", "saved slot name: %s", display(e, "Preset").c_str());
        set(e, "Spread", 90); set(e, "Model", 0); set(e, "VCF1 Type", 0);
        press(e, "Load");
        CHECK(display(e, "Model") == "DRONE" && display(e, "VCF1 Type") == "HP" && std::fabs(e->getParameter(e, param(e, "Spread")) - 0.375f) < 1e-4, "LOAD does not restore the saved sound");
        CHECK(e->getParameter(e, param(e, "Load")) == 0 && e->getParameter(e, param(e, "Save")) == 0, "buttons do not read released");
        /* LOAD on an empty slot leaves the sound alone */
        set(e, "Preset", 30); press(e, "Load");
        CHECK(display(e, "Model") == "DRONE", "empty slot wiped the sound");
        /* a factory slot can be overwritten, a second instance sees the bank, the file has both */
        set(e, "Preset", 1); press(e, "Save");
        e->dispatcher(e, 4, 0, 0, (void *)"MY SWARM", 0);
        AEffect *e2 = mainf(master);
        CHECK(progname(e2, 0) == "MY SWARM" && progname(e2, 27) == "Rattler 28" && progname(e2, 1) == "ORGAN PARTIALS", "second instance: %s", progname(e2, 0).c_str());
        e2->dispatcher(e2, 2, 0, 27, nullptr, 0);
        CHECK(display(e2, "Model") == "DRONE" && display(e2, "Preset") == "Rattler 28", "second instance cannot load slot 28");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        int lines = 0; char line[4096];
        if (FILE *f = std::fopen(bank, "r")) { while (std::fgets(line, sizeof line, f)) lines++; std::fclose(f); }
        CHECK(lines == 2, "bank file has %d lines", lines);
    }

    /* 8. everything up: stays below 0 dBFS */
    for (int m = 0; m < 8; m++) {
        plain(e, m);
        for (const char *k : {"Voice Vol", "Volume", "VCF1 Res", "VCF2 Res", "Character", "Spread", "Perc Vol", "Perc", "Rev Level", "Rev Input", "In"}) set(e, k, 100);
        set(e, "VCF1 Cutoff", 60); set(e, "VCF2 Cutoff", 60);
        on(e, 40, 127); double pk = peak(run(e, 1.0)); off(e, 40); run(e, 1.0);
        std::printf("  hot (model %d): peak %.3f\n", m, pk);
        CHECK(pk < 1.0, "output above 0 dBFS");
    }

    /* 9. chunk */
    plain(e, 2); set(e, "VCF1 Type", 1); set(e, "Env Mode", 1); set(e, "VCF1 Cutoff", 37.5); set(e, "Freq", -7); set(e, "Preset", 5);
    {
        void *chunk = nullptr;
        intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
        std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
        for (const char *k : {"Model", "VCF1 Type", "Env Mode", "VCF1 Cutoff", "Freq", "Volume", "Preset"})
            CHECK(display(e2, k) == display(e, k), "chunk: %s %s vs %s", k, display(e2, k).c_str(), display(e, k).c_str());
        CHECK(std::fabs(e2->getParameter(e2, param(e2, "VCF1 Cutoff")) - 0.375f) < 1e-4, "chunk loses knob resolution");
        std::printf("  chunk %ld bytes: %s, %s, %s, %s\n", (long)len, display(e2, "Model").c_str(), display(e2, "VCF1 Cutoff").c_str(), display(e2, "Freq").c_str(), display(e2, "Preset").c_str());
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    }

    /* 10. stress: random parameters (LOAD/SAVE included), keys, bends, sample offsets; 48 and 96 kHz */
    {
        std::srand(808);
        g_time = g_play = true; g_ppq = 0;
        double pk = 0;
        for (float sr : {44100.0f, 48000.0f, 96000.0f}) {
            e->dispatcher(e, 10, 0, 0, nullptr, sr);
            for (int it = 0; it < 400; it++) {
                for (int j = 0; j < 6; j++) e->setParameter(e, std::rand() % e->numParams, (float)std::rand() / RAND_MAX);
                int r = std::rand() % 4, n = 20 + std::rand() % 90;
                if (r == 0) send(e, 0x90, n, 1 + std::rand() % 127, std::rand() % BS);
                else if (r == 1) send(e, 0x80, n, 0, std::rand() % BS);
                else if (r == 2) send(e, 0xe0, std::rand() % 128, std::rand() % 128, std::rand() % BS);
                pk = std::max(pk, peak(run(e, 0.03)));
            }
            send(e, 0xb0, 123, 0);
        }
        std::printf("  stress: peak %.3f\n", pk);
        CHECK(pk < 1.0, "stress run above 0 dBFS");
    }

    CHECK(!bad, "NaN, Inf or denormals in the output");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::remove(bank);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
