/* =============================================================================
 * rattler_vst.cpp - Rattler: a paraphonic swarm synthesizer in the manner of the Eowave
 * Quadrantid Swarm as a VST2 instrument for the MPC OS plugin host (Force, MPC Live/One/X/Key),
 * armhf. The sound is rattler_core.h; this file is the plug-in around it: parameters, MIDI
 * (sample-accurate, mono: newest key, poly: one VCO per key, pitch bend +-2, CC 1-7 as on the
 * original), the 8-step sequencer on the host clock, the 32 preset slots with LOAD/SAVE (pattern
 * of mpc-vst-acid), the project chunk, and the dice (RANDOM / MUTATE / UNDO / DICE SEQ by style,
 * with locks; CC 20-23) plus the step chance of the sequencer.
 * MIT license (see ../LICENSE). "Eowave" and "Quadrantid Swarm" belong to their owners; no affiliation.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "rattler_core.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
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
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstTimeSigValid = 1 << 13 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
enum { kVstMidiType = 1 };

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum {
    MODEL, FREQ, SPREAD, CHARACTER, VOICE_VOL,
    F1_CUTOFF, F1_RES, F1_MOD, F1_TYPE, F2_CUTOFF, F2_RES,
    ATTACK, DECAY, ENV_MODE, VOLUME,
    SLOT, LOAD, SAVE,
    /* phase 2, appended */
    PERC, PERC_FREQ, PERC_VOL, LFO_SPEED, LFO_SHAPE, LFO_SLEW, F2_MOD, FM_DEPTH, FM_SRC,
    /* phase 3, appended */
    REV_IN, REV_LEVEL, REV_FB, REV_PRE, MODE, SEQ_RUN, SEQ_CLOCK,
    SEQ_P1, SEQ_P2, SEQ_P3, SEQ_P4, SEQ_P5, SEQ_P6, SEQ_P7, SEQ_P8,
    SEQ_G1, SEQ_G2, SEQ_G3, SEQ_G4, SEQ_G5, SEQ_G6, SEQ_G7, SEQ_G8,
    /* dice update, appended: triggers, the dice settings, the step chance */
    DICE, MUTATE, UNDO, DICE_SEQ, DICE_STYLE, DICE_AMOUNT, LOCK_FILTER, LOCK_ENV, LOCK_SEQ,
    SEQ_CHANCE, SEQ_DENSITY, NKEYS
};
static const char *const KEYS[NKEYS] = {
    "model", "freq", "spread", "character", "voice_vol",
    "f1_cutoff", "f1_res", "f1_mod", "f1_type", "f2_cutoff", "f2_res",
    "attack", "decay", "env_mode", "volume",
    "slot", "load", "save",
    "perc", "perc_freq", "perc_vol", "lfo_speed", "lfo_shape", "lfo_slew", "f2_mod", "fm_depth", "fm_src",
    "rev_in", "rev_level", "rev_fb", "rev_pre", "mode", "seq_run", "seq_clock",
    "seq_p1", "seq_p2", "seq_p3", "seq_p4", "seq_p5", "seq_p6", "seq_p7", "seq_p8",
    "seq_g1", "seq_g2", "seq_g3", "seq_g4", "seq_g5", "seq_g6", "seq_g7", "seq_g8",
    "dice", "mutate", "undo", "dice_seq", "dice_style", "dice_amount", "lock_filter", "lock_env", "lock_seq",
    "seq_chance", "seq_density",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours */
static bool is_trigger(int k) { return k == LOAD || k == SAVE || k == DICE || k == MUTATE || k == UNDO || k == DICE_SEQ; }
/* slot and the trigger buttons: never part of a chunk or a preset */
static bool is_bank_key(int i) { const int k = KEY_OF[i]; return k == SLOT || is_trigger(k); }
/* the dice settings are a way of working, not a sound: in the project chunk, never in a preset
 * slot (loading a preset keeps the style, the amount and the locks) */
static bool is_ui_key(int i) {
    const int k = KEY_OF[i];
    return k == DICE_STYLE || k == DICE_AMOUNT || k == LOCK_FILTER || k == LOCK_ENV || k == LOCK_SEQ || k == SEQ_DENSITY;
}
/* a parameter of the sound: what a preset, UNDO and the dice deal with */
static bool is_sound(int i) { return KEY_OF[i] >= 0 && !is_bank_key(i) && !is_ui_key(i); }

enum { SEG = 1024 };
struct MidiEv { int32_t frame; uint8_t d[3]; };

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};   /* LOAD/SAVE: the host currently reports them pressed */
    std::atomic<bool> dirty{true};
    std::atomic<int> cur{0};        /* selected preset slot, 0-based */
    rattler::Engine engine;
    rattler::Patch patch;
    float sr = 44100;
    MidiEv ev[256];
    int nev = 0;
    float cc[8] = {0};              /* CC 1..7, 0..1: added to the knob they belong to (not saved) */
    /* the sequencer (see "sequencer" below) */
    bool poly = false, seq_on = false, was_playing = false;
    int seq_pitch[8] = {0}, seq_i = 0, gate_off = 0;
    bool seq_gate[8] = {true, true, true, true, true, true, true, true};
    double seq_beats = 0.25, tempo = 120, to_next = 0;
    long long seq_abs = -1;
    uint8_t held[32];               /* held keys, oldest first */
    int nheld = 0;
    std::vector<uint8_t> chunk;
    /* dice (see "dice" below): requests from the skin or CC, run on the audio thread */
    std::atomic<int> req{0};
    float seq_chance = 1;
    uint32_t rs = 0x9e3779b9u;
    bool cc_trig[4] = {false, false, false, false};   /* CC 20..23 above the middle */
    enum { NHIST = 16 };
    float hist[NHIST][NPARAMS];
    int hpos = 0, hcount = 0;
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
/* a knob is used at full resolution (the integer range only names its ends), a switch as its index */
static float val_at(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    const float n = w->cache[i].load();
    if (p->nopts) return (float)norm_to_ui(p, n);
    return p->min + (p->max - p->min) * clamp01(n);
}
static float val(Plugin *w, int k) { return IDX[k] < 0 ? 0.0f : val_at(w, IDX[k]); }
static float pct(Plugin *w, int k) { return val(w, k) * 0.01f; }
static bool sw(Plugin *w, int k) { return val(w, k) > 0.5f; }
static void set_val(Plugin *w, int i, double v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}
static void start(Plugin *w, int k, double v) { set_val(w, IDX[k], v); }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* knob laws, shared by configure() and the value display */
static float sq(float x) { return x * x; }
static float law_time(float x, float lo, float ratio) { return lo * std::pow(ratio, x); }
static float law_cutoff_oct(float x) { return x * 9.8137812f; }          /* 20 Hz .. 18 kHz */
static float law_mod_oct(float x) { return 8.0f * x; }                    /* envelope -> VCF 1 */
static float law_perc_s(float x) { return 0.003f * std::pow(333.0f, x); }    /* 3 ms .. 1 s */
static float law_perc_hz(float x) { return 60.0f * std::pow(133.0f, x); }    /* 60 Hz .. 8 kHz */
static float law_lfo_hz(float x) { return 0.05f * std::pow(1000.0f, x); }    /* 0.05 .. 50 Hz */
/* a knob plus its controller (README: CC 1 LFO speed, 2 spread, 3 character, 4 perc, 5 attack,
 * 6 decay, 7 volume - the CC value is added to the knob) */
static float pcc(Plugin *w, int k, int cc) { return clamp01(pct(w, k) + w->cc[cc]); }

