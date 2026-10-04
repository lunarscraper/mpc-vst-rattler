/* Offline x86 test of rattler_vst.cpp (test.sh builds it with ASan/UBSan): silence without a
 * key, tuning (FREQ, pitch bend), SPREAD (harmonic partials, inharmonic drone), CHARACTER
 * (wavefolder), both filters (LP darkens, HP removes the fundamental), the envelope (AR holds
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

static intptr_t master(AEffect *, int32_t, int32_t, intptr_t, void *, float) { return 0; }
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
    else if (s == "Model") { hi = 2; }
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
        };
        for (auto &b : B) {
            plain(e, b.model); set(e, "Spread", b.spread); set(e, "Character", b.chr); set(e, "VCF1 Res", 50); set(e, "VCF1 Mod", 50);
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
        plain(e, 0);
        Buf a0 = note(e, 57, 1.0);
        set(e, "Character", 80);
        Buf a1 = note(e, 57, 1.0);
        std::printf("  fold: 3rd harmonic %.1f dB -> %.1f dB\n", db(goertzel(a0, 660)), db(goertzel(a1, 660)));
        CHECK(goertzel(a1, 660) + goertzel(a1, 1100) > 8 * (goertzel(a0, 660) + goertzel(a0, 1100)), "character adds no harmonics");
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
        plain(e, 0); set(e, "Decay", 40);
        on(e, 60); Buf hold = run(e, 2.0); off(e, 60); Buf tail = run(e, 3.0);
        CHECK(rms(hold, 1.5) > 0.5 * rms(hold, 0.2, 0.5), "AR does not hold");
        CHECK(rms(tail, 0, 0.05) > 0.1 * rms(hold, 1.5) && peak(Buf(tail.end() - 4410, tail.end())) < 1e-4, "AR release wrong");
        set(e, "Env Mode", 1);
        on(e, 60); Buf ad = run(e, 3.0); off(e, 60);
        CHECK(rms(ad, 0, 0.05) > 0.05 && peak(Buf(ad.end() - 4410, ad.end())) < 1e-4, "AD does not decay under a held key");
        set(e, "Env Mode", 0); set(e, "Attack", 70);
        on(e, 60); Buf at = run(e, 1.0); off(e, 60); run(e, 2.0);
        CHECK(rms(at, 0, 0.05) < 0.3 * rms(at, 0.9, 1.0), "attack is not slow");
        set(e, "Attack", 0);
        on(e, 57); run(e, 0.2); on(e, 69); double hi = freq(run(e, 0.6), 440); off(e, 69); double lo = freq(run(e, 0.6), 220); off(e, 57);
        CHECK(std::fabs(cents(hi, 440)) < 8 && std::fabs(cents(lo, 220)) < 8, "mono: newest key, then back to the held one");
        CHECK(peak(run(e, 3.0)) > 0 && peak(run(e, 0.2)) == 0, "does not fall silent after the last key");
    }

    /* 7. the 32 slots: program list, factory sounds, SAVE/LOAD, bank file shared by instances */
    {
        CHECK(e->numPrograms == 32, "numPrograms %d", e->numPrograms);
        CHECK(progname(e, 0) == "STRING SWARM" && progname(e, 31) == "32 (empty)", "program names: %s / %s", progname(e, 0).c_str(), progname(e, 31).c_str());
        for (int i = 0; i < 8; i++) {
            e->dispatcher(e, 2, 0, i, nullptr, 0);
            on(e, 48); Buf a = run(e, 2.0); off(e, 48); Buf tl = run(e, 12.0);
            std::printf("  preset %-15s 2 s %6.1f dBFS, peak %.3f\n", progname(e, i).c_str(), db(rms(a, 0)), peak(a));
            CHECK(rms(a, 0) > 3e-3 && peak(a) <= 0.981, "preset %s silent or too hot", progname(e, i).c_str());
            CHECK(e->dispatcher(e, 3, 0, 0, nullptr, 0) == i, "effGetProgram");
        }
        /* turning the PRESET knob only browses */
        e->dispatcher(e, 2, 0, 1, nullptr, 0);
        std::string model = display(e, "Model");
        set(e, "Preset", 20);
        CHECK(display(e, "Preset") == "20 (empty)" && display(e, "Model") == model, "browsing changed the sound");
        /* SAVE to 20, change, LOAD */
        set(e, "Spread", 37.5); set(e, "Model", 2); set(e, "VCF1 Type", 1);
        press(e, "Save");
        CHECK(display(e, "Preset") == "Rattler 20" && progname(e, 19) == "Rattler 20", "saved slot name: %s", display(e, "Preset").c_str());
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
        CHECK(progname(e2, 0) == "MY SWARM" && progname(e2, 19) == "Rattler 20" && progname(e2, 1) == "ORGAN PARTIALS", "second instance: %s", progname(e2, 0).c_str());
        e2->dispatcher(e2, 2, 0, 19, nullptr, 0);
        CHECK(display(e2, "Model") == "DRONE" && display(e2, "Preset") == "Rattler 20", "second instance cannot load slot 20");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        int lines = 0; char line[4096];
        if (FILE *f = std::fopen(bank, "r")) { while (std::fgets(line, sizeof line, f)) lines++; std::fclose(f); }
        CHECK(lines == 2, "bank file has %d lines", lines);
    }

    /* 8. everything up: stays below 0 dBFS */
    for (int m = 0; m < 3; m++) {
        plain(e, m);
        for (const char *k : {"Voice Vol", "Volume", "VCF1 Res", "VCF2 Res", "Character", "Spread"}) set(e, k, 100);
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
