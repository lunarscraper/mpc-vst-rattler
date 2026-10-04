/* =============================================================================
 * rattler_core.h - Rattler: a paraphonic swarm synthesizer in the manner of the Eowave
 * Quadrantid Swarm. Eight oscillators share ONE filter chain, one VCA and one envelope:
 *
 *   8 VCOs -> wavefolder -> mixer -> VCF 1 (12 dB, LP/HP) -> VCF 2 (12 dB, LP) -> VCA -> out
 *   envelope (AR or AD) -> VCA and VCF 1
 *
 *   percussion (noise + tone burst through its own bandpass, own decay) -> mixer
 *   LFO -> slew -> VCF 2;  FREQ MOD: LFO, envelope or both -> pitch of all VCOs
 *   VCA -> spring reverb -> out; the reverb return is the mixer's third input (feedback, limited).
 *   REV PRE VCA puts the spring in front of the VCA instead (the envelope then cuts the tail).
 *
 * All three phases of the README: eight models, SPREAD, CHARACTER, percussion, LFO (eight shapes,
 * the last one steps through the sequencer's values), FREQ MOD, spring reverb with feedback,
 * mono and poly. In POLY every VCO has its own pitch and its own VCA (SPREAD has no function);
 * filters and envelope stay shared. The step sequencer itself lives in rattler_vst.cpp (it needs
 * the host clock) and plays the engine like a keyboard.
 *
 *   ORGAN 8 sines | STRINGS 8 saws | DRONE 8 triangles | REED 8 x 2 squares, the second detuned
 *     -> CHARACTER = wavefolder
 *   METAL 8 sines, phase-modulated by a 9th oscillator -> CHARACTER = FM depth
 *   CHIPTUNE 8 pulses, each with its own slow PWM -> CHARACTER = PWM depth
 *   GRAINS 8 generators of windowed sine wavelets | NOISE 8 tuned sample & hold noises
 *     -> CHARACTER = amount of a short delay
 * Assumptions where the manual is silent (README, open questions): REED_DETUNE, METAL_RATIO,
 * GRAIN_HZ, NOISE_RATE, DELAY_S / DELAY_FB below, the three random LFO shapes, the percussion.
 *
 * SPREAD sets the frequency ratios of the eight VCOs (an assumption, the manual gives no
 * numbers - README "Spread"): Sc = S^SPREAD_EXP,
 *   harmonic   (ORGAN, STRINGS):  f_i = f0 * (1 + i * Sc)        -> partials 1..8 at full
 *   inharmonic (DRONE, others):   f_i = f0 * (1 + Sc * i^1.4)    -> about four octaves at full
 * The oscillators run free, and eight of them frozen at exactly the same frequency could cancel
 * each other: at the bottom of the knob they keep an irregular detune of a few cents instead.
 *
 * Performance rules as in carp 2000: float only, no libm in the per-sample loop, slow changes
 * every CTL samples with linear ramps, an idle engine is skipped. CPU does not depend on the
 * number of keys held (paraphonic). MIT license.
 * ========================================================================== */
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rattler {

static const int CTL = 16;              /* control rate: every 16 samples */
static const int NOSC = 8;
static const float SPREAD_EXP = 3.0f;   /* knob law of SPREAD: 3 .. 4, by ear (README, open question 1) */
static const float SPREAD_INH = 1.4f;   /* exponent of the inharmonic series */
static const float SPREAD_MIN = 0.0004f;
static const float REED_DETUNE = 1.006f;   /* second VCO of a REED pair: about 10 cents up */
static const float METAL_RATIO = 2.76f;    /* 9th oscillator of METAL, relative to the note */
static const float GRAIN_HZ = 30.0f;       /* GRAINS: mean wavelets per second and generator */
static const float NOISE_RATE = 6.0f;      /* NOISE: sample & hold clock, relative to the VCO frequency */
static const float DELAY_S = 0.030f;       /* GRAINS/NOISE: the short delay */
static const float DELAY_FB = 0.5f;
static const int DELAY_MAX = 8192;
static const float REV_SECONDS = 2.5f;     /* spring reverb: decay to -60 dB */
static const float POLY_NORM = 0.6f;       /* POLY: level of one VCO (mono: 1 / sqrt(8) each) */

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

enum { MODEL_ORGAN, MODEL_STRINGS, MODEL_DRONE, MODEL_REED, MODEL_METAL, MODEL_CHIPTUNE, MODEL_GRAINS, MODEL_NOISE, NMODELS };
enum { LFO_TRI, LFO_SINE, LFO_RAMP_DOWN, LFO_RAMP_UP, LFO_RANDOM1, LFO_RANDOM2, LFO_RANDOM3, LFO_SEQ, NLFO };
enum { FM_LFO, FM_ENV, FM_DUAL };
static inline bool folds(int model) { return model <= MODEL_REED; }
static inline bool delays(int model) { return model == MODEL_GRAINS || model == MODEL_NOISE; }
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
    /* phase 2 */
    float perc_dec = 0.1f;   /* percussion decay, seconds to -60 dB */
    float perc_hz = 700;     /* bandpass centre and pitch of the tone burst */
    float perc = 0;          /* mixer gain of the percussion into VCF 1 */
    float lfo_hz = 2;
    int lfo_shape = LFO_TRI;
    float lfo_slew = 0;      /* seconds */
    float f2_mod = 0;        /* LFO -> VCF 2, octaves */
    float fm = 0;            /* FREQ MOD, octaves */
    int fm_src = FM_LFO;
    /* phase 3 */
    float rev_in = 1;        /* VCA -> spring */
    float rev_level = 0;     /* spring -> output */
    float rev_fb = 0;        /* spring -> mixer input 3 */
    bool rev_pre = false;    /* spring in front of the VCA */
    bool poly = false;
    float seq[8] = {0, 0, 0, 0, 0, 0, 0, 0};   /* the sequencer's step values, -1..1, for LFO shape SEQ */
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

/* Spring reverb (taken from carp 2000): two springs, each a delay line in a damped feedback loop with
 * a chain of stretched allpasses (z^-K), which delays the treble more than the bass on every
 * pass - the spring's chirp. Mono in, spring A left, spring B right. No convolution. */
struct Reverb {
    void init(float rate) {
        sr = rate;
        K = (int)(rate / 11025.0f + 0.5f); if (K < 1) K = 1; if (K > KMAX) K = KMAX;
        sp[0].init((int)(0.0331f * rate), 0.62f);
        sp[1].init((int)(0.0413f * rate), 0.58f);
        ki = 0; in1 = in2 = inhp = 0; awake = false; quiet = 0;
        c_in = 1 - std::exp(-2 * 3.14159265f * 4500 / rate);
        c_hp = 1 - std::exp(-2 * 3.14159265f * 120 / rate);
        c_out = 1 - std::exp(-2 * 3.14159265f * 5000 / rate);
        set_length(len);
    }
    void set_length(float seconds) {               /* decay time to -60 dB */
        len = seconds;
        for (int s = 0; s < 2; s++) sp[s].fb = std::pow(10.0f, -3.0f * (float)sp[s].n / (sr * seconds));
    }
    bool active() const { return awake; }
    /* reads the send S, adds the wet signal to L and R */
    void process(const float *S, float *L, float *R, int n) {
        float pk = 0;
        for (int i = 0; i < n; i++) { const float a = std::fabs(S[i]); if (a > pk) pk = a; }
        if (!awake) { if (pk < 1e-6f) return; awake = true; quiet = 0; }
        float opk = 0;
        for (int i = 0; i < n; i++) {
            in1 += c_in * (S[i] - in1); in2 += c_in * (in1 - in2);
            inhp += c_hp * (in2 - inhp);
            const float x = in2 - inhp;
            const float a = sp[0].step(x, K, ki, c_out), b = sp[1].step(x, K, ki, c_out);
            if (++ki >= K) ki = 0;
            L[i] += 0.6f * (a + 0.25f * b); R[i] += 0.6f * (b + 0.25f * a);
            const float m = std::fabs(a) + std::fabs(b); if (m > opk) opk = m;
        }
        if (pk < 1e-6f && opk < 1e-6f) { quiet += n; if (quiet > (int)sr) { awake = false; for (int s = 0; s < 2; s++) sp[s].clear(); in1 = in2 = inhp = 0; } }
        else quiet = 0;
        if (not_finite(opk)) { for (int s = 0; s < 2; s++) sp[s].clear(); in1 = in2 = inhp = 0; }
    }
private:
    enum { NAP = 24, KMAX = 20 };
    struct Spring {
        std::vector<float> buf;
        int n = 1, pos = 0;
        float a = 0.6f, fb = 0.5f, damp = 0, out = 0, ap[NAP][KMAX];
        void init(int length, float coef) { n = length < 8 ? 8 : length; a = coef; buf.assign((size_t)n, 0.0f); clear(); }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; damp = out = 0; std::memset(ap, 0, sizeof ap); }
        inline float step(float in, int, int ki, float c_out) {
            const float y = buf[(size_t)pos];
            damp += 0.45f * (y - damp);                       /* loop damping */
            float x = in + fb * damp;
            for (int j = 0; j < NAP; j++) { const float o = a * x + ap[j][ki]; ap[j][ki] = x - a * o; x = o; }
            buf[(size_t)pos] = x; if (++pos >= n) pos = 0;
            out += c_out * (y - out);
            return out;
        }
    };
    Spring sp[2];
    float sr = 44100, len = 2, in1 = 0, in2 = 0, inhp = 0, c_in = 0.5f, c_hp = 0.02f, c_out = 0.5f;
    int K = 4, ki = 0, quiet = 0;
    bool awake = false;
};