static void all_off(Plugin *w);
static void configure(Plugin *w) {
    rattler::Patch &p = w->patch;
    p.model = clampi((int)val(w, MODEL), 0, rattler::NMODELS - 1);
    p.semis = std::round(val(w, FREQ));
    p.spread = pcc(w, SPREAD, 2);
    p.character = pcc(w, CHARACTER, 3);
    p.voice = 2 * sq(pct(w, VOICE_VOL));
    p.f1_oct = law_cutoff_oct(pct(w, F1_CUTOFF));
    p.f1_res = pct(w, F1_RES);
    p.f1_mod = law_mod_oct(pct(w, F1_MOD));
    p.f1_hp = sw(w, F1_TYPE);
    p.f2_oct = law_cutoff_oct(pct(w, F2_CUTOFF));
    p.f2_res = pct(w, F2_RES);
    p.att = law_time(pcc(w, ATTACK, 5), 0.001f, 5000);
    p.dec = law_time(pcc(w, DECAY, 6), 0.005f, 2000);
    p.ad = sw(w, ENV_MODE);
    p.volume = sq(pcc(w, VOLUME, 7));
    p.perc_dec = law_perc_s(pcc(w, PERC, 4));
    p.perc_hz = law_perc_hz(pct(w, PERC_FREQ));
    p.perc = 2 * sq(pct(w, PERC_VOL));
    p.lfo_hz = law_lfo_hz(pcc(w, LFO_SPEED, 1));
    p.lfo_shape = clampi((int)val(w, LFO_SHAPE), 0, rattler::NLFO - 1);
    p.lfo_slew = sq(pct(w, LFO_SLEW));
    p.f2_mod = 4 * pct(w, F2_MOD);
    p.fm = 4 * sq(pct(w, FM_DEPTH));
    p.fm_src = clampi((int)val(w, FM_SRC), 0, 2);
    p.rev_in = 1.5f * sq(pct(w, REV_IN));
    p.rev_level = 1.5f * sq(pct(w, REV_LEVEL));
    p.rev_fb = 1.5f * pct(w, REV_FB);
    p.rev_pre = sw(w, REV_PRE);
    p.poly = sw(w, MODE);
    static const double BEATS[6] = {1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};   /* 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 */
    w->seq_beats = BEATS[clampi((int)val(w, SEQ_CLOCK), 0, 5)];
    for (int i = 0; i < 8; i++) {
        w->seq_pitch[i] = (int)std::lround(val(w, SEQ_P1 + i));
        w->seq_gate[i] = sw(w, SEQ_G1 + i);
        p.seq[i] = w->seq_pitch[i] / 24.0f;
    }
    w->seq_chance = pct(w, SEQ_CHANCE);
    const bool seq_on = sw(w, SEQ_RUN) && !p.poly;   /* the sequencer plays the mono voice */
    if (p.poly != w->poly || seq_on != w->seq_on) { all_off(w); w->poly = p.poly; w->seq_on = seq_on; }
    w->engine.set_patch(p);
}

/* the start patch: a slowly beating string swarm through a half-open filter */
static void start_values(Plugin *w) {
    start(w, MODEL, rattler::MODEL_STRINGS); start(w, FREQ, 0); start(w, SPREAD, 22); start(w, CHARACTER, 0);
    start(w, VOICE_VOL, 70);
    start(w, F1_CUTOFF, 62); start(w, F1_RES, 20); start(w, F1_MOD, 25); start(w, F1_TYPE, 0);
    start(w, F2_CUTOFF, 85); start(w, F2_RES, 0);
    start(w, ATTACK, 30); start(w, DECAY, 60); start(w, ENV_MODE, 0);
    start(w, VOLUME, 80);
    start(w, PERC, 40); start(w, PERC_FREQ, 50); start(w, PERC_VOL, 0);
    start(w, LFO_SPEED, 40); start(w, LFO_SHAPE, 0); start(w, LFO_SLEW, 0); start(w, F2_MOD, 0);
    start(w, FM_DEPTH, 0); start(w, FM_SRC, 0);
    start(w, REV_IN, 70); start(w, REV_LEVEL, 0); start(w, REV_FB, 0); start(w, REV_PRE, 0);
    start(w, MODE, 0); start(w, SEQ_RUN, 0); start(w, SEQ_CLOCK, 3);
    for (int i = 0; i < 8; i++) { start(w, SEQ_P1 + i, 0); start(w, SEQ_G1 + i, 1); }
    start(w, SEQ_CHANCE, 100);
}
/* the dice settings at a new instance (a preset never changes them) */
static void ui_defaults(Plugin *w) {
    start(w, DICE_STYLE, 0); start(w, DICE_AMOUNT, 30);
    start(w, LOCK_FILTER, 0); start(w, LOCK_ENV, 0); start(w, LOCK_SEQ, 0);
    start(w, SEQ_DENSITY, 40);
}

/* ---- state as text: "key=value;" for every sound parameter. Used for the project chunk
 * and for the preset slots alike; restored by key, so parameters of later phases keep old
 * projects and presets loading (missing keys stay at their start values). The chunk also
 * carries the dice settings (with_ui), a preset slot doesn't. ------------------------------ */
static std::string build_state(Plugin *w, bool with_ui) {
    std::string t;
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (is_bank_key(i) || (!with_ui && is_ui_key(i))) continue;
        std::snprintf(buf, sizeof buf, "%s=%.3f;", PARAMS[i].key, (double)val_at(w, i));
        t += buf;
    }
    return t;
}
static void apply_state(Plugin *w, const std::string &t, bool with_slot) {
    for (size_t pos = 0; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq);
        if (key == "prog") {   /* which slot a project had selected; never loads the slot */
            const int v = std::atoi(kv.c_str() + eq + 1);
            if (with_slot && v >= 0 && v < 32) w->cur.store(v);
            continue;
        }
        const int i = param_index(key.c_str());
        if (i >= 0 && !is_bank_key(i) && (with_slot || !is_ui_key(i))) set_val(w, i, std::atof(kv.c_str() + eq + 1));
    }
    w->dirty.store(true);
}

/* ---------------------------------------------------------------------------
 * Preset bank (pattern of mpc-vst-acid): NSLOTS slots shared by every Rattler instance and
 * every project, kept in one text file on the SD card (one line per saved slot:
 * "index<TAB>name<TAB>state"). The slots are also the plugin's VST programs, so the host's
 * PRESET list shows and selects them; the PRESET knob + LOAD/SAVE buttons on the PRESET tab
 * do the same from the skin. The first slots come with factory sounds until SAVE overwrites
 * them (a factory sound is never written to the file).
 * ------------------------------------------------------------------------- */
#define NSLOTS 32
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

