// softgl.cpp - a software implementation of the OpenGL 1.1 fixed-function subset that AssaultCube draws with.
// Triangles are clipped against the near plane, rasterised with perspective-correct textures/colours/fog (the divide is done
// every 8 pixels), depth tested in a float buffer and blended into a 0x00RRGGBB frame buffer. No SSE: all maths is plain x87 float.

#include "GL/gl.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

namespace {

// ---- state ----------------------------------------------------------------------------------------------------------

struct Tex {
    int levels = 0, w[12] = {}, h[12] = {};
    uint32_t *px[12] = {};       // ARGB
    int minf = GL_NEAREST_MIPMAP_LINEAR, magf = GL_LINEAR, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    bool alpha = false, used = false;
};

struct Vert { float x, y, z, w, r, g, b, a, s, t; };      // as given by the application
struct CV { float x, y, z, w, r, g, b, a, s, t, ez; };    // clip space (+ eye depth for the fog)
struct SV { float x, y; float at[9]; };                    // screen space; at[]: z, 1/w, u/w, v/w, r/w, g/w, b/w, a/w, -ez/w

const int MAXTEX = 4096, STACK = 32;

struct State {
    uint32_t *fb = nullptr; int32_t *zb = nullptr; int fw = 0, fh = 0;
    int vx = 0, vy = 0, vw = 0, vh = 0, sx = 0, sy = 0, sw = 0, sh = 0;
    bool depthTest = false, blend = false, cull = false, fog = false, scissor = false, texOn = false, alphaTest = false, polyOff = false;
    int depthFunc = GL_LESS, cullFace = GL_BACK, frontFace = GL_CCW, srcF = GL_ONE, dstF = GL_ZERO, alphaFunc = GL_ALWAYS;
    bool depthMask = true, cmR = true, cmG = true, cmB = true;
    float alphaRef = 0, offFactor = 0, offUnits = 0;
    float clearC[4] = {0, 0, 0, 0}; float clearD = 1;
    int fogMode = GL_EXP; float fogDensity = 1, fogStart = 0, fogEnd = 1, fogCol[4] = {0, 0, 0, 0};
    int texEnv = GL_MODULATE;
    // GL_ARB_texture_env_combine (unit 0 only): [0] = RGB, [1] = alpha
    int cmbFn[2] = { GL_MODULATE, GL_MODULATE }, cmbSrc[2][3] = { { GL_TEXTURE, GL_PREVIOUS_ARB, GL_CONSTANT_ARB }, { GL_TEXTURE, GL_PREVIOUS_ARB, GL_CONSTANT_ARB } };
    int cmbOp[2][3] = { { GL_SRC_COLOR, GL_SRC_COLOR, GL_SRC_ALPHA }, { GL_SRC_ALPHA, GL_SRC_ALPHA, GL_SRC_ALPHA } }, cmbScale[2] = { 1, 1 };
    int envColor[4] = { 0, 0, 0, 0 };
    int matMode = GL_MODELVIEW;
    float mv[STACK][16], pj[STACK][16], tm[STACK][16]; int mvT = 0, pjT = 0, tmT = 0;
    float cur[4] = {1, 1, 1, 1}, curT[2] = {0, 0};
    float lineW = 1;
    unsigned boundTex = 0;
    int unpackAlign = 4, packAlign = 4, unpackRow = 0;
    // client arrays
    bool vaOn = false, caOn = false, taOn = false;
    int vaSize = 3, caSize = 4, taSize = 2; GLenum vaType = GL_FLOAT, caType = GL_FLOAT, taType = GL_FLOAT;
    int vaStride = 0, caStride = 0, taStride = 0; const uint8_t *vaP = nullptr, *caP = nullptr, *taP = nullptr;
    // immediate mode
    int primMode = -1; Vert *iv = nullptr; int ivN = 0, ivCap = 0;
    GLenum err = 0;
    Tex *tex[MAXTEX] = {};
    unsigned nextTex = 1, nextList = 1;
};

State S;
int g_dbg = 0;
struct Stats { unsigned calls, tris, culled, rejected, raster, pix, zfail, texOn, blend; } ST;

float *MV() { return S.mv[S.mvT]; }
float *PJ() { return S.pj[S.pjT]; }
float *TM() { return S.tm[S.tmT]; }

float *curMat()
{
    return S.matMode == GL_PROJECTION ? PJ() : (S.matMode == GL_TEXTURE ? TM() : MV());
}

void ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

void mulm(const float *a, const float *b, float *o)   // o = a * b (column major)
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r[c * 4 + rr] = a[rr] * b[c * 4] + a[4 + rr] * b[c * 4 + 1] + a[8 + rr] * b[c * 4 + 2] + a[12 + rr] * b[c * 4 + 3];
    memcpy(o, r, 64);
}

bool isIdent(const float *m)
{
    for (int i = 0; i < 16; i++) if (m[i] != ((i % 5) == 0 ? 1.0f : 0.0f)) return false;
    return true;
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// round-to-nearest float -> int without the x87 control-word switching of a C cast (|f| < 2^31)
inline int ftoi(float f) { union { double d; int32_t i[2]; } u; u.d = (double)f + 6755399441055744.0; return u.i[0]; }
const float ZSCALE = 1073741824.0f;   // depth 0..1 as fixed point
inline int32_t zFromFloat(float z) { return ftoi(clampf(z, -1.9f, 1.9f) * ZSCALE); }
inline uint8_t f2b(float v) { return (uint8_t)(int)(clampf(v, 0, 1) * 255.0f + 0.5f); }

// ---- textures -------------------------------------------------------------------------------------------------------

Tex *curTex()
{
    if (S.boundTex == 0 || S.boundTex >= MAXTEX) return nullptr;
    Tex *&t = S.tex[S.boundTex];
    if (!t) { t = new Tex(); t->used = true; }
    return t;
}

void freeLevels(Tex *t, int from)
{
    for (int i = from; i < 12; i++) { free(t->px[i]); t->px[i] = nullptr; t->w[i] = t->h[i] = 0; }
}

void buildMips(Tex *t)
{
    int n = 1;
    while (n < 12 && (t->w[n - 1] > 1 || t->h[n - 1] > 1)) {
        int w = t->w[n - 1] > 1 ? t->w[n - 1] / 2 : 1, h = t->h[n - 1] > 1 ? t->h[n - 1] / 2 : 1;
        uint32_t *src = t->px[n - 1], *dst = (uint32_t *)malloc((size_t)w * h * 4);
        int sw = t->w[n - 1], sh = t->h[n - 1];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < sw ? x0 + 1 : x0, y1 = y0 + 1 < sh ? y0 + 1 : y0;
                uint32_t p[4] = { src[y0 * sw + x0], src[y0 * sw + x1], src[y1 * sw + x0], src[y1 * sw + x1] };
                uint32_t o = 0;
                for (int sh2 = 0; sh2 < 32; sh2 += 8) o |= (((p[0] >> sh2 & 255) + (p[1] >> sh2 & 255) + (p[2] >> sh2 & 255) + (p[3] >> sh2 & 255) + 2) >> 2) << sh2;
                dst[y * w + x] = o;
            }
        t->px[n] = dst; t->w[n] = w; t->h[n] = h; n++;
    }
    t->levels = n;
}

bool wantsMips(const Tex *t) { return t->minf >= GL_NEAREST_MIPMAP_NEAREST && t->minf <= GL_LINEAR_MIPMAP_LINEAR; }

void refreshMips(Tex *t)
{
    if (t->levels == 0 || !t->px[0]) return;
    freeLevels(t, 1);
    t->levels = 1;
    if (wantsMips(t)) buildMips(t);
}

// converts application pixels to ARGB; rows may be padded to the unpack alignment
void convert(const void *data, GLsizei w, GLsizei h, GLenum format, GLenum type, uint32_t *out, bool *hasAlpha)
{
    int comps = format == GL_RGBA || format == GL_BGRA ? 4 : (format == GL_RGB || format == GL_BGR ? 3 : (format == GL_LUMINANCE_ALPHA ? 2 : 1));
    int rowLen = S.unpackRow ? S.unpackRow : w;
    size_t rowBytes = (size_t)rowLen * comps * (type == GL_FLOAT ? 4 : 1);
    rowBytes = (rowBytes + S.unpackAlign - 1) / S.unpackAlign * S.unpackAlign;
    *hasAlpha = comps == 4 || comps == 2 || format == GL_ALPHA;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = (const uint8_t *)data + y * rowBytes;
        for (int x = 0; x < w; x++) {
            uint8_t c[4] = {255, 255, 255, 255};
            if (type == GL_FLOAT) {
                const float *f = (const float *)row + x * comps;
                for (int i = 0; i < comps; i++) c[i] = f2b(f[i]);
            } else {
                for (int i = 0; i < comps; i++) c[i] = row[x * comps + i];
            }
            uint8_t r, g, b, a = 255;
            switch (format) {
            case GL_RGB: case GL_RGBA: r = c[0]; g = c[1]; b = c[2]; if (comps == 4) a = c[3]; break;
            case GL_BGR: case GL_BGRA: r = c[2]; g = c[1]; b = c[0]; if (comps == 4) a = c[3]; break;
            case GL_LUMINANCE: r = g = b = c[0]; break;
            case GL_LUMINANCE_ALPHA: r = g = b = c[0]; a = c[1]; break;
            case GL_ALPHA: r = g = b = 255; a = c[0]; break;
            default: r = g = b = c[0]; break;
            }
            out[y * w + x] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        }
    }
}