struct Engine {
    enum { ENV_IDLE, ENV_ATTACK, ENV_HOLD, ENV_DECAY };

    void init(float rate) {
        sr = rate;
        for (int i = 0; i < NOSC; i++) {
            phase[i] = std::fmod(0.137f + 0.381966f * (float)i, 1.0f); inc[i] = 0;
            phase2[i] = std::fmod(0.611f + 0.381966f * (float)i, 1.0f);
            pw_ph[i] = phase[i]; pw[i] = 0.5f;
            gph[i] = 1.0f; ginc[i] = 0; gon[i] = false; nval[i] = 0;
        }
        ph9 = 0; inc9 = 0;
        std::memset(dl, 0, sizeof dl); dpos = 0;
        dlen = std::max(1, std::min(DELAY_MAX - 1, (int)(DELAY_S * sr)));
        f1.reset(); f2.reset(); pbp.reset();
        reverb.init(rate); reverb.set_length(REV_SECONDS);
        std::memset(fbk, 0, sizeof fbk);
        for (int i = 0; i < NOSC; i++) { og[i] = ot[i] = 1.0f; onote[i] = 60; oheld[i] = oslow[i] = false; oage[i] = 0; }
        nheld = 0; age = 0; ovca = 1.0f - std::exp(-1.0f / (0.004f * rate));
        env = 0; stage = ENV_IDLE; gate = false;
        penv = 0; pph = 0;
        lph = 0; lfo = lfo_s = 0; r_prev = r_next = 0; sidx = 0;
        snap = true; ctl = 0;
        set_patch(p);
    }
    void set_patch(const Patch &q) {
        if (q.model != p.model) { std::memset(dl, 0, sizeof dl); for (int i = 0; i < NOSC; i++) { gph[i] = 1.0f; gon[i] = false; } }
        if (q.poly != p.poly) { for (int i = 0; i < NOSC; i++) { og[i] = ot[i] = q.poly ? 0.0f : 1.0f; oheld[i] = false; } nheld = 0; }
        p = q;
        ka = 1.0f - std::exp(-1.466f / (std::max(p.att, 0.0005f) * sr));   /* towards 1.3, crossing 1 after att */
        kd = std::exp(-6.908f / (std::max(p.dec, 0.002f) * sr));
        k1 = 2.0f - 1.92f * clampf(p.f1_res, 0, 1);
        k2 = 2.0f - 1.92f * clampf(p.f2_res, 0, 1);
        pkd = std::exp(-6.908f / (std::max(p.perc_dec, 0.002f) * sr));
        const float pf = clampf(p.perc_hz, 20.0f, std::min(12000.0f, 0.4f * sr));
        pg = std::tan(3.14159265f * pf / sr);
        pinc = pf / sr;
        const float dt = (float)CTL / sr;
        lslew = p.lfo_slew > 0.0005f ? 1.0f - std::exp(-dt / p.lfo_slew) : 1.0f;
        linc = std::min(p.lfo_hz * dt, 0.5f);
    }
    void note_on(int n, bool trigger) {
        note = (float)n;
        gate = true;
        if (trigger || stage == ENV_IDLE) {
            stage = ENV_ATTACK;                         /* from the current level: no click */
            if (trigger) { penv = 1.0f; pph = 0; }      /* every gate fires the percussion */
        }
    }
    void note_off() {
        gate = false;
        if (!p.ad && (stage == ENV_ATTACK || stage == ENV_HOLD)) stage = ENV_DECAY;
    }
    /* POLY: a key takes one of the eight VCOs. A key released while others are held fades its VCO
     * with the envelope's decay time; when the last key goes, what still sounds rings on through
     * the shared envelope's release. A new chord fades the old one out quickly. */
    void poly_on(int n) {
        int pick = -1;
        for (int i = 0; i < NOSC; i++) if (oheld[i] && onote[i] == n) pick = i;
        if (pick < 0) for (int i = 0; i < NOSC; i++) if (!oheld[i] && (pick < 0 || oage[i] < oage[pick])) pick = i;
        if (pick < 0) { pick = 0; for (int i = 1; i < NOSC; i++) if (oage[i] < oage[pick]) pick = i; }   /* all eight held: the oldest key goes */
        if (nheld <= 0) { nheld = 0; for (int i = 0; i < NOSC; i++) { ot[i] = 0; oslow[i] = false; } }   /* a new chord: the old one goes */
        if (!oheld[pick]) nheld++;
        oheld[pick] = true; onote[pick] = n; ot[pick] = 1.0f; oage[pick] = ++age;
        note_on(n, true);
    }
    void poly_off(int n) {
        for (int i = 0; i < NOSC; i++) {
            if (!oheld[i] || onote[i] != n) continue;
            oheld[i] = false; oage[i] = ++age; nheld--;
            ot[i] = 0; oslow[i] = true;
        }
        if (nheld <= 0) { nheld = 0; for (int i = 0; i < NOSC; i++) ot[i] = og[i]; note_off(); }
    }
    void all_off() {
        for (int i = 0; i < NOSC; i++) { oheld[i] = false; if (p.poly) ot[i] = og[i]; }
        nheld = 0;
        note_off();
    }
    void set_bend(float semitones) { bend = semitones; }
    bool idle() const { return stage == ENV_IDLE && !(rev_on() && !p.rev_pre && reverb.active()); }

