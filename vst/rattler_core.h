/* =============================================================================
 * rattler_core.h - Rattler: a paraphonic swarm synthesizer in the manner of the Eowave
 * Quadrantid Swarm. Eight oscillators share ONE filter chain, one VCA and one envelope:
 *
 *   8 VCOs -> wavefolder -> mixer -> VCF 1 (12 dB, LP/HP) -> VCF 2 (12 dB, LP) -> VCA -> out
 *   envelope (AR or AD) -> VCA and VCF 1
 *
 * Phase 1 (README): the models ORGAN (8 sines), STRINGS (8 saws) and DRONE (8 triangles) with
 * SPREAD and the wavefolder (CHARACTER), mono mode. Percussion voice, LFO, the other five
 * models, spring reverb, poly mode and the sequencer follow in phases 2 and 3.
 *
 * SPREAD sets the frequency ratios of the eight VCOs (an assumption, the manual gives no
 * numbers - README "Spread"): Sc = S^SPREAD_EXP,
 *   harmonic   (ORGAN, STRINGS):  f_i = f0 * (1 + i * Sc)        -> partials 1..8 at full
 *   inharmonic (DRONE, others):   f_i = f0 * (1 + Sc * i^1.4)    -> about four octaves at full
 * Sc never quite reaches 0: the oscillators run free, and eight of them frozen at exactly the
 * same frequency could cancel each other. At zero they drift by a fraction of a cent instead.
 *
 * Performance rules as in carp 2000: float only, no libm in the per-sample loop, slow changes
 * every CTL samples with linear ramps, an idle engine is skipped. CPU does not depend on the
 * number of keys held (paraphonic). MIT license.
 * ========================================================================== */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rattler {

static const int CTL = 16;              /* control rate: every 16 samples */
static const int NOSC = 8;
static const float SPREAD_EXP = 3.0f;   /* knob law of SPREAD: 3 .. 4, by ear (README, open question 1) */
static const float SPREAD_INH = 1.4f;   /* exponent of the inharmonic series */
static const float SPREAD_MIN = 0.0004f;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* tanh, rational approximation, exact +-1 beyond |x| = 3 */
static inline float fast_tanh(float x) {
    x = clampf(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
/* 2^x, |error| < 0.01 cent */
static inline float fast_exp2(float x) {
    x = clampf(x, -40.0f, 40.0f);
    const int i = (int)(x + 64.5f);
    const float f = (x - (float)(i - 64)) * 0.69314718f;
    const float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.041666667f + f * 0.0083333333f))));
    union { uint32_t u; float fl; } s;
    s.u = (uint32_t)(i - 64 + 127) << 23;
    return p * s.fl;
}
/* sin(2 pi p), p in [0,1) */
static inline float fast_sin(float p) {
    const float t = p - 0.5f;
    float y = 8.0f * t - 16.0f * t * std::fabs(t);
    y = 0.225f * (y * std::fabs(y) - y) + y;
    return -y;
}
/* PolyBLEP residual for a step at phase 0, phase increment dt */
static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
static inline bool not_finite(float v) {   /* works under -ffast-math */
    uint32_t u; std::memcpy(&u, &v, 4);
    return (u & 0x7f800000u) == 0x7f800000u;
}
/* wavefolder: identity up to +-1, reflects what goes beyond (triangle transfer curve) */
static inline float fold(float u) {
    float t = 0.25f * u + 8.25f;
    t -= (float)(int)t;
    return 1.0f - 4.0f * std::fabs(t - 0.5f);
}

enum { MODEL_ORGAN, MODEL_STRINGS, MODEL_DRONE, NMODELS };
static inline bool harmonic(int model) { return model == MODEL_ORGAN || model == MODEL_STRINGS; }

struct Patch {
    int model = MODEL_STRINGS;
    float semis = 0;         /* FREQ: transposition in semitones */
    float spread = 0.3f;     /* 0..1, knob position */
    float character = 0;     /* 0..1, wavefolder */
    float voice = 1;         /* mixer gain of the VCOs into VCF 1 */
    float f1_oct = 6;        /* VCF 1 cutoff, octaves above 20 Hz */
    float f1_res = 0.2f;     /* 0..1 */
    float f1_mod = 2;        /* envelope -> VCF 1, octaves */
    bool f1_hp = false;
    float f2_oct = 9;        /* VCF 2 cutoff, octaves above 20 Hz */
    float f2_res = 0;
    float att = 0.005f;      /* seconds to full level */
    float dec = 0.5f;        /* seconds to -60 dB (decay in AD, release in AR) */
    bool ad = false;         /* false: AR (holds while a key is down), true: AD */
    float volume = 0.64f;
};

/* 12 dB state variable filter (trapezoidal, after Simper); unconditionally stable */
struct Svf {
    float ic1 = 0, ic2 = 0;
    inline void step(float in, float g, float k, float &lp, float &hp) {
        const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1;
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + g * a2 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        lp = v2; hp = in - k * v1 - v2;
    }
    void reset() { ic1 = ic2 = 0; }
};

struct Engine {
    enum { ENV_IDLE, ENV_ATTACK, ENV_HOLD, ENV_DECAY };