// ---- rasteriser -----------------------------------------------------------------------------------------------------

inline bool depthPass(int z, int old)
{
    switch (S.depthFunc) {
    case GL_LESS: return z < old;
    case GL_LEQUAL: return z <= old;
    case GL_GREATER: return z > old;
    case GL_GEQUAL: return z >= old;
    case GL_EQUAL: return z == old;
    case GL_NOTEQUAL: return z != old;
    case GL_NEVER: return false;
    default: return true;
    }
}

inline bool alphaPass(int a)
{
    int ref = (int)(S.alphaRef * 255.0f + 0.5f);
    switch (S.alphaFunc) {
    case GL_LESS: return a < ref;
    case GL_LEQUAL: return a <= ref;
    case GL_GREATER: return a > ref;
    case GL_GEQUAL: return a >= ref;
    case GL_EQUAL: return a == ref;
    case GL_NOTEQUAL: return a != ref;
    case GL_NEVER: return false;
    default: return true;
    }
}

inline int factor(int f, int sc, int dc, int sa)   // 0..256 for one colour channel
{
    switch (f) {
    case GL_ZERO: return 0;
    case GL_ONE: return 256;
    case GL_SRC_COLOR: return sc + (sc >> 7);
    case GL_ONE_MINUS_SRC_COLOR: return 256 - (sc + (sc >> 7));
    case GL_DST_COLOR: return dc + (dc >> 7);
    case GL_ONE_MINUS_DST_COLOR: return 256 - (dc + (dc >> 7));
    case GL_SRC_ALPHA: return sa + (sa >> 7);
    case GL_ONE_MINUS_SRC_ALPHA: return 256 - (sa + (sa >> 7));
    case GL_DST_ALPHA: return 256;
    case GL_ONE_MINUS_DST_ALPHA: return 0;
    default: return 256;
    }
}

inline uint32_t texel(const Tex *t, int lvl, int x, int y)
{
    int w = t->w[lvl], h = t->h[lvl];
    if (t->wrapS == GL_REPEAT) x = (w & (w - 1)) == 0 ? (x & (w - 1)) : ((x % w) + w) % w; else x = clampi(x, 0, w - 1);
    if (t->wrapT == GL_REPEAT) y = (h & (h - 1)) == 0 ? (y & (h - 1)) : ((y % h) + h) % h; else y = clampi(y, 0, h - 1);
    return t->px[lvl][y * w + x];
}

// u, v in 24.8 fixed-point texel units of level `lvl`
inline uint32_t sample(const Tex *t, int lvl, int u, int v, bool linear)
{
    if (!linear) return texel(t, lvl, u >> 8, v >> 8);
    u -= 128; v -= 128;
    int x = u >> 8, y = v >> 8, fx = u & 255, fy = v & 255;
    uint32_t a = texel(t, lvl, x, y), b = texel(t, lvl, x + 1, y), c = texel(t, lvl, x, y + 1), d = texel(t, lvl, x + 1, y + 1);
    uint32_t o = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int top = (int)(a >> sh & 255) * (256 - fx) + (int)(b >> sh & 255) * fx;
        int bot = (int)(c >> sh & 255) * (256 - fx) + (int)(d >> sh & 255) * fx;
        o |= (uint32_t)(((top >> 8) * (256 - fy) + (bot >> 8) * fy) >> 8) << sh;
    }
    return o;
}

inline int log2i(float x)   // floor(log2(x) + 0.5) for x > 0
{
    union { float f; uint32_t i; } u; u.f = x;
    int e = (int)(u.i >> 23) - 127;
    return e + ((u.i & 0x7FFFFF) > 0x3504F3 ? 1 : 0);
}

struct Px { int c[4]; };   // r, g, b, a in 0..255