    /* adds into L and R */
    void render(float *L, float *R, int n) {
        const bool rev = rev_on();
        if (stage == ENV_IDLE && !(rev && reverb.active())) return;
        const float norm = p.poly ? POLY_NORM : 0.35355339f;   /* mono: 1 / sqrt(8) */
        int pos = 0;
        while (pos < n) {
            if (ctl <= 0) { control(); ctl = CTL; }
            const int m = std::min(ctl, n - pos);
            float buf[CTL], dry[CTL], envb[CTL];
            if (stage != ENV_IDLE) {
                for (int s = 0; s < m; s++) buf[s] = 0;
                if (p.voice > 0) {
                    oscillators(buf, m);
                    if (delays(p.model)) {
                        for (int s = 0; s < m; s++) {
                            int r = dpos - dlen; if (r < 0) r += DELAY_MAX;
                            const float x = buf[s] * norm, d = dl[r];
                            dl[dpos] = x + DELAY_FB * d;
                            if (++dpos >= DELAY_MAX) dpos = 0;
                            buf[s] = (x + ch * d) * p.voice;
                        }
                    } else {
                        const float g = norm * p.voice;
                        for (int s = 0; s < m; s++) buf[s] *= g;
                    }
                }
                if (p.perc > 0 && penv > 1e-5f) {
                    for (int s = 0; s < m; s++) {
                        float lp, hp;
                        const float exc = rnd() * penv;
                        pbp.step(exc, pg, 0.5f, lp, hp);
                        pph += pinc; if (pph >= 1.0f) pph -= 1.0f;
                        buf[s] += p.perc * (2.5f * (exc - hp - lp) + 0.9f * penv * penv * fast_sin(pph));
                        penv *= pkd;
                    }
                }
                if (p.rev_fb > 0) for (int s = 0; s < m; s++) buf[s] += fbk[s];   /* mixer input 3: the reverb return */
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
                    f1.step(fast_tanh(buf[s]), g1, k1, lp, hp);
                    const float y1 = p.f1_hp ? hp : lp;
                    f2.step(fast_tanh(y1), g2, k2, lp, hp);
                    const float x = fast_tanh(lp);
                    envb[s] = env;
                    dry[s] = p.rev_pre ? x : x * env;
                }
                if (stage == ENV_IDLE) { f1.reset(); f2.reset(); pbp.reset(); }   /* no stale ringing at the next key */
            } else {
                for (int s = 0; s < m; s++) dry[s] = envb[s] = 0;
            }
            if (rev) {
                float send[CTL], wl[CTL], wr[CTL];
                for (int s = 0; s < m; s++) { send[s] = dry[s] * p.rev_in; wl[s] = wr[s] = 0; }
                reverb.process(send, wl, wr, m);
                for (int s = 0; s < m; s++) fbk[s] = fast_tanh(p.rev_fb * (wl[s] + wr[s]));   /* limiter in the feedback path */
                if (p.rev_pre) {
                    for (int s = 0; s < m; s++) {
                        const float v = envb[s] * p.volume;
                        L[pos + s] += (dry[s] + p.rev_level * wl[s]) * v; R[pos + s] += (dry[s] + p.rev_level * wr[s]) * v;
                    }
                } else {
                    for (int s = 0; s < m; s++) {
                        L[pos + s] += (dry[s] + p.rev_level * wl[s]) * p.volume; R[pos + s] += (dry[s] + p.rev_level * wr[s]) * p.volume;
                    }
                }
            } else {
                for (int s = 0; s < m; s++) {
                    const float y = dry[s] * (p.rev_pre ? envb[s] : 1.0f) * p.volume;
                    L[pos + s] += y; R[pos + s] += y;
                }
                if (stage == ENV_IDLE) break;
            }
            pos += m; ctl -= m;
        }
    }

