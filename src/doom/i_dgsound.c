// i_dgsound.c - Doom's sound and music modules for RadiumOS.
//
// Sound effects (DMX lumps, 8-bit unsigned) are mixed in software together with a small MUS synthesizer (square, pulse,
// saw and triangle voices plus noise drums, no GENMIDI instruments) into 44.1 kHz stereo, and streamed to the AC97 codec
// through rust_pcm_* (rust_lib/src/mp4.rs). The command loop calls DG_SoundPump() often enough to keep the codec fed.

#include <stdint.h>
#include <stddef.h>
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define MIX_RATE     44100
#define HALF_FRAMES  2048
#define SFX_CHANNELS 32
#define MUS_VOICES   24
#define MUS_TICK     315            // samples per MUS tick (140 Hz)

extern int rust_pcm_open(uint32_t rate);
extern int rust_pcm_ready(void);
extern void rust_pcm_write(const int16_t *pcm, uint32_t frames);
extern void rust_pcm_close(void);

static int g_open;
static int16_t g_buf[HALF_FRAMES * 2];

static int open_device(void)
{
    if (!g_open && rust_pcm_open(MIX_RATE) > 0) g_open = 1;
    return g_open;
}

static void close_device(void)
{
    if (g_open) rust_pcm_close();
    g_open = 0;
}

// ---- sound effects -------------------------------------------------------------------------------------------------

typedef struct {
    const uint8_t *data;
    uint32_t len, pos, step;
    int lgain, rgain;
    int active;
} sfx_voice_t;

static sfx_voice_t g_sfx[SFX_CHANNELS];
static int g_use_prefix;

static void set_gain(sfx_voice_t *v, int vol, int sep)
{
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    v->lgain = (254 - sep) * vol;
    v->rgain = sep * vol;
}

static boolean sfx_init(boolean use_sfx_prefix)
{
    g_use_prefix = use_sfx_prefix;
    return open_device();
}

static void sfx_shutdown(void)
{
    for (int i = 0; i < SFX_CHANNELS; i++) g_sfx[i].active = 0;
    close_device();
}

static int sfx_lump(sfxinfo_t *sfx)
{
    char name[16];
    int n = 0;
    if (g_use_prefix) { name[n++] = 'd'; name[n++] = 's'; }
    for (int i = 0; sfx->name[i] && n < 14; i++) name[n++] = sfx->name[i];
    name[n] = 0;
    int lump = W_CheckNumForName(name);
    return lump >= 0 ? lump : W_GetNumForName("dspistol");
}

static void sfx_update(void);

static int sfx_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= SFX_CHANNELS) return -1;
    int lump = sfx->lumpnum;
    int len = W_LumpLength(lump);
    if (len < 8 + 32 + 1) return -1;
    const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);
    if (d[0] != 3 || d[1] != 0) return -1;
    uint32_t rate = d[2] | (d[3] << 8);
    uint32_t n = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    if (n > (uint32_t)len - 8) n = (uint32_t)len - 8;
    if (n <= 32 || rate < 4000 || rate > 48000) return -1;
    sfx_voice_t *v = &g_sfx[channel];
    v->data = d + 8 + 16;
    v->len = n - 32;
    v->pos = 0;
    v->step = (rate << 16) / MIX_RATE;
    set_gain(v, vol, sep);
    v->active = 1;
    return channel;
}

static void sfx_params(int channel, int vol, int sep)
{
    if (channel >= 0 && channel < SFX_CHANNELS) set_gain(&g_sfx[channel], vol, sep);
}

static void sfx_stop(int channel)
{
    if (channel >= 0 && channel < SFX_CHANNELS) g_sfx[channel].active = 0;
}

static boolean sfx_playing(int channel)
{
    return channel >= 0 && channel < SFX_CHANNELS && g_sfx[channel].active;
}

static snddevice_t sfx_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
                                     SNDDEVICE_ADLIB, SNDDEVICE_PCSPEAKER, SNDDEVICE_GENMIDI };