static const struct { const char *name, *state; } FACTORY[] = {
    {"STRING SWARM", "model=1;freq=0;spread=22;character=0;voice_vol=70;f1_cutoff=62;f1_res=20;f1_mod=25;f1_type=0;f2_cutoff=85;f2_res=0;attack=30;decay=60;env_mode=0;volume=80;"},
    {"ORGAN PARTIALS", "model=0;freq=0;spread=100;character=0;voice_vol=65;f1_cutoff=85;f1_res=0;f1_mod=0;f1_type=0;f2_cutoff=95;f2_res=0;attack=5;decay=35;env_mode=0;volume=80;"},
    {"FOLDED ORGAN", "model=0;freq=0;spread=12;character=55;voice_vol=70;f1_cutoff=66;f1_res=30;f1_mod=22;f1_type=0;f2_cutoff=85;f2_res=10;attack=20;decay=55;env_mode=0;volume=80;"},
    {"DRONE CLUSTER", "model=2;freq=-12;spread=50;character=20;voice_vol=75;f1_cutoff=58;f1_res=40;f1_mod=12;f1_type=0;f2_cutoff=72;f2_res=30;attack=72;decay=80;env_mode=0;volume=80;"},
    {"GLASS BELL", "model=2;freq=0;spread=100;character=0;voice_vol=65;f1_cutoff=92;f1_res=0;f1_mod=0;f1_type=0;f2_cutoff=95;f2_res=0;attack=0;decay=70;env_mode=1;volume=80;"},
    {"METAL PLUCK", "model=2;freq=0;spread=78;character=45;voice_vol=75;f1_cutoff=38;f1_res=45;f1_mod=65;f1_type=0;f2_cutoff=88;f2_res=15;attack=0;decay=44;env_mode=1;volume=80;"},
    {"THIN CLUSTER", "model=1;freq=0;spread=60;character=65;voice_vol=75;f1_cutoff=60;f1_res=35;f1_mod=25;f1_type=1;f2_cutoff=88;f2_res=20;attack=0;decay=52;env_mode=1;volume=85;"},
    {"DEEP BEATING", "model=0;freq=-12;spread=8;character=30;voice_vol=75;f1_cutoff=50;f1_res=55;f1_mod=15;f1_type=0;f2_cutoff=65;f2_res=40;attack=62;decay=76;env_mode=0;volume=85;"},
    /* phase 2: keys the state leaves out stay at their start values */
    {"PERC TICK", "voice_vol=0;perc=22;perc_freq=72;perc_vol=85;f1_cutoff=100;f1_res=0;f1_mod=0;f2_cutoff=100;attack=0;decay=40;env_mode=1;volume=85;"},
    {"PERC TOM", "voice_vol=0;perc=58;perc_freq=14;perc_vol=90;f1_cutoff=70;f1_res=10;f1_mod=20;f2_cutoff=90;attack=0;decay=58;env_mode=1;volume=90;"},
    {"PERC HAT", "voice_vol=0;perc=34;perc_freq=96;perc_vol=85;f1_cutoff=70;f1_res=20;f1_mod=0;f1_type=1;f2_cutoff=100;attack=0;decay=42;env_mode=1;volume=85;"},
    {"METAL HIT", "model=4;spread=64;character=55;voice_vol=70;perc=30;perc_freq=60;perc_vol=70;f1_cutoff=55;f1_res=30;f1_mod=45;f2_cutoff=95;attack=0;decay=55;env_mode=1;volume=85;"},
    {"NOISE SNARE", "model=7;freq=12;spread=40;character=15;voice_vol=60;perc=45;perc_freq=30;perc_vol=80;f1_cutoff=85;f1_res=10;f1_mod=10;f2_cutoff=95;attack=0;decay=46;env_mode=1;volume=85;"},
    {"CHIP SWARM", "model=5;spread=10;character=80;voice_vol=65;f1_cutoff=75;f1_res=15;f1_mod=15;f2_cutoff=90;attack=5;decay=45;env_mode=0;volume=75;"},
    {"GRAIN CLOUD", "model=6;freq=12;spread=45;character=60;voice_vol=80;f1_cutoff=80;f1_res=20;f1_mod=0;f2_cutoff=70;f2_res=35;f2_mod=40;lfo_speed=30;lfo_shape=5;attack=60;decay=75;env_mode=0;volume=85;"},
    /* phase 3 */
    {"SPRING PERC", "voice_vol=0;perc=30;perc_freq=55;perc_vol=85;f1_cutoff=100;f1_res=0;f1_mod=0;f2_cutoff=100;attack=0;decay=45;env_mode=1;rev_in=80;rev_level=70;volume=85;"},
    {"SPRING TOM", "voice_vol=0;perc=55;perc_freq=18;perc_vol=90;f1_cutoff=75;f1_res=10;f1_mod=15;f2_cutoff=90;attack=0;decay=56;env_mode=1;rev_in=75;rev_level=60;volume=90;"},
    {"FEEDBACK DRONE", "model=2;freq=-12;spread=35;character=25;voice_vol=60;f1_cutoff=55;f1_res=45;f1_mod=10;f2_cutoff=60;f2_res=40;f2_mod=30;lfo_speed=20;lfo_shape=1;attack=70;decay=80;env_mode=0;rev_in=80;rev_level=55;rev_fb=35;volume=60;"},
    {"POLY ORGAN", "model=0;mode=1;character=20;voice_vol=70;f1_cutoff=80;f1_res=10;f1_mod=10;f2_cutoff=90;attack=5;decay=45;env_mode=0;rev_in=60;rev_level=35;volume=80;"},
    {"POLY STRINGS", "model=1;mode=1;voice_vol=65;f1_cutoff=62;f1_res=20;f1_mod=20;f2_cutoff=85;attack=45;decay=66;env_mode=0;rev_in=70;rev_level=50;volume=80;"},
    {"SEQ METAL", "model=4;spread=40;character=40;voice_vol=70;f1_cutoff=50;f1_res=35;f1_mod=45;f2_cutoff=90;attack=0;decay=40;env_mode=1;seq_run=1;seq_clock=3;seq_p1=0;seq_p2=12;seq_p3=0;seq_p4=7;seq_p5=0;seq_p6=12;seq_p7=3;seq_p8=10;seq_g5=0;rev_in=60;rev_level=35;volume=85;"},
    {"SEQ PERC", "voice_vol=0;perc=28;perc_freq=45;perc_vol=85;f1_cutoff=60;f1_res=30;f1_mod=0;f2_cutoff=55;f2_res=45;f2_mod=70;lfo_speed=78;lfo_shape=7;attack=0;decay=40;env_mode=1;seq_run=1;seq_clock=3;seq_p1=-12;seq_p2=7;seq_p3=0;seq_p4=19;seq_p5=-5;seq_p6=12;seq_p7=3;seq_p8=24;seq_g3=0;seq_g7=0;rev_in=70;rev_level=45;volume=85;"},
    {"SEQ STRINGS", "model=1;spread=15;voice_vol=70;f1_cutoff=45;f1_res=40;f1_mod=50;f2_cutoff=85;attack=0;decay=44;env_mode=0;seq_run=1;seq_clock=3;seq_p1=0;seq_p2=0;seq_p3=12;seq_p4=0;seq_p5=7;seq_p6=0;seq_p7=10;seq_p8=12;rev_in=60;rev_level=30;volume=80;"},
    {"REED RANDOM", "model=3;freq=-12;spread=18;character=25;voice_vol=70;f1_cutoff=70;f1_res=25;f1_mod=10;f2_cutoff=50;f2_res=60;f2_mod=55;lfo_speed=62;lfo_shape=4;lfo_slew=20;attack=20;decay=60;env_mode=0;volume=80;"},
    /* dice update: industrial, slots 25-32 - single hits and sparse sequences */
    {"RUST HIT", "model=4;spread=85;character=80;voice_vol=70;f1_type=1;f1_cutoff=45;f1_res=60;f1_mod=30;f2_cutoff=82;f2_res=30;attack=0;decay=36;env_mode=1;perc=18;perc_freq=80;perc_vol=60;rev_in=75;rev_level=30;rev_fb=20;volume=80;"},
    {"SHEET METAL", "model=4;spread=100;character=100;voice_vol=70;f1_cutoff=55;f1_res=70;f1_mod=40;f2_cutoff=90;f2_res=20;attack=0;decay=50;env_mode=1;rev_in=80;rev_level=45;volume=75;"},
    {"SERVO", "model=5;spread=70;character=100;voice_vol=70;f1_cutoff=50;f1_res=75;f1_mod=50;f2_cutoff=85;f2_res=25;attack=0;decay=26;env_mode=1;fm_depth=30;fm_src=1;volume=78;"},
    {"STATIC BURST", "model=7;freq=12;spread=90;character=70;voice_vol=70;f1_type=1;f1_cutoff=60;f1_res=50;f1_mod=10;f2_cutoff=82;f2_res=60;f2_mod=50;lfo_speed=72;lfo_shape=4;attack=0;decay=30;env_mode=1;rev_in=70;rev_level=25;volume=78;"},
    {"PIPE KNOCK", "model=2;spread=100;character=60;voice_vol=55;perc=30;perc_freq=25;perc_vol=85;f1_cutoff=40;f1_res=80;f1_mod=55;f2_cutoff=88;f2_res=10;attack=0;decay=40;env_mode=1;rev_in=80;rev_level=50;rev_pre=1;volume=82;"},
    {"GEAR GRIND", "model=3;freq=-12;spread=60;character=90;voice_vol=70;f1_cutoff=45;f1_res=65;f1_mod=30;f2_cutoff=62;f2_res=40;lfo_speed=75;lfo_shape=6;fm_depth=25;fm_src=0;attack=0;decay=30;env_mode=0;volume=78;"},
    {"FACTORY HALL", "model=6;spread=70;character=90;voice_vol=75;f1_type=1;f1_cutoff=40;f1_res=40;f1_mod=20;f2_cutoff=85;f2_res=20;attack=0;decay=55;env_mode=1;rev_in=85;rev_level=60;rev_fb=45;volume=70;"},
    {"SCRAP SEQ", "model=4;spread=75;character=70;voice_vol=65;perc=20;perc_freq=70;perc_vol=60;f1_cutoff=42;f1_res=60;f1_mod=50;f2_cutoff=88;f2_res=20;attack=0;decay=32;env_mode=1;seq_run=1;seq_clock=3;seq_chance=60;seq_p1=0;seq_p2=13;seq_p3=0;seq_p4=-12;seq_p5=6;seq_p6=0;seq_p7=1;seq_p8=12;seq_g2=0;seq_g3=0;seq_g5=0;seq_g6=0;seq_g8=0;rev_in=70;rev_level=35;volume=80;"},
};
enum { NFACTORY = (int)(sizeof FACTORY / sizeof FACTORY[0]) };
static_assert(NFACTORY <= NSLOTS, "more factory sounds than slots");