// one GL_COMBINE evaluation: which = 0 for the RGB part, 1 for the alpha part; tex/prim are texture and primary colour
static void combine(const Px &tex, const Px &prim, int which, int *out)
{
    int arg[3][3];
    for (int i = 0; i < 3; i++) {
        const Px *srcp = S.cmbSrc[which][i] == GL_TEXTURE ? &tex : (S.cmbSrc[which][i] == GL_CONSTANT_ARB ? nullptr : &prim);   // PREVIOUS of unit 0 is the primary colour
        Px cst; for (int k = 0; k < 4; k++) cst.c[k] = S.envColor[k];
        if (!srcp) srcp = &cst;
        int op = S.cmbOp[which][i];
        for (int k = 0; k < (which ? 1 : 3); k++) {
            int v = (op == GL_SRC_ALPHA || op == GL_ONE_MINUS_SRC_ALPHA) ? srcp->c[3] : srcp->c[which ? 3 : k];
            if (op == GL_ONE_MINUS_SRC_COLOR || op == GL_ONE_MINUS_SRC_ALPHA) v = 255 - v;
            arg[i][k] = v;
        }
    }
    for (int k = 0; k < (which ? 1 : 3); k++) {
        int a0 = arg[0][k], a1 = arg[1][k], a2 = arg[2][k], v;
        switch (S.cmbFn[which]) {
        case GL_REPLACE: v = a0; break;
        case GL_ADD: v = a0 + a1; break;
        case GL_ADD_SIGNED_ARB: v = a0 + a1 - 128; break;
        case GL_INTERPOLATE_ARB: v = (a0 * a2 + a1 * (255 - a2)) / 255; break;
        case GL_SUBTRACT_ARB: v = a0 - a1; break;
        default: v = (a0 * a1 + 127) / 255; break;
        }
        v *= S.cmbScale[which];
        out[which ? 3 : k] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
}

inline bool alphaPassR(int a, int ref)
{
    switch (S.alphaFunc) {
    case GL_LESS: return a < ref;
    case GL_LEQUAL: return a <= ref;
    case GL_GREATER: return a > ref;
    case GL_GEQUAL: return a >= ref;
    case GL_EQUAL: return a == ref;
    case GL_NOTEQUAL: return a != ref;
    case GL_NEVER: return false;
    default: return true;
    }
}

// lerp of two ARGB pixels, f in 0..255, both channel pairs at once
inline uint32_t lerp8(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t rb = (((a & 0x00FF00FFu) * (256 - f) + (b & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * (256 - f) + ((b >> 8) & 0x00FF00FFu) * f) & 0xFF00FF00u;
    return rb | ag;
}

// true when the current texture environment is a plain texture * colour (optionally scaled), the case nearly all of AC's world drawing uses
bool modulateEnv(int &esc, int &eas)
{
    if (S.texEnv == GL_MODULATE) { esc = eas = 1; return true; }
    if (S.texEnv != GL_COMBINE_ARB || S.cmbFn[0] != GL_MODULATE || S.cmbFn[1] != GL_MODULATE) return false;
    for (int w = 0; w < 2; w++) {
        int s0 = S.cmbSrc[w][0], s1 = S.cmbSrc[w][1];
        bool t0 = s0 == GL_TEXTURE, t1 = s1 == GL_TEXTURE;
        bool p0 = !t0 && s0 != GL_CONSTANT_ARB, p1 = !t1 && s1 != GL_CONSTANT_ARB;
        if (!((t0 && p1) || (p0 && t1))) return false;
        int opc = w ? GL_SRC_ALPHA : GL_SRC_COLOR;
        if (S.cmbOp[w][0] != opc || S.cmbOp[w][1] != opc) return false;
    }
    esc = S.cmbScale[0]; eas = S.cmbScale[1];
    return esc >= 1 && eas >= 1;
}

bool g_flat = false;       // set by drawTri: all three vertices share one colour
float g_fc[4];             // that colour (0..1)

struct RC {                // per-triangle constants for the span loops
    const SV *v0, *v1, *v2;
    float gx[9], gy[9];
    float s02, s01, s12;       // dx/dy of the three edges (0 for a flat edge)
    int ys, ye, cx0, cx1;
    const Tex *tex; bool linMag, mip;
    int fogR, fogG, fogB;
    bool flat; int fr, fg, fb, fa;
    int esc, eas, aref;
    bool dLess, cmAll, blSA;
};

#define DIVN(x) (n == full ? (x) >> fsh : (x) / n)

template <bool TEX, bool FOG, bool BLEND, bool ATEST, bool MOD>
void rows(const RC &c)
{
    const SV *v0 = c.v0, *v1 = c.v1, *v2 = c.v2;
    const float *gx = c.gx, *gy = c.gy;
    const Tex *tex = c.tex;
    const bool depthTest = S.depthTest, depthWrite = S.depthTest && S.depthMask, needA = BLEND || ATEST || !MOD;
    unsigned pixCount = 0;

    for (int y = c.ys; y < c.ye; y++) {
        float fy = y + 0.5f;
        float xa = v0->x + c.s02 * (fy - v0->y), xb;
        if (fy < v1->y) xb = c.s01 != 0 || v1->y - v0->y > 1e-6f ? v0->x + c.s01 * (fy - v0->y) : v1->x;
        else xb = c.s12 != 0 || v2->y - v1->y > 1e-6f ? v1->x + c.s12 * (fy - v1->y) : v2->x;
        float xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa;
        int xs = ftoi(xl), xe = ftoi(xr);
        if (xs < c.cx0) xs = c.cx0;
        if (xe > c.cx1) xe = c.cx1;
        if (xs >= xe) continue;

        float A[9], B[9];
        const float dx0 = xs + 0.5f - v0->x, dy0 = fy - v0->y;
        #define ATT(i) A[i] = v0->at[i] + gx[i] * dx0 + gy[i] * dy0
        ATT(0); ATT(1);
        if (TEX) { ATT(2); ATT(3); }
        if (!c.flat) { ATT(4); ATT(5); ATT(6); ATT(7); }
        if (FOG) ATT(8);
        #undef ATT
        uint32_t *row = S.fb + (size_t)y * S.fw;
        int32_t *zrow = S.zb + (size_t)y * S.fw;

        // values at the start of the current 8-pixel chunk (carried over from the end of the previous one)
        float w0 = 1.0f / A[1], u0 = A[2] * w0, v0t = A[3] * w0;
        int R0, G0, B0, AL0;
        if (c.flat) { R0 = c.fr << 16; G0 = c.fg << 16; B0 = c.fb << 16; AL0 = c.fa << 16; }
        else { R0 = ftoi(A[4] * w0 * 65536.0f); G0 = ftoi(A[5] * w0 * 65536.0f); B0 = ftoi(A[6] * w0 * 65536.0f); AL0 = ftoi(A[7] * w0 * 65536.0f); }
        if (!c.flat) { R0 = clampi(R0, 0, 255 << 16); G0 = clampi(G0, 0, 255 << 16); B0 = clampi(B0, 0, 255 << 16); }
        int F0 = 0;
        if (FOG) {
            float e0 = A[8] * w0, f0;
            if (S.fogMode == GL_LINEAR) { float rng = S.fogEnd - S.fogStart; if (rng == 0) rng = 1; f0 = (S.fogEnd - e0) / rng; }
            else { float d0 = S.fogDensity * e0; if (S.fogMode == GL_EXP2) d0 *= d0; f0 = expf(-d0); }
            F0 = ftoi((1.0f - clampf(f0, 0, 1)) * 65536.0f);
        }

        // texture level, pointers and filter for the whole row
        int lvl = 0; const uint32_t *tp = nullptr; int wm = 0, hm = 0, ws = 0; bool fastTex = false, lin = false; float rsw = 0, rsh = 0;
        if (TEX) {
            float tw = (float)tex->w[0], th = (float)tex->h[0];
            float rho = 1.0f;
            if (c.mip || c.linMag) {
                float dudx = (gx[2] - u0 * gx[1]) * w0 * tw, dvdx = (gx[3] - v0t * gx[1]) * w0 * th;
                float dudy = (gy[2] - u0 * gy[1]) * w0 * tw, dvdy = (gy[3] - v0t * gy[1]) * w0 * th;
                rho = fabsf(dudx) + fabsf(dvdx); float r2 = fabsf(dudy) + fabsf(dvdy);
                if (r2 > rho) rho = r2;
                if (c.mip && rho > 1.0f) { lvl = log2i(rho * 0.7f); if (lvl > tex->levels - 1) lvl = tex->levels - 1; if (lvl < 0) lvl = 0; }
            }
            int lw = tex->w[lvl], lh = tex->h[lvl];
            rsw = (float)lw * 256.0f; rsh = (float)lh * 256.0f;
            tp = tex->px[lvl]; wm = lw - 1; hm = lh - 1; ws = __builtin_ctz((unsigned)lw);
            fastTex = ((lw & (lw - 1)) | (lh & (lh - 1))) == 0 && tex->wrapS == GL_REPEAT && tex->wrapT == GL_REPEAT;
            lin = c.linMag && lvl == 0 && rho < 0.7f;
        }

        for (int x = xs; x < xe; ) {
            // pixels between perspective divides: as many as keep 1/w within ~15% (8, 16, 32 or 64)
            int full = 8, fsh = 3;
            { float g1 = fabsf(gx[1]); float lim = g1 > 1e-12f ? 0.15f * A[1] / g1 : 64.0f; if (lim >= 16) { full = 16; fsh = 4; } if (lim >= 32) { full = 32; fsh = 5; } if (lim >= 64) { full = 64; fsh = 6; } }
            int n = xe - x; if (n > full) n = full;
            B[1] = A[1] + gx[1] * n;
            if (TEX) { B[2] = A[2] + gx[2] * n; B[3] = A[3] + gx[3] * n; }
            if (!c.flat) { B[4] = A[4] + gx[4] * n; B[5] = A[5] + gx[5] * n; B[6] = A[6] + gx[6] * n; B[7] = A[7] + gx[7] * n; }
            if (FOG) B[8] = A[8] + gx[8] * n;
            float w1 = 1.0f / (B[1] > 1e-12f ? B[1] : 1e-12f);
            float u1 = B[2] * w1, v1t = B[3] * w1;

            int U0 = 0, V0 = 0, dU = 0, dV = 0;
            if (TEX) {
                U0 = ftoi(u0 * rsw); V0 = ftoi(v0t * rsh);
                int U1 = ftoi(u1 * rsw), V1 = ftoi(v1t * rsh);
                dU = DIVN(U1 - U0); dV = DIVN(V1 - V0);
            }
            int R1 = R0, G1 = G0, B1 = B0, AL1 = AL0, dR = 0, dG = 0, dB = 0, dA = 0;
            if (!c.flat) {
                R1 = ftoi(B[4] * w1 * 65536.0f); G1 = ftoi(B[5] * w1 * 65536.0f); B1 = ftoi(B[6] * w1 * 65536.0f); AL1 = ftoi(B[7] * w1 * 65536.0f);
                R1 = clampi(R1, 0, 255 << 16); G1 = clampi(G1, 0, 255 << 16); B1 = clampi(B1, 0, 255 << 16);
                dR = DIVN(R1 - R0); dG = DIVN(G1 - G0); dB = DIVN(B1 - B0); dA = DIVN(AL1 - AL0);
            }
            int F1 = 0, dF = 0;
            if (FOG) {
                float e1 = B[8] * w1, f1;
                if (S.fogMode == GL_LINEAR) { float rng = S.fogEnd - S.fogStart; if (rng == 0) rng = 1; f1 = (S.fogEnd - e1) / rng; }
                else { float d1 = S.fogDensity * e1; if (S.fogMode == GL_EXP2) d1 *= d1; f1 = expf(-d1); }
                F1 = ftoi((1.0f - clampf(f1, 0, 1)) * 65536.0f); dF = DIVN(F1 - F0);
            }
            int z = zFromFloat(A[0] + gx[0] * (x - xs)), dz = ftoi(gx[0] * ZSCALE);
            const bool fogOn = FOG && (F0 > 1024 || F1 > 1024);
            int U = U0, V = V0, R = R0, G = G0, Bc = B0, Al = AL0, F = F0;

            // the common case, a lit textured wall or floor: nearest texel, plain depth test (fewer live values than the general loop)
            if (TEX && MOD && !BLEND && !ATEST && fastTex && !lin && c.cmAll && depthWrite && c.dLess) {
                const int er = c.esc * 257;
                uint32_t *rp = row + x; int32_t *zp = zrow + x;
                int zz = z, uu = U0, vv = V0, rr = R0, gg = G0, bb = B0, ff = F0 < 0 ? 0 : (F0 > 65536 ? 65536 : F0);
                const int fr = c.fogR, fg = c.fogG, fb = c.fogB;
                for (int i = 0; i < n; i++, zz += dz, uu += dU, vv += dV, rr += dR, gg += dG, bb += dB, ff += dF) {
                    if (zz >= zp[i]) continue;
                    uint32_t t = tp[(((vv >> 8) & hm) << ws) | ((uu >> 8) & wm)];
                    int r = ((int)(t >> 16 & 255) * ((rr >> 16) * er) + 32768) >> 16, g = ((int)(t >> 8 & 255) * ((gg >> 16) * er) + 32768) >> 16, b = ((int)(t & 255) * ((bb >> 16) * er) + 32768) >> 16;
                    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
                    if (FOG && fogOn) {
                        int f = (ff < 0 ? 0 : (ff > 65536 ? 65536 : ff)) >> 8;
                        r += (fr - r) * f >> 8; g += (fg - g) * f >> 8; b += (fb - b) * f >> 8;
                    }
                    rp[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                    zp[i] = zz;
                    pixCount++;
                }
                A[1] = B[1];
                if (TEX) { A[2] = B[2]; A[3] = B[3]; }
                if (!c.flat) { A[4] = B[4]; A[5] = B[5]; A[6] = B[6]; A[7] = B[7]; }
                if (FOG) A[8] = B[8];
                w0 = w1; u0 = u1; v0t = v1t; R0 = R1; G0 = G1; B0 = B1; AL0 = AL1; F0 = F1;
                x += n;
                continue;
            }

            for (int i = 0; i < n; i++, z += dz, U += dU, V += dV, R += dR, G += dG, Bc += dB, Al += dA, F += dF) {
                int px = x + i;
                if (depthTest) { int zr = zrow[px]; if (!(c.dLess ? z < zr : depthPass(z, zr))) continue; }
                int r, g, b, al = 255;
                if (c.flat) { r = c.fr; g = c.fg; b = c.fb; if (needA) al = c.fa; }
                else { r = clampi(R >> 16, 0, 255); g = clampi(G >> 16, 0, 255); b = clampi(Bc >> 16, 0, 255); if (needA) al = clampi(Al >> 16, 0, 255); }
                if (MOD) {
                    if (TEX) {
                        uint32_t t;
                        if (fastTex) {
                            if (lin) {
                                int uu = U - 128, vv = V - 128, tx = uu >> 8, ty = vv >> 8;
                                uint32_t fx = uu & 255, fyy = vv & 255;
                                uint32_t a = tp[((ty & hm) << ws) | (tx & wm)], b2 = tp[((ty & hm) << ws) | ((tx + 1) & wm)];
                                uint32_t c2 = tp[(((ty + 1) & hm) << ws) | (tx & wm)], d2 = tp[(((ty + 1) & hm) << ws) | ((tx + 1) & wm)];
                                t = lerp8(lerp8(a, b2, fx), lerp8(c2, d2, fx), fyy);
                            } else t = tp[(((V >> 8) & hm) << ws) | ((U >> 8) & wm)];
                        } else t = sample(tex, lvl, U, V, lin);
                        r = r * (int)(t >> 16 & 255); g = g * (int)(t >> 8 & 255); b = b * (int)(t & 255);
                        r = (r * c.esc * 257 + 32768) >> 16; g = (g * c.esc * 257 + 32768) >> 16; b = (b * c.esc * 257 + 32768) >> 16;
                        if (needA) al = (al * (int)(t >> 24) + 127) / 255 * c.eas;
                    } else {
                        if (needA) al *= c.eas;
                        if (c.esc != 1) { r *= c.esc; g *= c.esc; b *= c.esc; }
                    }
                    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
                    if (needA && al > 255) al = 255;
                } else if (S.texEnv == GL_COMBINE_ARB) {
                    Px tp2 = {{255, 255, 255, 255}}, pp = {{r, g, b, al}};
                    if (TEX) { uint32_t t = sample(tex, lvl, U, V, lin); tp2.c[0] = t >> 16 & 255; tp2.c[1] = t >> 8 & 255; tp2.c[2] = t & 255; tp2.c[3] = t >> 24; }
                    int o[4] = { r, g, b, al };
                    combine(tp2, pp, 0, o); combine(tp2, pp, 1, o);
                    r = o[0]; g = o[1]; b = o[2]; al = o[3];
                } else if (TEX) {
                    uint32_t t = sample(tex, lvl, U, V, lin);
                    int tr = t >> 16 & 255, tg = t >> 8 & 255, tb = t & 255, ta = t >> 24;
                    switch (S.texEnv) {
                    case GL_REPLACE: r = tr; g = tg; b = tb; al = ta; break;
                    case GL_DECAL:
                        if (tex->alpha) { r = (r * (255 - ta) + tr * ta) / 255; g = (g * (255 - ta) + tg * ta) / 255; b = (b * (255 - ta) + tb * ta) / 255; }
                        else { r = tr; g = tg; b = tb; }
                        break;
                    case GL_ADD: r = r + tr > 255 ? 255 : r + tr; g = g + tg > 255 ? 255 : g + tg; b = b + tb > 255 ? 255 : b + tb; al = al * ta / 255; break;
                    default: r = (r * tr + 127) / 255; g = (g * tg + 127) / 255; b = (b * tb + 127) / 255; al = (al * ta + 127) / 255; break;
                    }
                }
                if (ATEST && !alphaPassR(al, c.aref)) continue;
                if (FOG && fogOn) {
                    int f = clampi(F >> 8, 0, 256);
                    r += (c.fogR - r) * f >> 8; g += (c.fogG - g) * f >> 8; b += (c.fogB - b) * f >> 8;
                }
                uint32_t d = row[px];
                if (BLEND) {
                    int dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
                    if (c.blSA) {
                        int a = al + (al >> 7), ia = 256 - a;
                        r = (r * a + dr * ia) >> 8; g = (g * a + dg * ia) >> 8; b = (b * a + db * ia) >> 8;
                    } else {
                        int nr = (r * factor(S.srcF, r, dr, al) + dr * factor(S.dstF, r, dr, al)) >> 8;
                        int ng = (g * factor(S.srcF, g, dg, al) + dg * factor(S.dstF, g, dg, al)) >> 8;
                        int nb = (b * factor(S.srcF, b, db, al) + db * factor(S.dstF, b, db, al)) >> 8;
                        r = nr > 255 ? 255 : nr; g = ng > 255 ? 255 : ng; b = nb > 255 ? 255 : nb;
                    }
                }
                if (c.cmAll) row[px] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
                else {
                    uint32_t o = d;
                    if (S.cmR) o = (o & 0xFF00FFFF) | (uint32_t)r << 16;
                    if (S.cmG) o = (o & 0xFFFF00FF) | (uint32_t)g << 8;
                    if (S.cmB) o = (o & 0xFFFFFF00) | (uint32_t)b;
                    row[px] = o;
                }
                pixCount++;
                if (depthWrite) zrow[px] = z;
            }
            A[1] = B[1];
            if (TEX) { A[2] = B[2]; A[3] = B[3]; }
            if (!c.flat) { A[4] = B[4]; A[5] = B[5]; A[6] = B[6]; A[7] = B[7]; }
            if (FOG) A[8] = B[8];
            w0 = w1; u0 = u1; v0t = v1t; R0 = R1; G0 = G1; B0 = B1; AL0 = AL1; F0 = F1;
            x += n;
        }
    }
    ST.pix += pixCount;
}

struct Prof { unsigned long long cycles; unsigned pixels, tris; };
Prof g_prof[32];

typedef void (*RowsFn)(const RC &);
template <int I> struct Pick {
    static RowsFn get(int i) { return i == I ? &rows<(I & 1) != 0, (I & 2) != 0, (I & 4) != 0, (I & 8) != 0, (I & 16) != 0> : Pick<I - 1>::get(i); }
};
template <> struct Pick<-1> { static RowsFn get(int) { return nullptr; } };

void rasterTri(const SV *a, const SV *b, const SV *c)
{
    ST.raster++;
    const SV *v0 = a, *v1 = b, *v2 = c;
    if (v1->y < v0->y) { const SV *t = v0; v0 = v1; v1 = t; }
    if (v2->y < v0->y) { const SV *t = v0; v0 = v2; v2 = t; }
    if (v2->y < v1->y) { const SV *t = v1; v1 = v2; v2 = t; }
    float denom = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (denom == 0 || v2->y - v0->y < 1e-6f) return;

    RC rc;
    rc.v0 = v0; rc.v1 = v1; rc.v2 = v2;
    float id = 1.0f / denom;
    for (int i = 0; i < 9; i++) {
        float da = v1->at[i] - v0->at[i], db = v2->at[i] - v0->at[i];
        rc.gx[i] = (da * (v2->y - v0->y) - db * (v1->y - v0->y)) * id;
        rc.gy[i] = (db * (v1->x - v0->x) - da * (v2->x - v0->x)) * id;
    }

    int cx0 = 0, cy0 = 0, cx1 = S.fw, cy1 = S.fh;
    {
        int vx0 = S.vx, vy0 = S.fh - (S.vy + S.vh), vx1 = S.vx + S.vw, vy1 = S.fh - S.vy;
        cx0 = vx0 > cx0 ? vx0 : cx0; cy0 = vy0 > cy0 ? vy0 : cy0; cx1 = vx1 < cx1 ? vx1 : cx1; cy1 = vy1 < cy1 ? vy1 : cy1;
        if (S.scissor) {
            int sx0 = S.sx, sy0 = S.fh - (S.sy + S.sh), sx1 = S.sx + S.sw, sy1 = S.fh - S.sy;
            cx0 = sx0 > cx0 ? sx0 : cx0; cy0 = sy0 > cy0 ? sy0 : cy0; cx1 = sx1 < cx1 ? sx1 : cx1; cy1 = sy1 < cy1 ? sy1 : cy1;
        }
    }
    int ys = ftoi(v0->y), ye = ftoi(v2->y);
    if (ys < cy0) ys = cy0;
    if (ye > cy1) ye = cy1;
    if (ys >= ye) return;
    rc.ys = ys; rc.ye = ye; rc.cx0 = cx0; rc.cx1 = cx1;
    rc.fogR = ftoi(S.fogCol[0] * 255.0f); rc.fogG = ftoi(S.fogCol[1] * 255.0f); rc.fogB = ftoi(S.fogCol[2] * 255.0f);

    Tex *tex = S.texOn ? (S.boundTex < MAXTEX ? S.tex[S.boundTex] : nullptr) : nullptr;
    if (tex && (tex->levels == 0 || !tex->px[0])) tex = nullptr;
    rc.tex = tex;
    rc.linMag = tex && tex->magf == GL_LINEAR;
    rc.mip = tex && tex->levels > 1 && wantsMips(tex);

    { float dy02 = v2->y - v0->y, dy01 = v1->y - v0->y, dy12 = v2->y - v1->y;
      rc.s02 = (v2->x - v0->x) / dy02;
      rc.s01 = dy01 > 1e-6f ? (v1->x - v0->x) / dy01 : 0.0f;
      rc.s12 = dy12 > 1e-6f ? (v2->x - v1->x) / dy12 : 0.0f; }
    rc.flat = g_flat;
    if (g_flat) {
        rc.fr = clampi(ftoi(g_fc[0] * 255.0f), 0, 255); rc.fg = clampi(ftoi(g_fc[1] * 255.0f), 0, 255);
        rc.fb = clampi(ftoi(g_fc[2] * 255.0f), 0, 255); rc.fa = clampi(ftoi(g_fc[3] * 255.0f), 0, 255);
    }
    int esc = 1, eas = 1;
    bool mod = modulateEnv(esc, eas);
    rc.esc = esc; rc.eas = eas;
    rc.aref = (int)(S.alphaRef * 255.0f + 0.5f);
    rc.dLess = S.depthFunc == GL_LESS;
    rc.cmAll = S.cmR && S.cmG && S.cmB;
    rc.blSA = S.srcF == GL_SRC_ALPHA && S.dstF == GL_ONE_MINUS_SRC_ALPHA;

    static RowsFn table[32];
    if (!table[0]) for (int i = 0; i < 32; i++) table[i] = Pick<31>::get(i);
    int idx = (tex ? 1 : 0) | (S.fog ? 2 : 0) | (S.blend ? 4 : 0) | (S.alphaTest ? 8 : 0) | (mod ? 16 : 0);
    unsigned lo0, hi0, lo1, hi1;
    unsigned pix0 = ST.pix;
    __asm__ __volatile__("rdtsc" : "=a"(lo0), "=d"(hi0));
    table[idx](rc);
    __asm__ __volatile__("rdtsc" : "=a"(lo1), "=d"(hi1));
    g_prof[idx].cycles += ((unsigned long long)hi1 << 32 | lo1) - ((unsigned long long)hi0 << 32 | lo0);
    g_prof[idx].pixels += ST.pix - pix0;
    g_prof[idx].tris++;
}

// ---- primitive assembly ----------------------------------------------------------------------------------------------

void toClip(const Vert &v, CV &o)
{
    const float *m = MV(), *p = PJ();
    float ex = m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w;
    float ey = m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w;
    float ez = m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w;
    float ew = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w;
    o.x = p[0] * ex + p[4] * ey + p[8] * ez + p[12] * ew;
    o.y = p[1] * ex + p[5] * ey + p[9] * ez + p[13] * ew;
    o.z = p[2] * ex + p[6] * ey + p[10] * ez + p[14] * ew;
    o.w = p[3] * ex + p[7] * ey + p[11] * ez + p[15] * ew;
    o.r = v.r; o.g = v.g; o.b = v.b; o.a = v.a;
    o.ez = ez;
    const float *t = TM();
    if (isIdent(t)) { o.s = v.s; o.t = v.t; }
    else { o.s = t[0] * v.s + t[4] * v.t + t[12]; o.t = t[1] * v.s + t[5] * v.t + t[13]; }
}

void toScreen(const CV &c, SV &o)
{
    float iw = 1.0f / c.w;
    float nx = c.x * iw, ny = c.y * iw, nz = c.z * iw;
    o.x = S.vx + (nx * 0.5f + 0.5f) * S.vw;
    o.y = S.fh - (S.vy + (ny * 0.5f + 0.5f) * S.vh);
    float z = nz * 0.5f + 0.5f;
    if (S.polyOff) z += S.offUnits * 4e-6f;
    o.at[0] = z; o.at[1] = iw;
    o.at[2] = c.s * iw; o.at[3] = c.t * iw;
    o.at[4] = c.r * 255.0f * iw; o.at[5] = c.g * 255.0f * iw; o.at[6] = c.b * 255.0f * iw; o.at[7] = c.a * 255.0f * iw;
    o.at[8] = -c.ez * iw;
}

CV lerpCV(const CV &a, const CV &b, float t)
{
    CV o;
    const float *pa = &a.x, *pb = &b.x; float *po = &o.x;
    for (int i = 0; i < 11; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    return o;
}

void drawTri(const CV &a, const CV &b, const CV &c)
{
    ST.tris++;
    if (a.z > a.w && b.z > b.w && c.z > c.w) return;          // beyond the far plane
    if (a.x > a.w && b.x > b.w && c.x > c.w) return;
    if (a.x < -a.w && b.x < -b.w && c.x < -c.w) return;
    if (a.y > a.w && b.y > b.w && c.y > c.w) return;
    if (a.y < -a.w && b.y < -b.w && c.y < -c.w) return;
    CV in[3] = { a, b, c }, poly[4]; int n = 0;
    for (int i = 0; i < 3; i++) {                              // near plane: z >= -w
        const CV &p = in[i], &q = in[(i + 1) % 3];
        float dp = p.z + p.w, dq = q.z + q.w;
        if (dp >= 0) poly[n++] = p;
        if ((dp >= 0) != (dq >= 0)) poly[n++] = lerpCV(p, q, dp / (dp - dq));
    }
    if (n < 3) { ST.rejected++; return; }
    SV sv[4];
    for (int i = 0; i < n; i++) toScreen(poly[i], sv[i]);
    g_flat = a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a && a.r == c.r && a.g == c.g && a.b == c.b && a.a == c.a;
    g_fc[0] = a.r; g_fc[1] = a.g; g_fc[2] = a.b; g_fc[3] = a.a;
    for (int i = 1; i + 1 < n; i++) {
        float area = (sv[i].x - sv[0].x) * (sv[i + 1].y - sv[0].y) - (sv[i + 1].x - sv[0].x) * (sv[i].y - sv[0].y);   // screen y points down, so GL-counter-clockwise is negative
        if (S.cull) {
            bool front = (area < 0) == (S.frontFace == GL_CCW);
            if (S.cullFace == GL_FRONT_AND_BACK) continue;
            if (S.cullFace == GL_FRONT ? front : !front) { ST.culled++; continue; }
        }
        rasterTri(&sv[0], &sv[i], &sv[i + 1]);
    }
}

void drawLine(const CV &a, const CV &b)
{
    if (a.w <= 0 || b.w <= 0) return;
    SV p, q; toScreen(a, p); toScreen(b, q);
    float dx = q.x - p.x, dy = q.y - p.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    float hw = S.lineW > 1 ? S.lineW * 0.5f : 0.5f, nx = -dy / len * hw, ny = dx / len * hw;
    SV v[4] = { p, p, q, q };
    v[0].x += nx; v[0].y += ny; v[1].x -= nx; v[1].y -= ny; v[2].x += nx; v[2].y += ny; v[3].x -= nx; v[3].y -= ny;
    g_flat = false;
    bool tex = S.texOn; S.texOn = false;
    rasterTri(&v[0], &v[1], &v[2]); rasterTri(&v[1], &v[3], &v[2]);
    S.texOn = tex;
}

void emitVerts(GLenum mode, const Vert *v, int n)
{
    if (n <= 0) return;
    ST.calls++; if (S.texOn) ST.texOn++; if (S.blend) ST.blend++;
    if (g_dbg > 0) {
        g_dbg--;
        const Tex *t = S.boundTex < MAXTEX ? S.tex[S.boundTex] : nullptr;
        fprintf(stderr, "dbg: mode=%d n=%d tex=%d(%dx%d lv%d min=%x) blend=%d src=%x dst=%x depth=%d/%x mask=%d cull=%d/%x fog=%d env=%x v0=(%.1f,%.1f,%.1f) col=(%.2f,%.2f,%.2f,%.2f) st=(%.2f,%.2f)\n",
            mode, n, S.texOn, t ? t->w[0] : 0, t ? t->h[0] : 0, t ? t->levels : 0, t ? t->minf : 0, S.blend, S.srcF, S.dstF, S.depthTest, S.depthFunc, S.depthMask, S.cull, S.cullFace, S.fog, S.texEnv,
            v[0].x, v[0].y, v[0].z, v[0].r, v[0].g, v[0].b, v[0].a, v[0].s, v[0].t);
    }
    CV *c = (CV *)malloc(sizeof(CV) * n);
    for (int i = 0; i < n; i++) toClip(v[i], c[i]);
    switch (mode) {
    case GL_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) drawTri(c[i], c[i + 1], c[i + 2]); break;
    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) { if (i & 1) drawTri(c[i + 1], c[i], c[i + 2]); else drawTri(c[i], c[i + 1], c[i + 2]); }
        break;
    case GL_TRIANGLE_FAN: case GL_POLYGON: for (int i = 1; i + 1 < n; i++) drawTri(c[0], c[i], c[i + 1]); break;
    case GL_QUADS: for (int i = 0; i + 3 < n; i += 4) { drawTri(c[i], c[i + 1], c[i + 2]); drawTri(c[i], c[i + 2], c[i + 3]); } break;
    case GL_QUAD_STRIP: for (int i = 0; i + 3 < n; i += 2) { drawTri(c[i], c[i + 1], c[i + 3]); drawTri(c[i], c[i + 3], c[i + 2]); } break;
    case GL_LINES: for (int i = 0; i + 1 < n; i += 2) drawLine(c[i], c[i + 1]); break;
    case GL_LINE_STRIP: for (int i = 0; i + 1 < n; i++) drawLine(c[i], c[i + 1]); break;
    case GL_LINE_LOOP: for (int i = 0; i < n; i++) drawLine(c[i], c[(i + 1) % n]); break;
    default: break;   // points are not drawn
    }
    free(c);
}

void pushVert(float x, float y, float z, float w)
{
    if (S.primMode < 0) return;
    if (S.ivN == S.ivCap) { S.ivCap = S.ivCap ? S.ivCap * 2 : 1024; S.iv = (Vert *)realloc(S.iv, sizeof(Vert) * S.ivCap); }
    Vert &v = S.iv[S.ivN++];
    v.x = x; v.y = y; v.z = z; v.w = w;
    v.r = S.cur[0]; v.g = S.cur[1]; v.b = S.cur[2]; v.a = S.cur[3]; v.s = S.curT[0]; v.t = S.curT[1];
}

float readComp(const uint8_t *p, GLenum type, int i, bool color)
{
    switch (type) {
    case GL_FLOAT: return ((const float *)p)[i];
    case GL_UNSIGNED_BYTE: return color ? p[i] / 255.0f : (float)p[i];
    case GL_SHORT: return (float)((const int16_t *)p)[i];
    case GL_INT: return (float)((const int32_t *)p)[i];
    case GL_DOUBLE: return (float)((const double *)p)[i];
    default: return 0;
    }
}

int typeSize(GLenum t) { return t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT ? 4 : (t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : (t == GL_DOUBLE ? 8 : 1)); }

void fetchVert(int idx, Vert &v)
{
    int vs = S.vaStride ? S.vaStride : S.vaSize * typeSize(S.vaType);
    const uint8_t *p = S.vaP + (size_t)idx * vs;
    v.x = readComp(p, S.vaType, 0, false); v.y = readComp(p, S.vaType, 1, false);
    v.z = S.vaSize > 2 ? readComp(p, S.vaType, 2, false) : 0; v.w = S.vaSize > 3 ? readComp(p, S.vaType, 3, false) : 1;
    v.r = S.cur[0]; v.g = S.cur[1]; v.b = S.cur[2]; v.a = S.cur[3];
    if (S.caOn && S.caP) {
        int cs = S.caStride ? S.caStride : S.caSize * typeSize(S.caType);
        const uint8_t *c = S.caP + (size_t)idx * cs;
        v.r = readComp(c, S.caType, 0, true); v.g = readComp(c, S.caType, 1, true); v.b = readComp(c, S.caType, 2, true);
        v.a = S.caSize > 3 ? readComp(c, S.caType, 3, true) : 1;
    }
    v.s = S.curT[0]; v.t = S.curT[1];
    if (S.taOn && S.taP) {
        int ts = S.taStride ? S.taStride : S.taSize * typeSize(S.taType);
        const uint8_t *t = S.taP + (size_t)idx * ts;
        v.s = readComp(t, S.taType, 0, false); v.t = readComp(t, S.taType, 1, false);
    }
}

void drawIndexed(GLenum mode, GLsizei count, GLenum type, const void *indices, int first)
{
    if (!S.vaOn || !S.vaP || count <= 0) return;
    Vert *v = (Vert *)malloc(sizeof(Vert) * count);
    for (int i = 0; i < count; i++) {
        int idx = first + i;
        if (indices) idx = type == GL_UNSIGNED_BYTE ? ((const uint8_t *)indices)[i] : (type == GL_UNSIGNED_SHORT ? ((const uint16_t *)indices)[i] : (int)((const uint32_t *)indices)[i]);
        fetchVert(idx, v[i]);
    }
    emitVerts(mode, v, count);
    free(v);
}

}   // namespace

// ---- the GL entry points -----------------------------------------------------------------------------------------------

extern "C" {

void softgl_debug(int calls) { g_dbg = calls; }

// cycles per pixel of the busiest span loops (index bits: 1 texture, 2 fog, 4 blend, 8 alpha test, 16 modulate), then resets
int softgl_prof(char *buf, int n)
{
    int o = 0;
    for (int i = 0; i < 32 && o < n - 80; i++) {
        if (g_prof[i].tris == 0) continue;
        o += snprintf(buf + o, n - o, "[%d] tris=%u px=%u cyc/px=%u cyc/tri=%u; ", i, g_prof[i].tris, g_prof[i].pixels,
                      g_prof[i].pixels ? (unsigned)(g_prof[i].cycles / g_prof[i].pixels) : 0, (unsigned)(g_prof[i].cycles / g_prof[i].tris));
    }
    memset(g_prof, 0, sizeof g_prof);
    return o;
}

int softgl_stats(char *buf, int n)
{
    int r = snprintf(buf, n, "softgl: calls=%u tris=%u rejected=%u culled=%u raster=%u zfail=%u pix=%u tex=%u blend=%u", ST.calls, ST.tris, ST.rejected, ST.culled, ST.raster, ST.zfail, ST.pix, ST.texOn, ST.blend);
    memset(&ST, 0, sizeof(ST));
    return r;
}

void softgl_attach(unsigned int *pixels, int w, int h)
{
    S.fb = pixels; S.fw = w; S.fh = h;
    free(S.zb); S.zb = (int32_t *)malloc((size_t)w * h * sizeof(int32_t));
    if (S.mvT == 0 && S.pjT == 0 && S.vw == 0) { ident(S.mv[0]); ident(S.pj[0]); ident(S.tm[0]); }
    S.vx = S.vy = 0; S.vw = w; S.vh = h; S.sx = S.sy = 0; S.sw = w; S.sh = h;
}

void glClear(GLbitfield mask)
{
    if (!S.fb) return;
    size_t n = (size_t)S.fw * S.fh;
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t c = (uint32_t)f2b(S.clearC[0]) << 16 | (uint32_t)f2b(S.clearC[1]) << 8 | f2b(S.clearC[2]);
        for (size_t i = 0; i < n; i++) S.fb[i] = c;
    }
    if (mask & GL_DEPTH_BUFFER_BIT) { int32_t cz = zFromFloat(S.clearD); for (size_t i = 0; i < n; i++) S.zb[i] = cz; }
}
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { S.clearC[0] = r; S.clearC[1] = g; S.clearC[2] = b; S.clearC[3] = a; }
void glClearDepth(GLclampd d) { S.clearD = (float)d; }
void glClearStencil(GLint) {}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vx = x; S.vy = y; S.vw = w; S.vh = h; }
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sx = x; S.sy = y; S.sw = w; S.sh = h; }

