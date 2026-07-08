// Luma Ring — bandwidth, ringing and ghosting: the luma half of a cheap 80s
// video chain. Per scanline, on Y only (chroma passes straight through):
//   • SOFTNESS   — band-limit: box-blur the luma like a 3MHz channel,
//   • RING       — the "detail" circuit: over-sharpened band-limited luma
//                  overshoots at every edge, leaving bright/dark halo lines,
//   • GHOST      — RF multipath: a delayed, attenuated copy of the signal
//                  added back a few dozen pixels late (negative gain gives
//                  the inverted ghost of a badly terminated cable).
//
// Pure per-row signal processing — no randomness, fully deterministic. Runs
// on the host's render thread. Rows are independent scanlines.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace {
template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

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

// box blur of radius r over src into out (prefix-sum, O(W))
static void boxBlur(const std::vector<float> &src, std::vector<float> &sum,
                    std::vector<float> &out, int W, int r) {
  sum[0] = 0.0f;
  for (int x = 0; x < W; x++) sum[x + 1] = sum[x] + src[x];
  for (int x = 0; x < W; x++) {
    const int a = std::max(0, x - r), b = std::min(W - 1, x + r);
    out[x] = (sum[b + 1] - sum[a]) / (float)(b - a + 1);
  }
}
} // namespace

// ---------------------------------------------------------------------------
class LumaRingProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _Y, _I, _Q, _sum, _b1, _b2;
public:
  explicit LumaRingProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float soft, float ring, float ghost,
               int ghostDist, int radius) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    _Y.resize(_W); _I.resize(_W); _Q.resize(_W);
    _sum.resize(_W + 1); _b1.resize(_W); _b2.resize(_W);

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }

      for (int lx = 0; lx < _W; lx++) {
        PIX *p = row + lx * nComps;
        rgb2yiq(p[0] / (float)maxv, p[1] / (float)maxv, p[2] / (float)maxv,
                _Y[lx], _I[lx], _Q[lx]);
      }

      // band-limit, then overshoot against a wider blur (the halo maker)
      boxBlur(_Y, _sum, _b1, _W, radius);
      for (int lx = 0; lx < _W; lx++) _b1[lx] = _Y[lx] + (_b1[lx] - _Y[lx]) * soft;
      boxBlur(_b1, _sum, _b2, _W, radius * 2 + 1);
      for (int lx = 0; lx < _W; lx++) _b1[lx] += (_b1[lx] - _b2[lx]) * ring;

      // RF ghost: delayed attenuated echo, re-centred so exposure holds
      if (ghost != 0.0f && ghostDist > 0) {
        for (int lx = _W - 1; lx >= 0; lx--) {
          const int d = std::max(0, lx - ghostDist);
          _b1[lx] += ghost * (_b1[d] - 0.5f) * 0.5f;
        }
      }

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        float r, g, b;
        yiq2rgb(_b1[lx], _I[lx], _Q[lx], r, g, b);
        dst[0] = q<PIX, nComps, maxv>(r);
        dst[1] = q<PIX, nComps, maxv>(g);
        dst[2] = q<PIX, nComps, maxv>(b);
        if (nComps == 4) dst[3] = row[lx * nComps + 3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class LumaRingPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_soft = nullptr, *_ring = nullptr, *_ghost = nullptr;
  OFX::IntParam *_ghostDist = nullptr;
public:
  explicit LumaRingPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _soft = fetchDoubleParam("soft");
    _ring = fetchDoubleParam("ring");
    _ghost = fetchDoubleParam("ghost");
    _ghostDist = fetchIntParam("ghostDist");
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

    double soft, ring, ghost; int ghostDist;
    _soft->getValueAtTime(args.time, soft);
    _ring->getValueAtTime(args.time, ring);
    _ghost->getValueAtTime(args.time, ghost);
    _ghostDist->getValueAtTime(args.time, ghostDist);

    // pixel-unit distances scale for proxy/half-res renders
    const double rs = args.renderScale.x;
    const int radius = std::max(1, (int)std::lround(2.0 * rs));
    ghostDist = std::max(1, (int)std::lround(ghostDist * rs));

    LumaRingProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)soft,     \
                     (float)ring, (float)ghost, ghostDist, radius);               \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)soft,     \
                     (float)ring, (float)ghost, ghostDist, radius);               \
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
mDeclarePluginFactory(LumaRingFactory, {}, {});

using namespace OFX;

void LumaRingFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Luma Ring", "Luma Ring", "Luma Ring");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // filters run the whole scanline
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void LumaRingFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *so = desc.defineDoubleParam("soft");
  so->setLabels("Softness", "Softness", "Softness");
  so->setRange(0, 1); so->setDisplayRange(0, 1); so->setDefault(0.35);

  DoubleParamDescriptor *ri = desc.defineDoubleParam("ring");
  ri->setLabels("Ring", "Ring", "Ring");
  ri->setRange(0, 3); ri->setDisplayRange(0, 2); ri->setDefault(0.8);

  DoubleParamDescriptor *gh = desc.defineDoubleParam("ghost");
  gh->setLabels("Ghost", "Ghost", "Ghost");
  gh->setRange(-1, 1); gh->setDisplayRange(-1, 1); gh->setDefault(0.25);

  IntParamDescriptor *gd = desc.defineIntParam("ghostDist");
  gd->setLabels("Ghost distance", "Ghost distance", "Ghost distance");
  gd->setRange(2, 256); gd->setDisplayRange(8, 96); gd->setDefault(28);
}

OFX::ImageEffect *LumaRingFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new LumaRingPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static LumaRingFactory p("org.purzos.lumaRing", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