/* Next to the plugin's own folder rather than inside it, so reinstalling the plugin folder
 * doesn't take the presets with it; inside it if the parent can't be written. No dladdr:
 * /proc/self/maps has the path. RATTLER_PRESETS overrides the place (offline test). */
static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/rattler.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("RATTLER_PRESETS")) g_bank_path = env;
    else {
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/rattler_presets.txt";
        cand[1] = dir + "/rattler_presets.txt";
        for (const std::string &c : cand)             /* an existing file wins ... */
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)   /* ... else the first place we may write */
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::fclose(f);
    }
}
static bool bank_write_locked() {   /* whole file, via a temp file so a power cut can't leave half a bank */
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string slot_title(int i) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    char buf[32];
    if (g_slot_chunk[i].empty()) {
        if (i < NFACTORY) return FACTORY[i].name;
        std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1);
        return buf;
    }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "Rattler %02d", i + 1);
    return buf;
}
static void slot_load(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    std::string c;
    { std::lock_guard<std::mutex> lk(g_bank_lock); c = g_slot_chunk[n]; }
    if (c.empty() && n < NFACTORY) c = FACTORY[n].state;
    if (c.empty()) return;              /* an empty slot leaves the current sound alone */
    start_values(w);
    apply_state(w, c, false);
}
static bool slot_save(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    const std::string c = build_state(w, false);
    std::lock_guard<std::mutex> lk(g_bank_lock);
    g_slot_chunk[n] = c;
    return bank_write_locked();
}

/* ---- dice ------------------------------------------------------------------------
 * The quick way to a sound, as on the original: pick a starting point, then turn the filters.
 *   RANDOM    a new sound inside the ranges of the DICE STYLE (INDUSTRIAL, METAL, NOISE, PERC,
 *             DRONE, ANY): model, spread, character, filters, envelope, percussion, LFO,
 *             freq mod, reverb and - unless LOCK SEQ - the eight steps
 *   MUTATE    a variation of the current sound, MUTATE AMOUNT says how far
 *   DICE SEQ  only the eight steps: pitches from the style's intervals, gates by SEQ DENSITY
 *   UNDO      back one step (16 deep): every RANDOM, MUTATE and DICE SEQ can be taken back
 * LOCK FILTER / LOCK ENV / LOCK SEQ keep those groups. Never touched: FREQ, VOLUME, MODE,
 * SEQ START, SEQ CLOCK, SEQ CHANCE - the tuning, the level and the running sequence stay put.
 * After every roll a safety pass keeps the result audible (a source in the mixer, filters that
 * don't close each other off). The skin buttons and CC 20-23 only file a request; it runs on
 * the audio thread at the start of a block (or at the CC's frame), without allocating.
 * ---------------------------------------------------------------------------- */
enum { REQ_DICE = 1, REQ_MUTATE = 2, REQ_UNDO = 4, REQ_SEQ = 8 };
enum { ST_INDUSTRIAL, ST_METAL, ST_NOISE, ST_PERC, ST_DRONE, ST_ANY, NSTYLES };

static float urnd(Plugin *w) {   /* 0 .. 1, xorshift32 */
    uint32_t x = w->rs;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    w->rs = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}
static float rr(Plugin *w, float lo, float hi) { return lo + (hi - lo) * urnd(w); }
static bool chance(Plugin *w, float p) { return urnd(w) < p; }
static int pick(Plugin *w, const int *a, int n) { return a[clampi((int)(urnd(w) * n), 0, n - 1)]; }
#define PICK(w, ...) ([&] { static const int A_[] = {__VA_ARGS__}; return pick(w, A_, (int)(sizeof A_ / sizeof A_[0])); }())

static bool in_filter(int k) { return (k >= F1_CUTOFF && k <= F2_RES) || k == F2_MOD; }
static bool in_env(int k) { return k == ATTACK || k == DECAY || k == ENV_MODE; }
static bool in_seq(int k) { return k >= SEQ_P1 && k <= SEQ_G8; }
static bool never_diced(int k) {
    return k == FREQ || k == VOLUME || k == MODE || k == SEQ_RUN || k == SEQ_CLOCK || k == SEQ_CHANCE;
}
static bool locked(Plugin *w, int k) {
    if (never_diced(k)) return true;
    if (in_filter(k)) return sw(w, LOCK_FILTER);
    if (in_env(k)) return sw(w, LOCK_ENV);
    if (in_seq(k)) return sw(w, LOCK_SEQ);
    return false;
}
static void put(Plugin *w, int k, double v) { if (IDX[k] >= 0 && !locked(w, k)) set_val(w, IDX[k], v); }

static void hist_push(Plugin *w) {
    for (int i = 0; i < NPARAMS; i++) w->hist[w->hpos][i] = w->cache[i].load();
    w->hpos = (w->hpos + 1) % Plugin::NHIST;
    if (w->hcount < Plugin::NHIST) w->hcount++;
}
static bool hist_pop(Plugin *w) {
    if (!w->hcount) return false;
    w->hpos = (w->hpos + Plugin::NHIST - 1) % Plugin::NHIST;
    w->hcount--;
    for (int i = 0; i < NPARAMS; i++)
        if (is_sound(i) && w->cache[i].load() != w->hist[w->hpos][i]) { w->cache[i].store(w->hist[w->hpos][i]); w->notify[i].store(1); }
    return true;
}

