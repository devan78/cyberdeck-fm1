/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 devan78 (cyberdeck-fm1) */
/* NEON (cyberdeck-fm1, engine 14): the darksynth bass. One to six band-limited saws, detuned around the note so they
 * beat against each other (two: the Reese; six: a supersaw), a sine an octave below (SUB, clean: added after the
 * drive), DRV a tanh drive and FOLD a two-fold wavefolder before the filter (the growl), then the state-variable
 * low-pass with RES and key tracking, as ANALOG's.
 *
 *   SAWS 1..6  saws, spread evenly over -DTN .. +DTN (an odd count keeps one on the note), summed at 1 / sqrt(n)
 *   DTN        the spread: DTN^2 / 127^2 x 60 cents at the outer saws (fine at the bottom of the knob, where a Reese
 *              lives: 32 ~ 3.8 ct, 64 ~ 15 ct, 127 = 60 ct)
 *   SUB        the sub sine's level (an octave below the note, from its own phase)
 *   DRV        drive 1x .. 4x into a tanh (always on: at 0 it rounds the peaks of a big stack)
 *   CUT RES    the low-pass (ENV / LFO / matrix move CUT as on every engine)
 *   FOLD       a wavefolder after the drive, 1x .. 3x: the peaks fold back (two reflections cover 3x)
 *   KTR        key tracking of the cutoff (64: half, as ANALOG's)
 *
 * Phases: saws 1..3 in ph[0..2], saws 4..6 in s[4..6], the sub in s[7]; the filter in s[0], s[1]. A note starts its
 * saws at phases spread from the voice's age (no two notes start alike, as free-running oscillators); a retrigger
 * keeps them (keep: s[0], s[1], s[4..7]), so legato and MONO bass lines run on without a click. The sub starts at 0.
 * Voices: four a part (poly): a six-saw voice costs about two ANALOG voices. */
#define NEON_MAXSAW 6
static const int32_t NEON_NORM[NEON_MAXSAW + 1] = {0, 32767, 23170, 18919, 16384, 14654, 13377};   /* 1 / sqrt(n), Q15 */

static void neon_note_on(track_t *t, voice_t *v)
{
    uint32_t x = v->age * 2654435761u + 0x9E3779B9u;   /* the start phases: a hash of the voice's age */
    (void)t;
    v->ph[0] = x;
    v->ph[1] = x * 3u + 0x55555555u;
    v->ph[2] = x * 5u + 0xAAAAAAAAu;
    v->s[4] = (int32_t)(x * 7u + 0x2AAAAAAAu);
    v->s[5] = (int32_t)(x * 11u + 0x80000000u);
    v->s[6] = (int32_t)(x * 13u + 0xD5555555u);
    v->s[7] = 0;                                      /* the sub from its zero crossing */
    v->s[0] = v->s[1] = 0;                            /* the filter */
}