static void setCap(GLenum cap, bool on)
{
    switch (cap) {
    case GL_DEPTH_TEST: S.depthTest = on; break;
    case GL_BLEND: S.blend = on; break;
    case GL_CULL_FACE: S.cull = on; break;
    case GL_FOG: S.fog = on; break;
    case GL_SCISSOR_TEST: S.scissor = on; break;
    case GL_TEXTURE_2D: S.texOn = on; break;
    case GL_ALPHA_TEST: S.alphaTest = on; break;
    case GL_POLYGON_OFFSET_FILL: S.polyOff = on; break;
    default: break;
    }
}
void glEnable(GLenum cap) { setCap(cap, true); }
void glDisable(GLenum cap) { setCap(cap, false); }
GLboolean glIsEnabled(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST: return S.depthTest; case GL_BLEND: return S.blend; case GL_CULL_FACE: return S.cull;
    case GL_FOG: return S.fog; case GL_SCISSOR_TEST: return S.scissor; case GL_TEXTURE_2D: return S.texOn;
    default: return 0;
    }
}
void glBlendFunc(GLenum s, GLenum d) { S.srcF = s; S.dstF = d; }
void glDepthFunc(GLenum f) { S.depthFunc = f; }
void glDepthMask(GLboolean f) { S.depthMask = f != 0; }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean) { S.cmR = r; S.cmG = g; S.cmB = b; }
void glCullFace(GLenum m) { S.cullFace = m; }
void glFrontFace(GLenum m) { S.frontFace = m; }
void glAlphaFunc(GLenum f, GLclampf r) { S.alphaFunc = f; S.alphaRef = r; }
void glPolygonOffset(GLfloat f, GLfloat u) { S.offFactor = f; S.offUnits = u; }
void glPolygonMode(GLenum, GLenum) {}
void glShadeModel(GLenum) {}
void glLineWidth(GLfloat w) { S.lineW = w; }
void glHint(GLenum, GLenum) {}
void glFogi(GLenum p, GLint v) { if (p == GL_FOG_MODE) S.fogMode = v; else if (p == GL_FOG_START) S.fogStart = (float)v; else if (p == GL_FOG_END) S.fogEnd = (float)v; }
void glFogf(GLenum p, GLfloat v)
{
    if (p == GL_FOG_MODE) S.fogMode = (int)v; else if (p == GL_FOG_DENSITY) S.fogDensity = v;
    else if (p == GL_FOG_START) S.fogStart = v; else if (p == GL_FOG_END) S.fogEnd = v;
}
void glFogfv(GLenum p, const GLfloat *v) { if (p == GL_FOG_COLOR) memcpy(S.fogCol, v, 16); else glFogf(p, v[0]); }
void glStencilFunc(GLenum, GLint, GLuint) {}
void glStencilOp(GLenum, GLenum, GLenum) {}
GLenum glGetError(void) { GLenum e = S.err; S.err = 0; return e; }