/* step pitches by style: mostly the root, then octaves, tritones and minor seconds (industrial),
 * bell intervals (metal), or plain fifths and fourths (drone); ANY is free */
static int step_pitch(Plugin *w, int style) {
    switch (style) {
    case ST_METAL: return PICK(w, 0, 0, 0, 12, 7, -5, 19, 24, 3, -12);
    case ST_DRONE: return PICK(w, 0, 0, 0, 0, 12, -12, 7, -5, 5);
    case ST_ANY: return (int)std::lround(rr(w, -24, 24));
    default: return PICK(w, 0, 0, 0, 0, -12, 12, 1, 6, -6, 13, -11, 7, 24, -24);
    }
}
static void dice_steps(Plugin *w, int style) {
    const float dens = pct(w, SEQ_DENSITY);
    bool any = false;
    for (int i = 0; i < 8; i++) {
        put(w, SEQ_P1 + i, step_pitch(w, style));
        const bool g = chance(w, dens);
        any |= g;
        put(w, SEQ_G1 + i, g ? 1 : 0);
    }
    if (!any) put(w, SEQ_G1 + (int)(urnd(w) * 8) % 8, 1);   /* never an empty pattern */
}

/* keeps a roll audible: a source in the mixer, VCF 1 not shut, VCF 2 above a high-passed VCF 1 */
static void safety(Plugin *w) {
    if (val(w, VOICE_VOL) < 25 && val(w, PERC_VOL) < 25) put(w, VOICE_VOL, rr(w, 55, 75));
    if (sw(w, LOCK_FILTER)) return;
    if (val(w, F2_CUTOFF) < 35) put(w, F2_CUTOFF, rr(w, 40, 70));
    if (sw(w, F1_TYPE)) {
        /* the high-pass stays below what it would take away: sine-like models (organ, drone,
         * grains) have little above the note, a percussion-only sound sits at PERC FREQ; and a
         * high-pass that the envelope sweeps beyond ~5 kHz leaves only a click: cap the sweep */
        const int m = (int)val(w, MODEL);
        float cap = (m == rattler::MODEL_ORGAN || m == rattler::MODEL_DRONE || m == rattler::MODEL_GRAINS) ? 45.0f : 65.0f;
        if (val(w, VOICE_VOL) < 30) cap = std::min(cap, 100.0f * std::log2(1.2f * law_perc_hz(pct(w, PERC_FREQ)) / 20.0f) / 9.8137812f);
        cap = std::max(cap, 10.0f);
        if (val(w, F1_CUTOFF) > cap) put(w, F1_CUTOFF, rr(w, 0.5f * cap, cap));
        const float top = (8.0f - 0.0981378f * val(w, F1_CUTOFF)) / 0.08f;
        if (val(w, F1_MOD) > top) put(w, F1_MOD, std::max(0.0f, top * rr(w, 0.4f, 1.0f)));
        if (val(w, F2_CUTOFF) < val(w, F1_CUTOFF) + 20) put(w, F2_CUTOFF, std::min(100.0f, val(w, F1_CUTOFF) + rr(w, 20, 35)));
    } else if (val(w, F1_CUTOFF) + 0.6f * val(w, F1_MOD) < 40) put(w, F1_CUTOFF, rr(w, 40, 60));
}