static void neon_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t ns = clamp(p[P_E0], 1, NEON_MAXSAW), det = p[P_E1];
    int32_t sub = p[P_E2] * 220;                      /* 0 .. ~0.85, Q15 */
    int32_t drive = 32768 + p[P_E3] * 774;            /* 1x .. 4x, Q15 */
    int32_t fold = p[P_E6], fgain = 32768 + fold * 516;   /* 1x .. 3x, Q15 */
    int32_t cut = (p[P_E4] << 8) + m->cutoff + (p[P_E7] * (v->pitch16 - 60 * 16) >> 4);
    int32_t norm = NEON_NORM[ns];
    int32_t fmax = det * det * 142 / 16129;           /* the outer saws' detune: 0 .. 142 (1/4096: 60 ct) */
    uint32_t ph[NEON_MAXSAW], inc[NEON_MAXSAW], inc0 = m->inc, sinc = m->inc >> 1, sph = (uint32_t)v->s[7], i;
    int32_t ic1 = v->s[0], ic2 = v->s[1], k;
    tsvf_t flt;
    ph[0] = v->ph[0]; ph[1] = v->ph[1]; ph[2] = v->ph[2];   /* state in locals: out[] may alias v->s[] */
    ph[3] = (uint32_t)v->s[4]; ph[4] = (uint32_t)v->s[5]; ph[5] = (uint32_t)v->s[6];
    for (k = 0; k < ns; k++) {                        /* -fmax .. +fmax, evenly; one saw: on the note */
        int32_t f = ns > 1 ? fmax * (2 * k - (ns - 1)) / (ns - 1) : 0;
        inc[k] = inc0 + (uint32_t)((int32_t)(inc0 >> 12) * f);
    }
    tsvf_coef(&flt, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t s = 0, y;
        for (k = 0; k < ns; k++) {
            s += osc_saw(ph[k], inc[k]);
            ph[k] += inc[k];
        }
        s = clamp(mulq15(s, norm), -65535, 65535);    /* the stack at about one saw's level (its peaks up to sqrt(n)) */
        s = softclip(((s >> 2) * (drive >> 2)) >> 11);   /* the drive: |s| < 2^16 and drive < 2^17, no overflow */
        if (fold) {                                   /* fold the peaks back into +-32767 (up to 3x: two reflections) */
            s = (s * (fgain >> 4)) >> 11;
            if (s > 32767) s = 65534 - s;
            else if (s < -32767) s = -65534 - s;
            if (s > 32767) s = 65534 - s;
            else if (s < -32767) s = -65534 - s;
        }
        if (sub) {
            s += mulq15(sine_i(sph), sub);
            sph += sinc;
        }
        y = tsvf_lp(&flt, s >> 1, &ic1, &ic2);        /* the low-pass, then ANALOG's soft knee: only resonant peaks clip */
        s = soft_knee(y, 16000) * 2;
        out[i] += voice_amp(s, m, i) * 2;
    }
    v->ph[0] = ph[0]; v->ph[1] = ph[1]; v->ph[2] = ph[2];
    v->s[4] = (int32_t)ph[3]; v->s[5] = (int32_t)ph[4]; v->s[6] = (int32_t)ph[5];
    v->s[7] = (int32_t)sph;
    v->s[0] = ic1;
    v->s[1] = ic2;
}

/* {SAWS DTN SUB DRV CUT RES FOLD KTR}, {ATK DEC SUS REL}, ENV -> FILTER, MONO, FX(DIST CHORUS DELAY REVERB), pattern */
static const preset_t NEON_PRESETS[] = {
    {"REESE",      {2, 46, 56, 30, 54, 22, 0, 40},  {0, 70, 112, 26}, 10, 1, FX(8, 0, 10, 16), PAT(2)},
    {"NEON CITY",  {6, 62, 0, 12, 92, 14, 0, 64},   {2, 80, 100, 56}, 16, 0, FX(0, 34, 40, 46), PAT(4)},
    {"CHROME JAW", {3, 30, 34, 92, 46, 72, 62, 50}, {0, 52, 60, 20},  40, 1, FX(28, 0, 18, 10), PAT(1)},
    {"BLACK ICE",  {2, 22, 112, 20, 34, 8, 0, 30},  {0, 60, 100, 22}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"NIGHT DRIVE",{2, 16, 62, 22, 40, 36, 0, 64},  {0, 44, 30, 24},  46, 1, FX(0, 0, 16, 10), PAT(2)},
    {"DATA SPIKE", {4, 36, 0, 30, 30, 50, 20, 80},  {0, 40, 0, 40},   56, 0, FX(0, 20, 56, 30), PAT(13)},
    {"MEGACORP",   {5, 52, 20, 52, 60, 30, 30, 64}, {0, 56, 40, 36},  36, 0, FX(14, 10, 30, 36), PAT(9)},
    {"RAIN PAD",   {6, 42, 0, 0, 64, 10, 0, 32},    {86, 90, 110, 100}, 10, 0, FX(0, 60, 26, 82), PAT(5)},
};

static const engine_t ENG_NEON = {
    .name = "NEON",
    .page_title = {"OSC", "FLT"},
    .edit = {
        {"SAWS", F_INT, 1, NEON_MAXSAW, 2, 0, 0},
        {"DTN", F_INT, 0, 127, 46, 0, 0},
        {"SUB", F_PCT, 0, 127, 56, 0, 0},
        {"DRV", F_PCT, 0, 127, 30, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 54, 0, 0},
        {"RES", F_PCT, 0, 127, 22, 0, 0},
        {"FOLD", F_PCT, 0, 127, 0, 0, 0},
        {"KTR", F_PCT, 0, 127, 40, 0, 0},
    },
    .presets = NEON_PRESETS,
    .npresets = NELEM(NEON_PRESETS),
    .note_on = neon_note_on,
    .render = neon_render,
    .knob = {P_E4, P_E5, P_E1, P_E3},       /* HOME: CUT RES DTN DRV */
    .poly = 4,
    .keep = 0xF3,                           /* the filter (s[0], s[1]) and the phases in s[4..7] */
};