sound_module_t DG_sound_module = {
    sfx_devices, sizeof(sfx_devices) / sizeof(sfx_devices[0]),
    sfx_init, sfx_shutdown, sfx_lump, sfx_update, sfx_params, sfx_start, sfx_stop, sfx_playing, NULL,
};

// ---- MUS synthesizer -----------------------------------------------------------------------------------------------

enum { W_PULSE, W_SQUARE, W_SAW, W_TRI, W_NOISE, W_DRUM };
enum { E_ATTACK, E_DECAY, E_RELEASE, E_PERC };

typedef struct {
    uint8_t on, ch, note, wave, stage, shift;
    uint16_t gain;
    int32_t env, sus, att;
    uint32_t phase, inc, floor_inc, noise;
    int32_t prev;
} voice_t;

static struct {
    const uint8_t *data;
    uint32_t pos, start, end;
    int delay, tick_left;
    int playing, paused, looping;
    int mvol;
    uint8_t program[16], volume[16], vel[16];
    voice_t v[MUS_VOICES];
} g_mus = { .mvol = 100 };

// pitch of notes 0..11 in octave 0 (8.18 Hz for note 0), q16
static const uint32_t BASE_Q16[12] = { 535809, 567665, 601393, 637118, 675070, 715238, 757759, 802816, 850540, 901128, 954771, 1011474 };

static uint32_t note_inc(int note)
{
    if (note > 127) note = 127;
    uint32_t f = BASE_Q16[note % 12] << (note / 12);          // Hz, q16 (at most ~12.5 kHz)
    return (uint32_t)(((uint64_t)f * 97392) >> 16);          // 2^32 / MIX_RATE in q16
}

static int wave_for(int program)
{
    if (program < 8) return W_PULSE;
    if (program < 16) return W_TRI;
    if (program < 24) return W_SQUARE;
    if (program < 32) return W_SAW;
    if (program < 40) return W_TRI;
    if (program < 56) return W_SAW;
    if (program < 72) return W_PULSE;
    if (program < 80) return W_TRI;
    if (program < 88) return W_SQUARE;
    return W_SAW;
}

static void voice_off(int ch, int note)
{
    for (int i = 0; i < MUS_VOICES; i++) {
        voice_t *v = &g_mus.v[i];
        if (v->on && v->ch == ch && v->note == note && v->stage != E_RELEASE && v->stage != E_PERC) v->stage = E_RELEASE;
    }
}

static void channel_off(int ch)
{
    for (int i = 0; i < MUS_VOICES; i++) {
        voice_t *v = &g_mus.v[i];
        if (v->on && v->ch == ch && v->stage != E_PERC) v->stage = E_RELEASE;
    }
}

static void note_on(int ch, int note, int vel)
{
    voice_t *v = NULL;
    for (int i = 0; i < MUS_VOICES; i++) if (!g_mus.v[i].on) { v = &g_mus.v[i]; break; }
    if (!v) {
        int32_t lowest = 0x7fffffff;
        for (int i = 0; i < MUS_VOICES; i++) {
            int32_t score = g_mus.v[i].env + (g_mus.v[i].stage == E_RELEASE ? 0 : 40000);
            if (score < lowest) { lowest = score; v = &g_mus.v[i]; }
        }
    }
    v->on = 1; v->ch = (uint8_t)ch; v->note = (uint8_t)note;
    v->gain = (uint16_t)(vel * g_mus.volume[ch] / 127);
    v->phase = 0; v->prev = 0; v->noise = 12345u + (uint32_t)note * 977u;
    v->inc = note_inc(note);
    if (ch == 15) {
        v->stage = E_PERC; v->env = 32767; v->sus = 0; v->att = 0;
        switch (note) {
        case 35: case 36: v->wave = W_DRUM; v->inc = note_inc(40); v->floor_inc = note_inc(16); v->shift = 11; break;
        case 41: case 43: case 45: case 47: case 48: case 50:
            v->wave = W_DRUM; v->inc = note_inc(note + 16); v->floor_inc = note_inc(note - 4); v->shift = 11; break;
        case 42: case 44: v->wave = W_NOISE; v->shift = 9; break;
        case 46: v->wave = W_NOISE; v->shift = 11; break;
        case 49: case 51: case 52: case 53: case 55: case 57: case 59: v->wave = W_NOISE; v->shift = 13; break;
        default: v->wave = W_NOISE; v->shift = 11; break;
        }
        return;
    }
    int program = g_mus.program[ch];
    v->wave = (uint8_t)wave_for(program);
    v->stage = E_ATTACK; v->env = 0;
    v->att = (program >= 40 && program < 56) ? 25 : 160;
    v->sus = 14000;
    v->shift = (program >= 40 && program < 56) || (program >= 16 && program < 24) ? 15 : 13;   // sustained vs plucked
}