const GLubyte *glGetString(GLenum n)
{
    switch (n) {
    case GL_VENDOR: return (const GLubyte *)"RadiumOS";
    case GL_RENDERER: return (const GLubyte *)"softgl (x87 software rasteriser)";
    case GL_VERSION: return (const GLubyte *)"1.1 softgl";
    case GL_EXTENSIONS: return (const GLubyte *)"GL_ARB_texture_env_combine";
    default: return (const GLubyte *)"";
    }
}
void glGetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case GL_VIEWPORT: v[0] = S.vx; v[1] = S.vy; v[2] = S.vw; v[3] = S.vh; break;
    case GL_MAX_TEXTURE_SIZE: *v = 2048; break;
    case GL_MAX_TEXTURE_UNITS: *v = 1; break;
    case GL_DEPTH_BITS: *v = 32; break;
    case GL_STENCIL_BITS: *v = 0; break;
    case GL_RED_BITS: *v = 8; break;
    default: *v = 0; break;
    }
}
void glGetFloatv(GLenum p, GLfloat *v)
{
    if (p == GL_MODELVIEW_MATRIX) memcpy(v, MV(), 64);
    else if (p == GL_PROJECTION_MATRIX) memcpy(v, PJ(), 64);
    else v[0] = 0;
}
void glFlush(void) {}
void glFinish(void) {}
void glPixelStorei(GLenum p, GLint v)
{
    if (p == GL_UNPACK_ALIGNMENT) S.unpackAlign = v; else if (p == GL_PACK_ALIGNMENT) S.packAlign = v; else if (p == GL_UNPACK_ROW_LENGTH) S.unpackRow = v;
}
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels)
{
    if (format == GL_DEPTH_COMPONENT) {          // the depth buffer holds 0..1 already
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int sx = x + i, sy = S.fh - 1 - (y + j);
            float d = (sx >= 0 && sx < S.fw && sy >= 0 && sy < S.fh) ? S.zb[sy * S.fw + sx] * (1.0f / ZSCALE) : 1.0f;
            if (type == GL_FLOAT) ((float *)pixels)[j * w + i] = d; else ((uint8_t *)pixels)[j * w + i] = f2b(d);
        }
        return;
    }
    int comps = format == GL_RGBA || format == GL_BGRA ? 4 : (format == GL_RGB || format == GL_BGR ? 3 : 1);
    size_t rowBytes = ((size_t)w * comps + S.packAlign - 1) / S.packAlign * S.packAlign;
    for (int j = 0; j < h; j++) {
        uint8_t *o = (uint8_t *)pixels + j * rowBytes;
        int sy = S.fh - 1 - (y + j);
        for (int i = 0; i < w; i++) {
            uint32_t p = (x + i >= 0 && x + i < S.fw && sy >= 0 && sy < S.fh) ? S.fb[sy * S.fw + x + i] : 0;
            uint8_t r = p >> 16, g = p >> 8, b = p;
            if (comps == 1) { o[i] = format == GL_ALPHA ? 255 : (format == GL_RED ? r : (uint8_t)((r * 77 + g * 150 + b * 29) >> 8)); continue; }
            if (format == GL_BGR || format == GL_BGRA) { o[i * comps] = b; o[i * comps + 1] = g; o[i * comps + 2] = r; }
            else { o[i * comps] = r; o[i * comps + 1] = g; o[i * comps + 2] = b; }
            if (comps == 4) o[i * comps + 3] = 255;
        }
    }
}