static void dice_sound(Plugin *w) {
    const int st = clampi((int)val(w, DICE_STYLE), 0, NSTYLES - 1);
    using namespace rattler;
    switch (st) {
    case ST_INDUSTRIAL:
        put(w, MODEL, PICK(w, MODEL_METAL, MODEL_METAL, MODEL_NOISE, MODEL_NOISE, MODEL_GRAINS, MODEL_CHIPTUNE, MODEL_REED, MODEL_STRINGS));
        put(w, SPREAD, rr(w, 35, 100)); put(w, CHARACTER, rr(w, 40, 100)); put(w, VOICE_VOL, rr(w, 55, 85));
        put(w, F1_TYPE, chance(w, 0.35f)); put(w, F1_CUTOFF, rr(w, 30, 80)); put(w, F1_RES, rr(w, 25, 85)); put(w, F1_MOD, rr(w, 10, 70));
        put(w, F2_CUTOFF, rr(w, 45, 100)); put(w, F2_RES, rr(w, 0, 65)); put(w, F2_MOD, chance(w, 0.5f) ? rr(w, 10, 60) : 0);
        put(w, ATTACK, rr(w, 0, 8)); put(w, DECAY, rr(w, 26, 55)); put(w, ENV_MODE, chance(w, 0.75f));
        if (chance(w, 0.55f)) { put(w, PERC_VOL, rr(w, 40, 90)); put(w, PERC, rr(w, 5, 45)); put(w, PERC_FREQ, rr(w, 20, 100)); }
        else put(w, PERC_VOL, 0);
        put(w, LFO_SPEED, rr(w, 30, 90)); put(w, LFO_SHAPE, PICK(w, LFO_RANDOM1, LFO_RANDOM2, LFO_RANDOM3, LFO_RAMP_DOWN, LFO_SEQ, LFO_TRI));
        put(w, LFO_SLEW, rr(w, 0, 30));
        put(w, FM_DEPTH, chance(w, 0.4f) ? rr(w, 5, 45) : 0); put(w, FM_SRC, PICK(w, FM_LFO, FM_ENV, FM_DUAL));
        put(w, REV_IN, rr(w, 50, 90)); put(w, REV_LEVEL, chance(w, 0.5f) ? rr(w, 15, 55) : 0);
        put(w, REV_FB, chance(w, 0.3f) ? rr(w, 10, 40) : 0); put(w, REV_PRE, chance(w, 0.3f));
        break;
    case ST_METAL:
        put(w, MODEL, PICK(w, MODEL_METAL, MODEL_METAL, MODEL_DRONE, MODEL_ORGAN, MODEL_REED));
        put(w, SPREAD, rr(w, 55, 100)); put(w, CHARACTER, rr(w, 25, 90)); put(w, VOICE_VOL, rr(w, 60, 85));
        put(w, F1_TYPE, chance(w, 0.2f)); put(w, F1_CUTOFF, rr(w, 40, 90)); put(w, F1_RES, rr(w, 10, 70)); put(w, F1_MOD, rr(w, 0, 60));
        put(w, F2_CUTOFF, rr(w, 60, 100)); put(w, F2_RES, rr(w, 0, 50)); put(w, F2_MOD, chance(w, 0.3f) ? rr(w, 5, 35) : 0);
        put(w, ATTACK, rr(w, 0, 5)); put(w, DECAY, rr(w, 35, 75)); put(w, ENV_MODE, chance(w, 0.9f));
        if (chance(w, 0.4f)) { put(w, PERC_VOL, rr(w, 30, 80)); put(w, PERC, rr(w, 5, 35)); put(w, PERC_FREQ, rr(w, 50, 100)); }
        else put(w, PERC_VOL, 0);
        put(w, LFO_SPEED, rr(w, 10, 70)); put(w, LFO_SHAPE, PICK(w, LFO_TRI, LFO_SINE, LFO_RANDOM2, LFO_RANDOM3)); put(w, LFO_SLEW, rr(w, 0, 40));
        put(w, FM_DEPTH, chance(w, 0.25f) ? rr(w, 3, 25) : 0); put(w, FM_SRC, PICK(w, FM_LFO, FM_ENV));
        put(w, REV_IN, rr(w, 55, 90)); put(w, REV_LEVEL, chance(w, 0.7f) ? rr(w, 25, 65) : 0);
        put(w, REV_FB, chance(w, 0.2f) ? rr(w, 10, 35) : 0); put(w, REV_PRE, chance(w, 0.2f));
        break;
    case ST_NOISE:
        put(w, MODEL, PICK(w, MODEL_NOISE, MODEL_NOISE, MODEL_GRAINS, MODEL_CHIPTUNE));
        put(w, SPREAD, rr(w, 20, 100)); put(w, CHARACTER, rr(w, 30, 100)); put(w, VOICE_VOL, rr(w, 55, 85));
        put(w, F1_TYPE, chance(w, 0.5f)); put(w, F1_CUTOFF, rr(w, 35, 85)); put(w, F1_RES, rr(w, 30, 90)); put(w, F1_MOD, rr(w, 0, 60));
        put(w, F2_CUTOFF, rr(w, 40, 95)); put(w, F2_RES, rr(w, 20, 80)); put(w, F2_MOD, chance(w, 0.6f) ? rr(w, 15, 70) : 0);
        put(w, ATTACK, rr(w, 0, 10)); put(w, DECAY, rr(w, 26, 50)); put(w, ENV_MODE, chance(w, 0.7f));
        if (chance(w, 0.4f)) { put(w, PERC_VOL, rr(w, 30, 80)); put(w, PERC, rr(w, 3, 30)); put(w, PERC_FREQ, rr(w, 40, 100)); }
        else put(w, PERC_VOL, 0);
        put(w, LFO_SPEED, rr(w, 50, 95)); put(w, LFO_SHAPE, PICK(w, LFO_RANDOM1, LFO_RANDOM3, LFO_RANDOM2, LFO_SEQ)); put(w, LFO_SLEW, rr(w, 0, 20));
        put(w, FM_DEPTH, chance(w, 0.35f) ? rr(w, 5, 50) : 0); put(w, FM_SRC, PICK(w, FM_LFO, FM_ENV, FM_DUAL));
        put(w, REV_IN, rr(w, 50, 90)); put(w, REV_LEVEL, chance(w, 0.4f) ? rr(w, 15, 50) : 0);
        put(w, REV_FB, chance(w, 0.35f) ? rr(w, 10, 45) : 0); put(w, REV_PRE, chance(w, 0.4f));
        break;
    case ST_PERC:
        put(w, MODEL, PICK(w, MODEL_METAL, MODEL_NOISE, MODEL_GRAINS, MODEL_DRONE, MODEL_CHIPTUNE));
        put(w, SPREAD, rr(w, 30, 100)); put(w, CHARACTER, rr(w, 20, 90)); put(w, VOICE_VOL, chance(w, 0.5f) ? 0 : rr(w, 15, 45));
        put(w, PERC_VOL, rr(w, 70, 95)); put(w, PERC, rr(w, 18, 55)); put(w, PERC_FREQ, rr(w, 0, 100));
        put(w, F1_TYPE, chance(w, 0.3f)); put(w, F1_CUTOFF, rr(w, 45, 100)); put(w, F1_RES, rr(w, 0, 60)); put(w, F1_MOD, rr(w, 0, 50));
        put(w, F2_CUTOFF, rr(w, 60, 100)); put(w, F2_RES, rr(w, 0, 50)); put(w, F2_MOD, chance(w, 0.3f) ? rr(w, 10, 50) : 0);
        put(w, ATTACK, rr(w, 0, 3)); put(w, DECAY, rr(w, 30, 55)); put(w, ENV_MODE, 1);
        put(w, LFO_SPEED, rr(w, 40, 90)); put(w, LFO_SHAPE, PICK(w, LFO_RANDOM1, LFO_RANDOM3, LFO_SEQ)); put(w, LFO_SLEW, 0);
        put(w, FM_DEPTH, chance(w, 0.3f) ? rr(w, 5, 40) : 0); put(w, FM_SRC, FM_ENV);
        put(w, REV_IN, rr(w, 55, 90)); put(w, REV_LEVEL, chance(w, 0.5f) ? rr(w, 20, 60) : 0);
        put(w, REV_FB, chance(w, 0.15f) ? rr(w, 10, 30) : 0); put(w, REV_PRE, chance(w, 0.4f));
        break;
    case ST_DRONE:
        put(w, MODEL, PICK(w, MODEL_DRONE, MODEL_STRINGS, MODEL_REED, MODEL_ORGAN, MODEL_GRAINS));
        put(w, SPREAD, rr(w, 5, 60)); put(w, CHARACTER, rr(w, 0, 70)); put(w, VOICE_VOL, rr(w, 60, 80));
        put(w, F1_TYPE, chance(w, 0.15f)); put(w, F1_CUTOFF, rr(w, 35, 75)); put(w, F1_RES, rr(w, 20, 70)); put(w, F1_MOD, rr(w, 0, 25));
        put(w, F2_CUTOFF, rr(w, 45, 85)); put(w, F2_RES, rr(w, 10, 60)); put(w, F2_MOD, chance(w, 0.7f) ? rr(w, 10, 50) : 0);
        put(w, ATTACK, rr(w, 40, 85)); put(w, DECAY, rr(w, 55, 90)); put(w, ENV_MODE, 0);
        put(w, PERC_VOL, 0);
        put(w, LFO_SPEED, rr(w, 5, 45)); put(w, LFO_SHAPE, PICK(w, LFO_TRI, LFO_SINE, LFO_RANDOM2)); put(w, LFO_SLEW, rr(w, 10, 60));
        put(w, FM_DEPTH, chance(w, 0.3f) ? rr(w, 2, 20) : 0); put(w, FM_SRC, FM_LFO);
        put(w, REV_IN, rr(w, 60, 90)); put(w, REV_LEVEL, chance(w, 0.8f) ? rr(w, 30, 70) : 0);
        put(w, REV_FB, chance(w, 0.5f) ? rr(w, 15, 45) : 0); put(w, REV_PRE, 0);
        break;
    default:   /* ANY */
        put(w, MODEL, (int)(urnd(w) * NMODELS) % NMODELS);
        put(w, SPREAD, rr(w, 0, 100)); put(w, CHARACTER, rr(w, 0, 100)); put(w, VOICE_VOL, rr(w, 40, 85));
        put(w, F1_TYPE, chance(w, 0.3f)); put(w, F1_CUTOFF, rr(w, 25, 95)); put(w, F1_RES, rr(w, 0, 85)); put(w, F1_MOD, rr(w, 0, 70));
        put(w, F2_CUTOFF, rr(w, 40, 100)); put(w, F2_RES, rr(w, 0, 70)); put(w, F2_MOD, chance(w, 0.5f) ? rr(w, 0, 60) : 0);
        put(w, ATTACK, rr(w, 0, 70)); put(w, DECAY, rr(w, 20, 85)); put(w, ENV_MODE, chance(w, 0.5f));
        if (chance(w, 0.5f)) { put(w, PERC_VOL, rr(w, 30, 90)); put(w, PERC, rr(w, 5, 60)); put(w, PERC_FREQ, rr(w, 0, 100)); }
        else put(w, PERC_VOL, 0);
        put(w, LFO_SPEED, rr(w, 0, 95)); put(w, LFO_SHAPE, (int)(urnd(w) * NLFO) % NLFO); put(w, LFO_SLEW, rr(w, 0, 50));
        put(w, FM_DEPTH, chance(w, 0.35f) ? rr(w, 0, 40) : 0); put(w, FM_SRC, (int)(urnd(w) * 3) % 3);
        put(w, REV_IN, rr(w, 40, 90)); put(w, REV_LEVEL, chance(w, 0.5f) ? rr(w, 10, 60) : 0);
        put(w, REV_FB, chance(w, 0.3f) ? rr(w, 5, 40) : 0); put(w, REV_PRE, chance(w, 0.3f));
        break;
    }
    dice_steps(w, st);
    safety(w);
}