static void mus_reset(void)
{
    for (int i = 0; i < MUS_VOICES; i++) g_mus.v[i].on = 0;
    for (int c = 0; c < 16; c++) { g_mus.program[c] = 0; g_mus.volume[c] = 100; g_mus.vel[c] = 100; }
    g_mus.pos = g_mus.start;
    g_mus.delay = 0;
    g_mus.tick_left = 0;
}

static void mus_tick(void)
{
    if (g_mus.delay > 0) { g_mus.delay--; if (g_mus.delay > 0) return; }
    for (int guard = 0; guard < 4096 && g_mus.playing; guard++) {
        if (g_mus.pos >= g_mus.end) { g_mus.playing = 0; return; }
        uint8_t b = g_mus.data[g_mus.pos++];
        int last = b & 0x80, type = (b >> 4) & 7, ch = b & 15;
        switch (type) {
        case 0: voice_off(ch, g_mus.data[g_mus.pos++] & 127); break;
        case 1: {
            uint8_t n = g_mus.data[g_mus.pos++];
            if (n & 0x80) g_mus.vel[ch] = g_mus.data[g_mus.pos++] & 127;
            note_on(ch, n & 127, g_mus.vel[ch]);
            break;
        }
        case 2: g_mus.pos++; break;
        case 3: { uint8_t c = g_mus.data[g_mus.pos++]; if (c == 10 || c == 11) channel_off(ch); break; }
        case 4: {
            uint8_t c = g_mus.data[g_mus.pos++], val = g_mus.data[g_mus.pos++];
            if (c == 0) g_mus.program[ch] = val & 127;
            else if (c == 3) g_mus.volume[ch] = val & 127;
            break;
        }
        case 5: break;
        default:   // 6: end of score
            if (g_mus.looping) { g_mus.pos = g_mus.start; } else { g_mus.playing = 0; return; }
            break;
        }
        if (last) {
            int d = 0;
            uint8_t c;
            do { c = g_mus.data[g_mus.pos++]; d = (d << 7) | (c & 127); } while ((c & 0x80) && g_mus.pos < g_mus.end);
            if (d > 0) { g_mus.delay = d; return; }
        }
    }
}

static int voice_sample(voice_t *v)
{
    int w;
    switch (v->wave) {
    case W_PULSE:  w = v->phase < 0x40000000u ? 14000 : -14000; break;
    case W_SQUARE: w = v->phase < 0x80000000u ? 11000 : -11000; break;
    case W_SAW:    w = ((int)(v->phase >> 16) - 32768) >> 1; break;
    case W_NOISE: {
        v->noise = v->noise * 1664525u + 1013904223u;
        int n = (int)(v->noise >> 16) - 32768;
        w = (n - v->prev) >> 1;
        v->prev = n;
        break;
    }
    default: {   // triangle (also the pitched drums)
        int t = (int)(v->phase >> 16);
        w = ((t < 32768 ? t : 65535 - t) * 2 - 32768) >> 1;
        break;
    }
    }
    v->phase += v->inc;
    switch (v->stage) {
    case E_ATTACK:
        v->env += v->att;
        if (v->env >= 32767) { v->env = 32767; v->stage = E_DECAY; }
        break;
    case E_DECAY:
        v->env -= (v->env - v->sus) >> v->shift;
        break;
    case E_RELEASE:
        v->env -= v->env >> 10;
        if (v->env < 40) v->on = 0;
        break;
    default:     // percussion
        v->env -= v->env >> v->shift;
        if (v->wave == W_DRUM && v->inc > v->floor_inc) v->inc -= (v->inc - v->floor_inc) >> 11;
        if (v->env < 48) v->on = 0;
        break;
    }
    return (((w * v->env) >> 15) * (int)v->gain) >> 9;
}