void glMatrixMode(GLenum m) { S.matMode = m; }
void glLoadIdentity(void) { ident(curMat()); }
void glLoadMatrixf(const GLfloat *m) { memcpy(curMat(), m, 64); }
void glMultMatrixf(const GLfloat *m) { float *c = curMat(); mulm(c, m, c); }
void glPushMatrix(void)
{
    if (S.matMode == GL_PROJECTION) { if (S.pjT < STACK - 1) { memcpy(S.pj[S.pjT + 1], PJ(), 64); S.pjT++; } }
    else if (S.matMode == GL_TEXTURE) { if (S.tmT < STACK - 1) { memcpy(S.tm[S.tmT + 1], TM(), 64); S.tmT++; } }
    else if (S.mvT < STACK - 1) { memcpy(S.mv[S.mvT + 1], MV(), 64); S.mvT++; }
}
void glPopMatrix(void)
{
    if (S.matMode == GL_PROJECTION) { if (S.pjT > 0) S.pjT--; }
    else if (S.matMode == GL_TEXTURE) { if (S.tmT > 0) S.tmT--; }
    else if (S.mvT > 0) S.mvT--;
}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16]; ident(m); m[12] = x; m[13] = y; m[14] = z; glMultMatrixf(m);
}
void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    float m[16]; ident(m); m[0] = x; m[5] = y; m[10] = z; glMultMatrixf(m);
}
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
    float len = sqrtf(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len; y /= len; z /= len;
    float rad = a * 3.14159265358979f / 180.0f, c = cosf(rad), s = sinf(rad), t = 1 - c, m[16];
    ident(m);
    m[0] = t * x * x + c;     m[4] = t * x * y - s * z; m[8]  = t * x * z + s * y;
    m[1] = t * x * y + s * z; m[5] = t * y * y + c;     m[9]  = t * y * z - s * x;
    m[2] = t * x * z - s * y; m[6] = t * y * z + s * x; m[10] = t * z * z + c;
    glMultMatrixf(m);
}
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16]; ident(m);
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    glMultMatrixf(m);
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
    float m[16]; memset(m, 0, 64);
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    glMultMatrixf(m);
}