/* a step away from the current sound: knobs move by up to +-40 % of their range times AMOUNT,
 * a knob at zero (a send, a mod depth) mostly stays off, switches flip now and then, MODEL
 * changes rarely; the steps change their pitch to one of the style's intervals */
static void mutate(Plugin *w) {
    const float a = pct(w, DICE_AMOUNT);
    if (a <= 0) return;
    const int st = clampi((int)val(w, DICE_STYLE), 0, NSTYLES - 1);
    for (int k = 0; k < NKEYS; k++) {
        const int i = IDX[k];
        if (i < 0 || !is_sound(i) || locked(w, k)) continue;
        const param_t *pp = &PARAMS[i];
        const float v = val_at(w, i);
        if (k >= SEQ_P1 && k <= SEQ_P8) { if (chance(w, 0.3f * a)) put(w, k, step_pitch(w, st)); continue; }
        if (pp->nopts) {
            const float p = k == MODEL ? 0.12f * a : 0.2f * a;
            if (!chance(w, p)) continue;
            if (pp->nopts == 2) put(w, k, v > 0.5f ? 0 : 1);
            else put(w, k, (int)(urnd(w) * pp->nopts) % pp->nopts);
            continue;
        }
        if (v <= pp->min && !chance(w, 0.3f * a)) continue;
        const float span = pp->max - pp->min;
        put(w, k, rattler::clampf(v + (2 * urnd(w) - 1) * a * 0.4f * span, pp->min, pp->max));
    }
    bool any = false;
    for (int g = 0; g < 8; g++) any |= sw(w, SEQ_G1 + g);
    if (!any) put(w, SEQ_G1, 1);
    safety(w);
}

static void configure(Plugin *w);
/* runs on the audio thread: the requests of the skin buttons and of CC 20..23 */
static void run_requests(Plugin *w) {
    const int r = w->req.exchange(0);
    if (!r) return;
    if (r & REQ_UNDO) hist_pop(w);
    if (r & REQ_DICE) { hist_push(w); dice_sound(w); }
    if (r & REQ_MUTATE) { hist_push(w); mutate(w); }
    if (r & REQ_SEQ) { hist_push(w); dice_steps(w, clampi((int)val(w, DICE_STYLE), 0, NSTYLES - 1)); }
    configure(w);
}

/* ---- sequencer -----------------------------------------------------------------
 * 8 steps, each a pitch (semitones relative to the key held) and a gate; a step with its gate
 * on fires the envelope and the percussion and holds the gate for half a step. It runs while a
 * key is held, in mono mode. With the host's transport running, step N simply IS position
 * N * step length of the song (absolute index, as in mpc-vst-acid's clock): the pattern stays
 * locked to the bar whatever the host does between callbacks, and step 1 falls on the start of
 * the song. With the transport stopped it runs at the last tempo seen, from step 1 with the key.
 * ---------------------------------------------------------------------------- */
static double step_samples(Plugin *w) { return std::max(16.0, w->seq_beats * 60.0 / w->tempo * w->sr); }
/* SEQ CHANCE: a step with its gate on fires with this probability (100 % = as the original);
 * the step a key press starts always sounds, so a key never stays silent */
static void fire_step(Plugin *w, long long idx, bool by_key = false) {
    if (!w->nheld) return;
    const int st = (int)(((idx % 8) + 8) % 8);
    if (!w->seq_gate[st]) return;
    if (!by_key && w->seq_chance < 0.999f && urnd(w) >= w->seq_chance) return;
    w->engine.note_on(clampi(w->held[w->nheld - 1] + w->seq_pitch[st], 0, 127), true);
    w->gate_off = std::max(1, (int)(0.5 * step_samples(w)));
}