    void init(float rate) {
        sr = rate;
        for (int i = 0; i < NOSC; i++) { phase[i] = std::fmod(0.137f + 0.381966f * (float)i, 1.0f); inc[i] = 0; }
        f1.reset(); f2.reset();
        env = 0; stage = ENV_IDLE; gate = false;
        snap = true; ctl = 0;
        set_patch(p);
    }
    void set_patch(const Patch &q) {
        p = q;
        ka = 1.0f - std::exp(-1.466f / (std::max(p.att, 0.0005f) * sr));   /* towards 1.3, crossing 1 after att */
        kd = std::exp(-6.908f / (std::max(p.dec, 0.002f) * sr));
        k1 = 2.0f - 1.92f * clampf(p.f1_res, 0, 1);
        k2 = 2.0f - 1.92f * clampf(p.f2_res, 0, 1);
    }
    void note_on(int n, bool trigger) {
        note = (float)n;
        gate = true;
        if (trigger || stage == ENV_IDLE) stage = ENV_ATTACK;   /* from the current level: no click */
    }
    void note_off() {
        gate = false;
        if (!p.ad && (stage == ENV_ATTACK || stage == ENV_HOLD)) stage = ENV_DECAY;
    }
    void set_bend(float semitones) { bend = semitones; }
    bool idle() const { return stage == ENV_IDLE; }

    /* adds into L and R */
    void render(float *L, float *R, int n) {
        if (stage == ENV_IDLE) return;
        const float norm = 0.35355339f * p.voice;   /* 1 / sqrt(8) */
        int pos = 0;
        while (pos < n) {
            if (ctl <= 0) { control(); ctl = CTL; }
            const int m = std::min(ctl, n - pos);
            float buf[CTL];
            for (int s = 0; s < m; s++) buf[s] = 0;
            oscillators(buf, m);
            for (int s = 0; s < m; s++) {
                /* envelope */
                if (stage == ENV_ATTACK) {
                    env += (1.3f - env) * ka;
                    if (env >= 1.0f) { env = 1.0f; stage = (p.ad || !gate) ? ENV_DECAY : ENV_HOLD; }
                } else if (stage == ENV_DECAY) {
                    env *= kd;
                    if (env < 1e-5f) { env = 0; stage = ENV_IDLE; }
                } else if (stage == ENV_HOLD && (p.ad || !gate)) stage = ENV_DECAY;
                g1 += dg1; g2 += dg2;
                float lp, hp;
                f1.step(fast_tanh(buf[s] * norm), g1, k1, lp, hp);
                const float y1 = p.f1_hp ? hp : lp;
                f2.step(fast_tanh(y1), g2, k2, lp, hp);
                const float y = fast_tanh(lp) * env * p.volume;
                L[pos + s] += y; R[pos + s] += y;
            }
            pos += m; ctl -= m;
            if (stage == ENV_IDLE) break;
        }
    }

private:
    Patch p;
    float sr = 44100;
    float phase[NOSC], inc[NOSC];
    Svf f1, f2;
    float env = 0, ka = 0, kd = 0, k1 = 2, k2 = 2;
    float g1 = 0.1f, g2 = 0.1f, dg1 = 0, dg2 = 0;
    float sp = 0, ch = 0, fgain = 1;
    float note = 60, bend = 0;
    int stage = ENV_IDLE, ctl = 0;
    bool gate = false, snap = true;

    float cutoff_g(float oct) const {
        const float fc = clampf(20.0f * fast_exp2(oct), 20.0f, std::min(18000.0f, 0.45f * sr));
        return std::tan(3.14159265f * fc / sr);
    }
    /* every CTL samples: pitch and spread -> the eight increments, fold gain, filter targets */
    void control() {
        const float a = snap ? 1.0f : 0.06f;
        sp += (p.spread - sp) * a;
        ch += (p.character - ch) * a;
        float sc = sp;
        for (float e = 1; e < SPREAD_EXP - 0.5f; e += 1) sc *= sp;      /* integer exponent 3 or 4 */
        sc = std::max(sc, SPREAD_MIN);
        const float f0 = 440.0f * fast_exp2((note - 69.0f + p.semis + bend) * (1.0f / 12.0f)) / sr;
        static const float INH[NOSC] = {0.0f, 1.0f, 2.6390158f, 4.6555367f, 6.9644045f, 9.5182697f, 12.286436f, 15.245345f};   /* i^1.4 */
        const bool h = harmonic(p.model);
        for (int i = 0; i < NOSC; i++) inc[i] = std::min(f0 * (1.0f + sc * (h ? (float)i : INH[i])), 0.45f);
        fgain = 1.0f + 7.0f * ch * ch;
        const float t1 = cutoff_g(p.f1_oct + p.f1_mod * env), t2 = cutoff_g(p.f2_oct);
        if (snap) { g1 = t1; g2 = t2; dg1 = dg2 = 0; snap = false; }
        else { dg1 = (t1 - g1) * (1.0f / CTL); dg2 = (t2 - g2) * (1.0f / CTL); }
        if (not_finite(f1.ic1 + f1.ic2 + f2.ic1 + f2.ic2)) { f1.reset(); f2.reset(); }
    }
    void oscillators(float *buf, int m) {
        const bool folding = fgain > 1.001f;
        const float fg = fgain;
        for (int i = 0; i < NOSC; i++) {
            float ph = phase[i];
            const float d = inc[i];
            switch (p.model) {
            case MODEL_ORGAN:
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    const float x = fast_sin(ph);
                    buf[s] += folding ? fold(fg * x) : x;
                }
                break;
            case MODEL_STRINGS:
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    const float x = 2.0f * ph - 1.0f - blep(ph, d);
                    buf[s] += folding ? fold(fg * x) : x;
                }
                break;
            default:   /* MODEL_DRONE */
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    const float x = 1.0f - 4.0f * std::fabs(ph - 0.5f);
                    buf[s] += folding ? fold(fg * x) : x;
                }
                break;
            }
            phase[i] = ph;
        }
    }
};

}   // namespace rattler
