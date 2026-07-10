// purzfx.hpp — shared helpers for the purzOS OFX plugin collection.
//
// Every plugin is still its own self-contained module (one .cpp -> one .ofx),
// but the maths that must stay *identical* across effects — deterministic
// hashing, YIQ/HSV colour, value noise, clamped/bilinear sampling — lives here
// so the analog suite speaks one language and 48 effects don't each re-derive
// the same primitives (and drift apart).
//
// Determinism contract: nothing here calls rand()/time(). Every "random" value
// is a pure hash of integer coordinates + a seed, so a render is byte-for-byte
// identical on any machine and safe across a render farm.

#ifndef PURZFX_HPP
#define PURZFX_HPP

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace purz {

// --- scalar maths -----------------------------------------------------------
template <class T> static inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float fracf(float x) { return x - std::floor(x); }
static inline float smoothstep(float e0, float e1, float x) {
  if (e0 == e1) return x < e0 ? 0.f : 1.f;
  float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3.f - 2.f * t);
}
static inline float wrapf(float x, float n) { float r = std::fmod(x, n); return r < 0 ? r + n : r; }

// quantize a normalised 0..1 value back to the pixel type (maxv==1 -> float).
template <class PIX, int maxv> static inline PIX q(float v) {
  v = clamp01(v);
  return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
}

// --- deterministic hashing --------------------------------------------------
static inline uint32_t hashu(uint32_t x) {          // integer avalanche (PCG-ish)
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
static inline float uf(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); } // -> [0,1)
static inline float hash1(int a) { return uf(hashu((uint32_t)a * 0x9e3779b1U + 0x85ebca6bU)); }
static inline float hash2(int a, int b) {
  return uf(hashu((uint32_t)a * 0x9e3779b1U ^ hashu((uint32_t)b * 0x85ebca6bU + 0x1u)));
}
static inline float hash3(int a, int b, int c) {
  uint32_t h = hashu((uint32_t)a * 0x9e3779b1U);
  h = hashu(h ^ (uint32_t)b * 0x85ebca6bU);
  h = hashu(h ^ (uint32_t)c * 0xc2b2ae35U);
  return uf(h);
}
// signed variants in [-1,1]
static inline float sh2(int a, int b) { return hash2(a, b) * 2.f - 1.f; }
static inline float sh3(int a, int b, int c) { return hash3(a, b, c) * 2.f - 1.f; }

// value noise on a lattice (smooth, deterministic). seed decorrelates layers.
static inline float vnoise(float x, float y, int seed) {
  int xi = (int)std::floor(x), yi = (int)std::floor(y);
  float xf = x - xi, yf = y - yi;
  float u = xf * xf * (3.f - 2.f * xf), v = yf * yf * (3.f - 2.f * yf);
  float a = hash3(xi, yi, seed), b = hash3(xi + 1, yi, seed);
  float c = hash3(xi, yi + 1, seed), d = hash3(xi + 1, yi + 1, seed);
  return lerp(lerp(a, b, u), lerp(c, d, u), v);
}
// 1-D value noise (great for per-line/per-frame wobble)
static inline float vnoise1(float x, int seed) {
  int xi = (int)std::floor(x); float xf = x - xi;
  float u = xf * xf * (3.f - 2.f * xf);
  return lerp(hash2(xi, seed), hash2(xi + 1, seed), u);
}

// --- colour -----------------------------------------------------------------
static inline float luma(float r, float g, float b) { return 0.299f * r + 0.587f * g + 0.114f * b; }