static boolean mus_init(void) { return open_device(); }
static void mus_shutdown(void) { g_mus.playing = 0; close_device(); }
static void mus_volume(int volume) { g_mus.mvol = volume < 0 ? 0 : volume > 127 ? 127 : volume; }
static void mus_pause(void) { g_mus.paused = 1; }
static void mus_resume(void) { g_mus.paused = 0; }

static void *mus_register(void *data, int len)
{
    const uint8_t *d = data;
    if (len < 16 || d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1a) return NULL;
    return data;
}

static void mus_unregister(void *handle) { (void)handle; }

static void mus_play(void *handle, boolean looping)
{
    if (!handle) return;
    const uint8_t *d = handle;
    uint32_t len = d[4] | (d[5] << 8), start = d[6] | (d[7] << 8);
    g_mus.data = d;
    g_mus.start = start;
    g_mus.end = start + len;
    g_mus.looping = looping;
    mus_reset();
    g_mus.playing = 1;
}

static void mus_stop(void)
{
    g_mus.playing = 0;
    for (int i = 0; i < MUS_VOICES; i++) g_mus.v[i].on = 0;
}

static boolean mus_is_playing(void) { return g_mus.playing; }

static void mus_poll(void);

static snddevice_t mus_devices[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
                                     SNDDEVICE_ADLIB, SNDDEVICE_GENMIDI };

music_module_t DG_music_module = {
    mus_devices, sizeof(mus_devices) / sizeof(mus_devices[0]),
    mus_init, mus_shutdown, mus_volume, mus_pause, mus_resume, mus_register, mus_unregister, mus_play, mus_stop, mus_is_playing, mus_poll,
};

// ---- mixing --------------------------------------------------------------------------------------------------------

static void mix(int16_t *out, int frames)
{
    int music_on = g_mus.playing && !g_mus.paused;
    for (int i = 0; i < frames; i++) {
        int l = 0, r = 0;
        for (int c = 0; c < SFX_CHANNELS; c++) {
            sfx_voice_t *v = &g_sfx[c];
            if (!v->active) continue;
            uint32_t idx = v->pos >> 16;
            if (idx >= v->len) { v->active = 0; continue; }
            int s0 = v->data[idx], s1 = idx + 1 < v->len ? v->data[idx + 1] : 128;
            int s = (s0 + (((s1 - s0) * (int)(v->pos & 0xffff)) >> 16) - 128) << 8;
            l += (s * v->lgain) >> 13;
            r += (s * v->rgain) >> 13;
            v->pos += v->step;
        }
        if (music_on) {
            if (--g_mus.tick_left <= 0) { g_mus.tick_left += MUS_TICK; mus_tick(); }
            int m = 0;
            for (int k = 0; k < MUS_VOICES; k++) if (g_mus.v[k].on) m += voice_sample(&g_mus.v[k]);
            m = (m * g_mus.mvol) >> 7;
            l += m; r += m;
        }
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}

void DG_SoundPump(void)
{
    if (!g_open) return;
    for (int n = 0; n < 2 && rust_pcm_ready(); n++) {
        mix(g_buf, HALF_FRAMES);
        rust_pcm_write(g_buf, HALF_FRAMES);
    }
}

static void sfx_update(void) { DG_SoundPump(); }
static void mus_poll(void) { DG_SoundPump(); }

// Called by the command when Doom is left, however that happened.
void DG_SoundReset(void)
{
    close_device();
    for (int i = 0; i < SFX_CHANNELS; i++) g_sfx[i].active = 0;
    mus_stop();
    g_mus.playing = 0;
    g_mus.paused = 0;
}