/* ---- MIDI ------------------------------------------------------------------ */
static void all_off(Plugin *w) { w->nheld = 0; w->gate_off = 0; w->engine.all_off(); }
static void midi(Plugin *w, const uint8_t *d) {
    const int st = d[0] & 0xf0, n = d[1] & 0x7f;
    if (st == 0x90 && d[2] > 0) {
        if (w->poly) { w->engine.poly_on(n); return; }
        int k = 0;
        for (int i = 0; i < w->nheld; i++) if (w->held[i] != n) w->held[k++] = w->held[i];
        if (k == 32) { std::memmove(w->held, w->held + 1, 31); k = 31; }
        const bool first = k == 0;
        w->held[k++] = (uint8_t)n;
        w->nheld = k;
        if (!w->seq_on) w->engine.note_on(n, true);     /* every key is a gate, as on the original */
        else if (first) {                               /* the step in progress sounds at once; later keys only transpose */
            if (!w->was_playing) { w->seq_i = 0; w->to_next = step_samples(w); }
            fire_step(w, w->was_playing ? w->seq_abs : 0, true);
        }
    } else if (st == 0x80 || st == 0x90) {
        if (w->poly) { w->engine.poly_off(n); return; }
        int k = 0;
        for (int i = 0; i < w->nheld; i++) if (w->held[i] != n) w->held[k++] = w->held[i];
        if (k == w->nheld) return;
        w->nheld = k;
        if (!k) { w->gate_off = 0; w->engine.note_off(); }
        else if (!w->seq_on) w->engine.note_on(w->held[k - 1], false);   /* back to the key still held, no new attack */
    } else if (st == 0xe0) {
        w->engine.set_bend(((((int)d[2] & 0x7f) << 7 | (d[1] & 0x7f)) - 8192) * (2.0f / 8192.0f));
    } else if (st == 0xb0 && n >= 1 && n <= 7) {
        w->cc[n] = (d[2] & 0x7f) / 127.0f;
        configure(w);
    } else if (st == 0xb0 && n >= 20 && n <= 23) {   /* CC 20 RANDOM, 21 MUTATE, 22 UNDO, 23 DICE SEQ: fire when crossing the middle upwards */
        static const int REQ[4] = {REQ_DICE, REQ_MUTATE, REQ_UNDO, REQ_SEQ};
        const bool hi = (d[2] & 0x7f) >= 64;
        if (hi && !w->cc_trig[n - 20]) { w->req.fetch_or(REQ[n - 20]); run_requests(w); }
        w->cc_trig[n - 20] = hi;
    } else if (st == 0xb0 && (n == 120 || n == 123)) {
        all_off(w);
    }
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    run_requests(w);
    if (w->dirty.exchange(false)) configure(w);
    float *L = out[0], *R = out[1];
    std::memset(L, 0, sizeof(float) * (size_t)n);
    std::memset(R, 0, sizeof(float) * (size_t)n);

    /* the host's clock */
    const VstTimeInfo *ti = (const VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid | kVstPpqPosValid, 0, 0.0f);
    if (ti && (ti->flags & kVstTempoValid) && ti->tempo > 20 && ti->tempo < 1000) w->tempo = ti->tempo;
    const bool playing = ti && (ti->flags & kVstTransportPlaying) && (ti->flags & kVstPpqPosValid);
    const double sps = step_samples(w);
    const double step_pos = playing ? ti->ppqPos / w->seq_beats : 0;
    if (playing != w->was_playing) { w->was_playing = playing; w->seq_abs = -1000000; w->to_next = sps; w->seq_i = 0; }
    /* frame of the next step boundary at or after pos, n if none in this block */
    auto next_step = [&](int pos) -> int {
        if (!w->seq_on) return n;
        double f;
        if (playing) f = std::ceil(((double)(w->seq_abs + 1) - step_pos) * sps - 1e-6);
        else if (w->nheld) f = pos + std::ceil(w->to_next);
        else return n;
        return f >= n ? n : f < pos ? pos : (int)f;
    };
    if (w->seq_on && playing) {   /* the step this block starts in (transport start, a jump, or a boundary at frame 0) */
        const long long cur = (long long)std::floor(step_pos + 1e-6);
        if (cur != w->seq_abs) { w->seq_abs = cur; fire_step(w, cur); }
    }

    int pos = 0, k = 0;
    for (;;) {
        /* at pos: gate off, then step boundaries, then MIDI (a key played exactly on the grid meets the new step) */
        if (w->seq_on && next_step(pos) == pos && pos < n) {
            if (playing) { w->seq_abs++; fire_step(w, w->seq_abs); }
            else { w->seq_i++; w->to_next += sps; fire_step(w, w->seq_i); }
        }
        while (k < w->nev && w->ev[k].frame <= pos) midi(w, w->ev[k++].d);
        if (pos >= n) break;
        int f = n;
        if (k < w->nev) f = std::min(f, clampi(w->ev[k].frame, pos + 1, n));
        f = std::min(f, std::max(pos + 1, next_step(pos + 1)));
        if (w->gate_off > 0) f = std::min(f, pos + w->gate_off);
        w->engine.render(L + pos, R + pos, f - pos);
        if (w->gate_off > 0) { w->gate_off -= f - pos; if (w->gate_off <= 0) { w->gate_off = 0; w->engine.note_off(); } }
        if (!playing && w->seq_on && w->nheld) w->to_next -= f - pos;
        pos = f;
    }
    while (k < w->nev) midi(w, w->ev[k++].d);
    w->nev = 0;

    for (int c = 0; c < 2; c++) {
        float *y = out[c];
        for (int i = 0; i < n; i++) {
            const float a = std::fabs(y[i]);              /* output safety above -3 dBFS, ceiling 0.98 */
            if (a > 0.7f) {
                float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
                y[i] = std::copysign(0.7f + 0.28f * t * (27 + t2) / (27 + 9 * t2), y[i]);
            }
        }
    }
    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static intptr_t process_events(Plugin *w, const VstEvents *evs) {
    if (!evs) return 0;
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent *ev = evs->events[i];
        if (!ev || ev->type != kVstMidiType || w->nev >= 256) continue;
        const VstMidiEvent *m = (const VstMidiEvent *)ev;
        MidiEv &q = w->ev[w->nev++];
        q.frame = m->deltaFrames;
        q.d[0] = m->midiData[0]; q.d[1] = m->midiData[1]; q.d[2] = m->midiData[2];
    }
    std::stable_sort(w->ev, w->ev + w->nev, [](const MidiEv &a, const MidiEv &b) { return a.frame < b.frame; });
    return 1;
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key == SLOT) {   /* browsing only: LOAD loads, so turning the knob can't wipe what is playing */
        const int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur.load()) { w->cur.store(v); w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
        return;
    }
    if (is_trigger(key)) {
        const bool down = n > 0.5f;
        const bool rising = down && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = down;
        if (rising) {
            if (key == LOAD) slot_load(w);
            else if (key == SAVE) slot_save(w);
            else w->req.fetch_or(key == DICE ? REQ_DICE : key == MUTATE ? REQ_MUTATE : key == UNDO ? REQ_UNDO : REQ_SEQ);   /* runs at the next block */
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    const int key = KEY_OF[i];
    if (key == SLOT) return (float)w->cur.load() / (NSLOTS - 1);
    if (is_trigger(key)) return 0.0f;   /* triggers always read released */
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* project chunk: "RTLR1;" + the state + "prog=<slot>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "RTLR1;" + build_state(w, true);
    char buf[24];
    std::snprintf(buf, sizeof buf, "prog=%d;", w->cur.load());
    t += buf;
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 6) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));   /* stop at a NUL the host may have included */
    if (t.compare(0, 6, "RTLR1;")) return 0;
    apply_state(w, t.substr(6), true);
    return 1;
}

static void fmt_hz(char *b, size_t n, float hz) {
    if (hz >= 1000) std::snprintf(b, n, "%.2f kHz", hz / 1000);
    else if (hz >= 100) std::snprintf(b, n, "%.0f Hz", hz);
    else std::snprintf(b, n, "%.1f Hz", hz);
}
static void fmt_time(char *b, size_t n, float s) {
    if (s < 1) std::snprintf(b, n, "%.0f ms", s * 1000);
    else std::snprintf(b, n, "%.2f s", s);
}
static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    if (key == SLOT) { std::snprintf(buf, n, "%s", slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str()); return; }
    if (is_trigger(key)) { buf[0] = 0; return; }
    if (popup_is(idx)) {
        const int u = norm_to_ui(pp, w->open[idx]);
        if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[u]); else std::snprintf(buf, n, "%d", u);
        return;
    }
    const int u = norm_to_ui(pp, w->cache[idx].load());
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[u]); return; }
    const float x = (val_at(w, idx) - pp->min) / (pp->max > pp->min ? pp->max - pp->min : 1);
    switch (key) {
    case FREQ: std::snprintf(buf, n, "%+d st", u); break;
    case SEQ_P1: case SEQ_P2: case SEQ_P3: case SEQ_P4: case SEQ_P5: case SEQ_P6: case SEQ_P7: case SEQ_P8: std::snprintf(buf, n, "%+d st", u); break;
    case F1_CUTOFF: case F2_CUTOFF: fmt_hz(buf, n, 20 * std::pow(2.0f, law_cutoff_oct(x))); break;
    case ATTACK: fmt_time(buf, n, law_time(x, 0.001f, 5000)); break;
    case DECAY: fmt_time(buf, n, law_time(x, 0.005f, 2000)); break;
    case PERC: fmt_time(buf, n, law_perc_s(x)); break;
    case PERC_FREQ: fmt_hz(buf, n, law_perc_hz(x)); break;
    case LFO_SPEED: if (law_lfo_hz(x) < 1) std::snprintf(buf, n, "%.2f Hz", law_lfo_hz(x)); else fmt_hz(buf, n, law_lfo_hz(x)); break;
    case LFO_SLEW: fmt_time(buf, n, sq(x)); break;
    default: std::snprintf(buf, n, "%d %%", u); break;
    }
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: delete w; return 1;
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) {
            w->cur.store((int)v);
            slot_load(w);
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS || !p) return 0;
        copy_str(p, slot_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {   /* only a saved slot has a line in the file to carry the name */
        const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[n].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[n] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 2;   /* kPlugCategSynth */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_bank_key(idx);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[32];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0) { w->sr = o; w->engine.init(o); all_off(w); std::memset(w->cc, 0, sizeof w->cc); w->dirty.store(true); }
        return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (!v) all_off(w); return 1;
    case effProcessEvents: return process_events(w, (const VstEvents *)p);
    case effCanDo:
        if (p && (!std::strcmp((const char *)p, "receiveVstEvents") || !std::strcmp((const char *)p, "receiveVstMidiEvent") || !std::strcmp((const char *)p, "receiveVstTimeInfo"))) return 1;
        return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    ui_defaults(w);
    w->rs ^= (uint32_t)std::time(nullptr) * 2654435761u ^ (uint32_t)(uintptr_t)w;   /* each instance rolls differently */
    if (const char *seed = std::getenv("RATTLER_SEED")) w->rs = (uint32_t)std::strtoul(seed, nullptr, 10);   /* offline test */
    if (!w->rs) w->rs = 0x9e3779b9u;
    w->engine.init(w->sr);
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks | effFlagsIsSynth;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