void glBegin(GLenum mode) { S.primMode = (int)mode; S.ivN = 0; }
void glEnd(void)
{
    if (S.primMode >= 0) emitVerts((GLenum)S.primMode, S.iv, S.ivN);
    S.primMode = -1; S.ivN = 0;
}
void glVertex2f(GLfloat x, GLfloat y) { pushVert(x, y, 0, 1); }
void glVertex2d(GLdouble x, GLdouble y) { pushVert((float)x, (float)y, 0, 1); }
void glVertex2i(GLint x, GLint y) { pushVert((float)x, (float)y, 0, 1); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { pushVert(x, y, z, 1); }
void glVertex3fv(const GLfloat *v) { pushVert(v[0], v[1], v[2], 1); }
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { pushVert(x, y, z, w); }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { S.cur[0] = r; S.cur[1] = g; S.cur[2] = b; S.cur[3] = 1; }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { glColor3f((float)r, (float)g, (float)b); }
void glColor3fv(const GLfloat *v) { glColor3f(v[0], v[1], v[2]); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { glColor3f(r / 255.0f, g / 255.0f, b / 255.0f); }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.cur[0] = r; S.cur[1] = g; S.cur[2] = b; S.cur[3] = a; }
void glColor4fv(const GLfloat *v) { glColor4f(v[0], v[1], v[2], v[3]); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glColor4ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], v[3]); }
void glTexCoord2f(GLfloat s, GLfloat t) { S.curT[0] = s; S.curT[1] = t; }
void glTexCoord2i(GLint s, GLint t) { S.curT[0] = (float)s; S.curT[1] = (float)t; }
void glTexCoord2fv(const GLfloat *v) { S.curT[0] = v[0]; S.curT[1] = v[1]; }
void glNormal3f(GLfloat, GLfloat, GLfloat) {}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { S.vaSize = size; S.vaType = type; S.vaStride = stride; S.vaP = (const uint8_t *)p; }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { S.caSize = size; S.caType = type; S.caStride = stride; S.caP = (const uint8_t *)p; }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { S.taSize = size; S.taType = type; S.taStride = stride; S.taP = (const uint8_t *)p; }
void glNormalPointer(GLenum, GLsizei, const GLvoid *) {}
static void setArray(GLenum a, bool on)
{
    if (a == GL_VERTEX_ARRAY) S.vaOn = on; else if (a == GL_COLOR_ARRAY) S.caOn = on; else if (a == GL_TEXTURE_COORD_ARRAY) S.taOn = on;
}
void glEnableClientState(GLenum a) { setArray(a, true); }
void glDisableClientState(GLenum a) { setArray(a, false); }
void glDrawArrays(GLenum mode, GLint first, GLsizei count) { drawIndexed(mode, count, 0, nullptr, first); }
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) { drawIndexed(mode, count, type, indices, 0); }
void glDrawRangeElements(GLenum mode, GLuint, GLuint, GLsizei count, GLenum type, const GLvoid *indices) { drawIndexed(mode, count, type, indices, 0); }