private:
    Patch p;
    float sr = 44100;
    float phase[NOSC], inc[NOSC];
    float phase2[NOSC], pw_ph[NOSC], pw[NOSC], gph[NOSC], ginc[NOSC], nval[NOSC];
    bool gon[NOSC];
    float ph9 = 0, inc9 = 0;
    float dl[DELAY_MAX];
    int dpos = 0, dlen = 1;
    Svf f1, f2, pbp;
    Reverb reverb;
    float fbk[CTL];
    float og[NOSC], ot[NOSC], ovca = 0.01f;   /* POLY: the VCA of each VCO, and its target */
    int onote[NOSC], nheld = 0;
    uint32_t oage[NOSC], age = 0;
    bool oheld[NOSC], oslow[NOSC];
    int sidx = 0;
    float env = 0, ka = 0, kd = 0, k1 = 2, k2 = 2;
    float g1 = 0.1f, g2 = 0.1f, dg1 = 0, dg2 = 0;
    float sp = 0, ch = 0, fgain = 1;
    float penv = 0, pkd = 0, pg = 0.1f, pph = 0, pinc = 0;
    float lph = 0, linc = 0, lfo = 0, lfo_s = 0, lslew = 1, r_prev = 0, r_next = 0;
    float note = 60, bend = 0;
    uint32_t rng = 0x2f6e2b1u;
    int stage = ENV_IDLE, ctl = 0;
    bool gate = false, snap = true;

    bool rev_on() const { return p.rev_level > 0 || p.rev_fb > 0; }
    inline float rnd() {   /* -1 .. 1 */
        rng = rng * 1664525u + 1013904223u;
        return (float)(int32_t)rng * (1.0f / 2147483648.0f);
    }
    float cutoff_g(float oct) const {
        const float fc = clampf(20.0f * fast_exp2(oct), 20.0f, std::min(18000.0f, 0.45f * sr));
        return std::tan(3.14159265f * fc / sr);
    }
    /* the LFO, once per control tick; -1 .. 1 */
    void lfo_tick() {
        lph += linc;
        const bool wrap = lph >= 1.0f;
        if (wrap) lph -= 1.0f;
        switch (p.lfo_shape) {
        case LFO_TRI: lfo = 1.0f - 4.0f * std::fabs(lph - 0.5f); break;
        case LFO_SINE: lfo = fast_sin(lph); break;
        case LFO_RAMP_DOWN: lfo = 1.0f - 2.0f * lph; break;
        case LFO_RAMP_UP: lfo = 2.0f * lph - 1.0f; break;
        case LFO_RANDOM1: if (wrap) lfo = rnd(); break;                                   /* a new step every cycle */
        case LFO_RANDOM2:                                                                 /* gliding between random points */
            if (wrap) { r_prev = r_next; r_next = rnd(); }
            lfo = r_prev + (r_next - r_prev) * lph;
            break;
        case LFO_RANDOM3: if (wrap && rnd() > 0.0f) lfo = rnd(); break;                   /* steps at irregular times */
        default: if (wrap) sidx = (sidx + 1) & 7; lfo = p.seq[sidx]; break;               /* SEQ: the eight step values, at LFO speed */
        }
        lfo_s += (lfo - lfo_s) * lslew;
    }
    /* every CTL samples: LFO, pitch and spread -> the increments, character, filter targets */
    void control() {
        lfo_tick();
        const float a = snap ? 1.0f : 0.06f;
        sp += (p.spread - sp) * a;
        ch += (p.character - ch) * a;
        float sc = sp;
        for (float e = 1; e < SPREAD_EXP - 0.5f; e += 1) sc *= sp;      /* integer exponent 3 or 4 */
        /* at the very bottom of SPREAD the eight keep a small irregular detune (a few cents): frozen
         * on one frequency they could cancel, and evenly spaced they would swell only once in seconds */
        const float jit = std::max(0.0f, SPREAD_MIN - sc);
        static const float JIT[NOSC] = {0.0f, 1.0f, -1.6f, 2.7f, -3.4f, 4.6f, -5.3f, 6.1f};
        float fm = 0;
        if (p.fm > 0) fm = p.fm * ((p.fm_src != FM_ENV ? lfo : 0.0f) + (p.fm_src != FM_LFO ? env : 0.0f));
        const float tune = (p.semis + bend - 69.0f) * (1.0f / 12.0f) + fm;
        const float f0 = 440.0f * fast_exp2(note * (1.0f / 12.0f) + tune) / sr;
        static const float INH[NOSC] = {0.0f, 1.0f, 2.6390158f, 4.6555367f, 6.9644045f, 9.5182697f, 12.286436f, 15.245345f};   /* i^1.4 */
        const bool h = harmonic(p.model);
        if (p.poly) {
            for (int i = 0; i < NOSC; i++) inc[i] = std::min(440.0f * fast_exp2((float)onote[i] * (1.0f / 12.0f) + tune) / sr, 0.45f);
        } else {
            for (int i = 0; i < NOSC; i++) { inc[i] = std::min(f0 * (1.0f + sc * (h ? (float)i : INH[i]) + jit * JIT[i]), 0.45f); ot[i] = 1.0f; }
        }
        inc9 = std::min(f0 * METAL_RATIO, 0.45f);
        fgain = 1.0f + 7.0f * ch * ch;
        if (p.model == MODEL_CHIPTUNE) {
            for (int i = 0; i < NOSC; i++) {
                pw_ph[i] += (0.31f + 0.17f * (float)i) * (float)CTL / sr;
                if (pw_ph[i] >= 1.0f) pw_ph[i] -= 1.0f;
                pw[i] = 0.5f + 0.45f * ch * (1.0f - 4.0f * std::fabs(pw_ph[i] - 0.5f));
            }
        }
        const float t1 = cutoff_g(p.f1_oct + p.f1_mod * env), t2 = cutoff_g(p.f2_oct + p.f2_mod * lfo_s);
        if (snap) { g1 = t1; g2 = t2; dg1 = dg2 = 0; snap = false; }
        else { dg1 = (t1 - g1) * (1.0f / CTL); dg2 = (t2 - g2) * (1.0f / CTL); }
        if (not_finite(f1.ic1 + f1.ic2 + f2.ic1 + f2.ic2 + pbp.ic1 + pbp.ic2)) { f1.reset(); f2.reset(); pbp.reset(); }
    }
    static inline float square(float ph, float d) {
        float q = ph + 0.5f; if (q >= 1.0f) q -= 1.0f;
        return (ph < 0.5f ? 1.0f : -1.0f) + blep(ph, d) - blep(q, d);
    }
    void oscillators(float *buf, int m) {
        const bool folding = fgain > 1.001f;
        const float fg = fgain;
        float mod[CTL];
        if (p.model == MODEL_METAL) {
            const float depth = 1.5f * ch * ch;
            for (int s = 0; s < m; s++) { ph9 += inc9; if (ph9 >= 1.0f) ph9 -= 1.0f; mod[s] = depth * fast_sin(ph9) + 4.0f; }
        }
        for (int i = 0; i < NOSC; i++) {
            if (ot[i] <= 0 && og[i] < 1e-4f) { og[i] = 0; continue; }   /* POLY: a VCO without a key rests */
            float tmp[CTL];
            float ph = phase[i];
            const float d = inc[i];
            switch (p.model) {
            case MODEL_ORGAN:
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    const float x = fast_sin(ph);
                    tmp[s] = folding ? fold(fg * x) : x;
                }
                break;
            case MODEL_STRINGS:
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    const float x = 2.0f * ph - 1.0f - blep(ph, d);
                    tmp[s] = folding ? fold(fg * x) : x;
                }
                break;
            case MODEL_DRONE:
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    const float x = 1.0f - 4.0f * std::fabs(ph - 0.5f);
                    tmp[s] = folding ? fold(fg * x) : x;
                }
                break;
            case MODEL_REED: {
                float ph2 = phase2[i];
                const float d2 = std::min(d * REED_DETUNE, 0.45f);
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    ph2 += d2; if (ph2 >= 1.0f) ph2 -= 1.0f;
                    const float x = 0.5f * (square(ph, d) + square(ph2, d2));
                    tmp[s] = folding ? fold(fg * x) : x;
                }
                phase2[i] = ph2;
                break;
            }
            case MODEL_METAL:
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    float t = ph + mod[s];
                    t -= (float)(int)t;
                    tmp[s] = fast_sin(t);
                }
                break;
            case MODEL_CHIPTUNE: {
                const float w = pw[i], dc = 2.0f * w - 1.0f;
                for (int s = 0; s < m; s++) {
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    float q = ph - w; if (q < 0.0f) q += 1.0f;
                    tmp[s] = (ph < w ? 1.0f : -1.0f) + blep(ph, d) - blep(q, d) - dc;
                }
                break;
            }
            case MODEL_GRAINS: {
                float g = gph[i];
                for (int s = 0; s < m; s++) {
                    g += ginc[i];
                    if (g >= 1.0f) {                    /* next wavelet: random length, one in four is a gap */
                        g = 0; ph = 0;
                        gon[i] = rnd() > -0.5f;
                        ginc[i] = GRAIN_HZ / sr * (1.0f + 0.4f * rnd());
                    }
                    ph += d; if (ph >= 1.0f) ph -= 1.0f;
                    float c = g + 0.25f; if (c >= 1.0f) c -= 1.0f;
                    tmp[s] = gon[i] ? 1.6f * (0.5f - 0.5f * fast_sin(c)) * fast_sin(ph) : 0.0f;
                }
                gph[i] = g;
                break;
            }
            default: {   /* MODEL_NOISE */
                const float dn = std::min(d * NOISE_RATE, 0.5f);
                float v = nval[i];
                for (int s = 0; s < m; s++) {
                    ph += dn; if (ph >= 1.0f) { ph -= 1.0f; v = rnd(); }
                    tmp[s] = v;
                }
                nval[i] = v;
                break;
            }
            }
            phase[i] = ph;
            float g = og[i];
            const float t = ot[i];
            if (g == 1.0f && t == 1.0f) { for (int s = 0; s < m; s++) buf[s] += tmp[s]; }
            else {
                const float c = t > g ? 1.0f - ovca : oslow[i] ? kd : 1.0f - 0.25f * ovca;   /* open: 4 ms; fade: decay time or 16 ms */
                for (int s = 0; s < m; s++) { g = t + (g - t) * c; buf[s] += tmp[s] * g; }
                og[i] = std::fabs(t - g) < 1e-4f ? t : g;
            }
        }
    }
};

}   // namespace rattler
