// Tape Wow — the mechanics of a worn VHS transport. All row displacement,
// driven by sums of slow sines (a capstan is periodic, not random):
//   • WOW      — the whole picture breathes left/right over seconds,
//   • FLUTTER  — faster per-scanline ripple down the frame,
//   • BOUNCE   — small vertical frame bob (loose tape guide),
//   • HEAD SWITCH — the bottom few lines skew hard right with a touch of
//     per-line tear: the head-switching band every VCR hides in overscan.
//
// Whole-pixel row moves (lossless); the only hash is the per-line tear inside
// the head-switch band, keyed by (seed, frame, row) — deterministic on any
// machine. Runs on the host's render thread. OFX rows are bottom-up; the
// head-switch band is anchored to the BOTTOM of the picture, in top-down
// space, where it belongs.

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

namespace {
template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
static inline uint32_t hash2(uint32_t a, uint32_t b) { return hash32(a * 0x9E3779B9u ^ hash32(b)); }
static inline float hashf(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); } // [0,1)
} // namespace

// ---------------------------------------------------------------------------
class TapeWowProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
public:
  explicit TapeWowProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  inline void *srcAt(int lx, int ly) const {
    lx = clampv(lx, 0, _W - 1); ly = clampv(ly, 0, _H - 1);
    return _src->getPixelAddress(_bounds.x1 + lx, _bounds.y2 - 1 - ly);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float wow, float flutter, float bounce,
               int hsH, float hsSkew, float t, uint32_t frame, uint32_t seedh) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;

    // frame-wide motions, once
    const float wowDx = std::sin(t * 0.71f) * wow
                      + std::sin(t * 0.233f + 1.7f) * wow * 0.5f;
    const int vy = (int)std::lround(std::sin(t * 1.31f) * bounce);
    const uint32_t fstate = hash2(seedh, frame);

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y; // top-down row

      float dx = wowDx
               + std::sin(ly * 0.013f + t * 2.9f) * flutter
               + std::sin(ly * 0.037f + t * 5.3f) * flutter * 0.4f;

      // head-switch band at the bottom of the picture
      if (hsH > 0 && ly >= _H - hsH) {
        const float k = (float)(ly - (_H - hsH)) / (float)hsH; // 0 at band top -> 1 at bottom
        dx += hsSkew * k * k
            + (hashf(hash2(fstate, (uint32_t)ly)) * 2.0f - 1.0f) * hsSkew * 0.3f * k;
      }
      const int idx = (int)std::lround(dx);
      const int sy = ly - vy;

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        PIX *p = (PIX *)srcAt(wrapi(lx - idx, _W), sy);
        if (!p) { for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
        for (int c = 0; c < nComps; c++) dst[c] = p[c];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class TapeWowPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_wow = nullptr, *_flutter = nullptr, *_bounce = nullptr,
                   *_hsSkew = nullptr, *_speed = nullptr;
  OFX::IntParam *_hsH = nullptr, *_seed = nullptr;
public:
  explicit TapeWowPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _wow = fetchDoubleParam("wow");
    _flutter = fetchDoubleParam("flutter");
    _bounce = fetchDoubleParam("bounce");
    _hsH = fetchIntParam("hsH");
    _hsSkew = fetchDoubleParam("hsSkew");
    _speed = fetchDoubleParam("speed");
    _seed = fetchIntParam("seed");
  }

  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);

    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4
                 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double wow, flutter, bounce, hsSkew, speed; int hsH, seed;
    _wow->getValueAtTime(args.time, wow);
    _flutter->getValueAtTime(args.time, flutter);
    _bounce->getValueAtTime(args.time, bounce);
    _hsH->getValueAtTime(args.time, hsH);
    _hsSkew->getValueAtTime(args.time, hsSkew);
    _speed->getValueAtTime(args.time, speed);
    _seed->getValueAtTime(args.time, seed);

    // pixel-unit params scale for proxy/half-res renders
    const double rs = args.renderScale.x;
    wow *= rs; flutter *= rs; bounce *= rs; hsSkew *= rs;
    hsH = (int)std::lround(hsH * rs);

    const uint32_t frame = (uint32_t)std::lround(std::max(0.0, args.time));
    const float t = (float)(args.time * speed);
    const uint32_t seedh = hash32((uint32_t)seed * 0x9E3779B9u + 6u);

    TapeWowProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)wow,      \
                     (float)flutter, (float)bounce, hsH, (float)hsSkew, t,        \
                     frame, seedh);                                               \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)wow,      \
                     (float)flutter, (float)bounce, hsH, (float)hsSkew, t,        \
                     frame, seedh);                                               \
    } while (0)

    switch (depth) {
      case OFX::eBitDepthUByte:  DISPATCH(unsigned char, 255); break;
      case OFX::eBitDepthUShort: DISPATCH(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  DISPATCH(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef DISPATCH
  }
};

// ---------------------------------------------------------------------------
mDeclarePluginFactory(TapeWowFactory, {}, {});

using namespace OFX;

void TapeWowFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Tape Wow", "Tape Wow", "Tape Wow");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // displaced rows wrap the full width
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void TapeWowFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *wo = desc.defineDoubleParam("wow");
  wo->setLabels("Wow", "Wow", "Wow");
  wo->setRange(0, 64); wo->setDisplayRange(0, 24); wo->setDefault(6);

  DoubleParamDescriptor *fl = desc.defineDoubleParam("flutter");
  fl->setLabels("Flutter", "Flutter", "Flutter");
  fl->setRange(0, 32); fl->setDisplayRange(0, 12); fl->setDefault(3);

  DoubleParamDescriptor *bo = desc.defineDoubleParam("bounce");
  bo->setLabels("Bounce", "Bounce", "Bounce");
  bo->setRange(0, 32); bo->setDisplayRange(0, 12); bo->setDefault(2);

  IntParamDescriptor *hh = desc.defineIntParam("hsH");
  hh->setLabels("Head switch", "Head switch", "Head switch");
  hh->setRange(0, 64); hh->setDisplayRange(0, 32); hh->setDefault(8);

  DoubleParamDescriptor *hs = desc.defineDoubleParam("hsSkew");
  hs->setLabels("Head skew", "Head skew", "Head skew");
  hs->setRange(0, 128); hs->setDisplayRange(0, 64); hs->setDefault(24);

  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed");
  sp->setRange(0, 4); sp->setDisplayRange(0, 2); sp->setDefault(1);

  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed");
  se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(0);
}

OFX::ImageEffect *TapeWowFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new TapeWowPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static TapeWowFactory p("org.purzos.tapeWow", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