void glGenTextures(GLsizei n, GLuint *t) { for (int i = 0; i < n; i++) t[i] = S.nextTex < MAXTEX ? S.nextTex++ : 0; }
void glDeleteTextures(GLsizei n, const GLuint *t)
{
    for (int i = 0; i < n; i++) {
        if (t[i] == 0 || t[i] >= MAXTEX || !S.tex[t[i]]) continue;
        freeLevels(S.tex[t[i]], 0); delete S.tex[t[i]]; S.tex[t[i]] = nullptr;
        if (S.boundTex == t[i]) S.boundTex = 0;
    }
}
void glBindTexture(GLenum, GLuint t) { S.boundTex = t < MAXTEX ? t : 0; }
void glTexParameteri(GLenum, GLenum p, GLint v)
{
    Tex *t = curTex(); if (!t) return;
    if (p == GL_TEXTURE_MIN_FILTER) { bool was = wantsMips(t); t->minf = v; if (was != wantsMips(t)) refreshMips(t); }
    else if (p == GL_TEXTURE_MAG_FILTER) t->magf = v;
    else if (p == GL_TEXTURE_WRAP_S) t->wrapS = v == GL_CLAMP_TO_EDGE ? GL_CLAMP : v;
    else if (p == GL_TEXTURE_WRAP_T) t->wrapT = v == GL_CLAMP_TO_EDGE ? GL_CLAMP : v;
}
void glTexParameterf(GLenum t, GLenum p, GLfloat v) { glTexParameteri(t, p, (GLint)v); }
void glTexEnvi(GLenum, GLenum p, GLint v)
{
    if (p == GL_TEXTURE_ENV_MODE) S.texEnv = v;
    else if (p == GL_COMBINE_RGB_ARB) S.cmbFn[0] = v;
    else if (p == GL_COMBINE_ALPHA_ARB) S.cmbFn[1] = v;
    else if (p >= GL_SOURCE0_RGB_ARB && p <= GL_SOURCE0_RGB_ARB + 2) S.cmbSrc[0][p - GL_SOURCE0_RGB_ARB] = v;
    else if (p >= GL_SOURCE0_ALPHA_ARB && p <= GL_SOURCE0_ALPHA_ARB + 2) S.cmbSrc[1][p - GL_SOURCE0_ALPHA_ARB] = v;
    else if (p >= GL_OPERAND0_RGB_ARB && p <= GL_OPERAND0_RGB_ARB + 2) S.cmbOp[0][p - GL_OPERAND0_RGB_ARB] = v;
    else if (p >= GL_OPERAND0_ALPHA_ARB && p <= GL_OPERAND0_ALPHA_ARB + 2) S.cmbOp[1][p - GL_OPERAND0_ALPHA_ARB] = v;
    else if (p == GL_RGB_SCALE_ARB) S.cmbScale[0] = v;
    else if (p == GL_ALPHA_SCALE) S.cmbScale[1] = v;
}
void glTexEnvf(GLenum t, GLenum p, GLfloat v) { glTexEnvi(t, p, (GLint)v); }
void glTexEnvfv(GLenum, GLenum p, const GLfloat *v) { if (p == GL_TEXTURE_ENV_COLOR) for (int i = 0; i < 4; i++) S.envColor[i] = f2b(v[i]); }

void glTexImage2D(GLenum, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const GLvoid *pixels)
{
    Tex *t = curTex();
    if (!t || level < 0 || level >= 12 || w <= 0 || h <= 0) return;
    free(t->px[level]);
    t->px[level] = (uint32_t *)malloc((size_t)w * h * 4);
    t->w[level] = w; t->h[level] = h;
    bool a = false;
    if (pixels) convert(pixels, w, h, format, type, t->px[level], &a);
    else memset(t->px[level], 0, (size_t)w * h * 4);
    if (level == 0) { t->alpha = a; t->levels = 1; freeLevels(t, 1); if (wantsMips(t)) buildMips(t); }
    else if (level >= t->levels) t->levels = level + 1;   // the application supplies its own mip chain
}
void glTexSubImage2D(GLenum, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid *pixels)
{
    Tex *t = curTex();
    if (!t || level < 0 || level >= 12 || !t->px[level] || !pixels) return;
    uint32_t *tmp = (uint32_t *)malloc((size_t)w * h * 4); bool a;
    convert(pixels, w, h, format, type, tmp, &a);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        if (xo + x < t->w[level] && yo + y < t->h[level]) t->px[level][(yo + y) * t->w[level] + xo + x] = tmp[y * w + x];
    free(tmp);
    if (level == 0) refreshMips(t);
}
void glCopyTexSubImage2D(GLenum, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
    Tex *t = curTex();
    if (!t || level != 0 || !t->px[0] || !S.fb) return;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int sy = S.fh - 1 - (y + j), sx = x + i, dx = xo + i, dy = yo + j;
        if (sx < 0 || sx >= S.fw || sy < 0 || sy >= S.fh || dx >= t->w[0] || dy >= t->h[0]) continue;
        t->px[0][dy * t->w[0] + dx] = 0xFF000000u | (S.fb[sy * S.fw + sx] & 0xFFFFFF);
    }
    refreshMips(t);
}
void glGetTexImage(GLenum, GLint level, GLenum format, GLenum, GLvoid *pixels)
{
    Tex *t = curTex();
    if (!t || level < 0 || level >= 12 || !t->px[level]) return;
    int comps = format == GL_RGBA || format == GL_BGRA ? 4 : 3;
    for (int i = 0; i < t->w[level] * t->h[level]; i++) {
        uint32_t p = t->px[level][i]; uint8_t *o = (uint8_t *)pixels + i * comps;
        uint8_t r = p >> 16, g = p >> 8, b = p;
        if (format == GL_BGR || format == GL_BGRA) { o[0] = b; o[1] = g; o[2] = r; } else { o[0] = r; o[1] = g; o[2] = b; }
        if (comps == 4) o[3] = p >> 24;
    }
}

// display lists are not recorded (AssaultCube only uses them for model speed-ups that can be switched off)
GLuint glGenLists(GLsizei range) { GLuint l = S.nextList; S.nextList += range; return l; }
void glNewList(GLuint, GLenum) {}
void glEndList(void) {}
void glCallList(GLuint) {}
void glDeleteLists(GLuint, GLsizei) {}

}   // extern "C"