static inline void rgb2yiq(float r, float g, float b, float &Y, float &I, float &Q) {
  Y = 0.299f * r + 0.587f * g + 0.114f * b;
  I = 0.596f * r - 0.274f * g - 0.322f * b;
  Q = 0.211f * r - 0.523f * g + 0.312f * b;
}
static inline void yiq2rgb(float Y, float I, float Q, float &r, float &g, float &b) {
  r = Y + 0.956f * I + 0.621f * Q;
  g = Y - 0.272f * I - 0.647f * Q;
  b = Y - 1.106f * I + 1.703f * Q;
}
static inline void rgb2hsv(float r, float g, float b, float &h, float &s, float &v) {
  float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
  v = mx; float d = mx - mn; s = mx <= 0.f ? 0.f : d / mx;
  if (d <= 0.f) { h = 0.f; return; }
  if (mx == r)      h = (g - b) / d + (g < b ? 6.f : 0.f);
  else if (mx == g) h = (b - r) / d + 2.f;
  else              h = (r - g) / d + 4.f;
  h /= 6.f;
}
static inline void hsv2rgb(float h, float s, float v, float &r, float &g, float &b) {
  h = fracf(h) * 6.f; int i = (int)std::floor(h); float f = h - i;
  float p = v * (1.f - s), qv = v * (1.f - s * f), t = v * (1.f - s * (1.f - f));
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break; case 1: r = qv; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break; case 3: r = p; g = qv; b = v; break;
    case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = qv; break;
  }
}

// --- source sampler ---------------------------------------------------------
// Wraps an OFX source image + its bounds and reads normalised RGBA in float,
// regardless of the underlying bit depth. Coordinates are ABSOLUTE OFX pixel
// coords (bottom-up). Out-of-range reads clamp to the edge (wrap variants wrap).
template <class PIX, int nComps, int maxv>
struct Src {
  OFX::Image *img = nullptr;
  OfxRectI b{};
  int W = 0, H = 0;
  void init(OFX::Image *i) { img = i; b = i->getBounds(); W = b.x2 - b.x1; H = b.y2 - b.y1; }

  inline void at(int x, int y, float o[4]) const {
    x = clampv(x, b.x1, b.x2 - 1); y = clampv(y, b.y1, b.y2 - 1);
    const PIX *p = (const PIX *)img->getPixelAddress(x, y);
    if (!p) { o[0] = o[1] = o[2] = 0.f; o[3] = 1.f; return; }
    o[0] = p[0] / (float)maxv; o[1] = p[1] / (float)maxv; o[2] = p[2] / (float)maxv;
    o[3] = nComps == 4 ? p[3] / (float)maxv : 1.f;
  }
  inline void atWrap(int x, int y, float o[4]) const {
    int lx = (int)wrapf((float)(x - b.x1), (float)W);
    int ly = (int)wrapf((float)(y - b.y1), (float)H);
    at(b.x1 + lx, b.y1 + ly, o);
  }
  // bilinear read at fractional absolute coords (clamped edges).
  inline void bilin(float x, float y, float o[4]) const {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    float a[4], c[4], d[4], e[4];
    at(x0, y0, a); at(x0 + 1, y0, c); at(x0, y0 + 1, d); at(x0 + 1, y0 + 1, e);
    for (int k = 0; k < 4; k++)
      o[k] = lerp(lerp(a[k], c[k], fx), lerp(d[k], e[k], fx), fy);
  }
};

// --- OFX describe() boilerplate --------------------------------------------
static inline void describeStd(OFX::ImageEffectDescriptor &desc, const char *label, bool tiles) {
  desc.setLabels(label, label, label);
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(OFX::eContextFilter);
  desc.addSupportedContext(OFX::eContextGeneral);
  desc.addSupportedBitDepth(OFX::eBitDepthUByte);
  desc.addSupportedBitDepth(OFX::eBitDepthUShort);
  desc.addSupportedBitDepth(OFX::eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(tiles);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}
static inline void defineStdClips(OFX::ImageEffectDescriptor &desc, bool tiles) {
  OFX::ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(OFX::ePixelComponentRGBA);
  src->addSupportedComponent(OFX::ePixelComponentRGB);
  src->setSupportsTiles(tiles);
  OFX::ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(OFX::ePixelComponentRGBA);
  dst->addSupportedComponent(OFX::ePixelComponentRGB);
  dst->setSupportsTiles(tiles);
}

} // namespace purz

#endif // PURZFX_HPP
