// Rainbow Phase — NTSC: Never The Same Color. Three chroma-phase failures of
// composite video, per scanline in YIQ:
//   • HUE DRIFT   — the subcarrier phase wanders line-to-line and crawls with
//                   the frame counter, so hue ripples slowly up the picture,
//   • CROSS COLOR — fine luma detail near the subcarrier frequency decodes as
//                   colour: pinstripes and tweed shimmer in rainbows,
//   • DOT CRAWL   — the complementary error: strong chroma edges decode as a
//                   moving luma checker that crawls along coloured boundaries.
//
// Everything is sines of (row, column, frame) — no randomness, deterministic.
// Runs on the host's render thread. OFX rows are bottom-up; line phase is
// computed on the top-down row index so the crawl direction matches a real
// raster scan.

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
} // namespace

// ---------------------------------------------------------------------------
class RainbowPhaseProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _Y, _I, _Q, _cm;
public:
  explicit RainbowPhaseProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float hueAmp, float cross, float crawl,
               float omega, float t) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    _Y.resize(_W); _I.resize(_W); _Q.resize(_W); _cm.resize(_W);

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
      const int ly = _bounds.y2 - 1 - y; // top-down row (raster order)

      for (int lx = 0; lx < _W; lx++) {
        PIX *p = row + lx * nComps;
        rgb2yiq(p[0] / (float)maxv, p[1] / (float)maxv, p[2] / (float)maxv,
                _Y[lx], _I[lx], _Q[lx]);
        _cm[lx] = std::fabs(_I[lx]) + std::fabs(_Q[lx]);
      }

      // per-line subcarrier phase error, crawling with the frame counter
      const float hue = hueAmp * (std::sin(ly * 0.71f + t * 1.13f)
                                + 0.5f * std::sin(ly * 2.31f - t * 0.67f));
      const float ch = std::cos(hue), sh = std::sin(hue);
      // NTSC alternates subcarrier phase per line; 180° per row + frame crawl
      const float linePhase = (ly & 1) * 3.14159265f + t * 3.14159265f;

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        float I = _I[lx] * ch - _Q[lx] * sh;
        float Q = _I[lx] * sh + _Q[lx] * ch;
        float Y = _Y[lx];

        const int l = std::max(0, lx - 1), r = std::min(_W - 1, lx + 1);
        if (cross > 0.0f) { // fine luma detail decodes as spinning chroma
          const float hp = _Y[lx] - (_Y[l] + _Y[lx] + _Y[r]) * (1.0f / 3.0f);
          const float ph = lx * omega + linePhase;
          I += cross * hp * 2.2f * std::cos(ph);
          Q += cross * hp * 2.2f * std::sin(ph);
        }
        if (crawl > 0.0f) { // chroma edges decode as a crawling luma checker
          const float hpc = _cm[lx] - (_cm[l] + _cm[lx] + _cm[r]) * (1.0f / 3.0f);
          Y += crawl * hpc * 0.8f * std::cos(lx * omega + linePhase);
        }

        float rr, gg, bb;
        yiq2rgb(Y, I, Q, rr, gg, bb);
        dst[0] = q<PIX, nComps, maxv>(rr);
        dst[1] = q<PIX, nComps, maxv>(gg);
        dst[2] = q<PIX, nComps, maxv>(bb);
        if (nComps == 4) dst[3] = row[lx * nComps + 3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class RainbowPhasePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_hue = nullptr, *_cross = nullptr, *_crawl = nullptr, *_speed = nullptr;
public:
  explicit RainbowPhasePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _hue = fetchDoubleParam("hue");
    _cross = fetchDoubleParam("cross");
    _crawl = fetchDoubleParam("crawl");
    _speed = fetchDoubleParam("speed");
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

    double hue, cross, crawl, speed;
    _hue->getValueAtTime(args.time, hue);
    _cross->getValueAtTime(args.time, cross);
    _crawl->getValueAtTime(args.time, crawl);
    _speed->getValueAtTime(args.time, speed);

    // subcarrier period ~3 full-res px; scale for proxy renders
    const float omega = (float)(2.0 * 3.14159265 / (3.0 * args.renderScale.x));
    const float hueRad = (float)(hue * 3.14159265 / 180.0);
    const float t = (float)(args.time * speed);

    RainbowPhaseProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, hueRad,          \
                     (float)cross, (float)crawl, omega, t);                       \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, hueRad,          \
                     (float)cross, (float)crawl, omega, t);                       \
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
mDeclarePluginFactory(RainbowPhaseFactory, {}, {});

using namespace OFX;

void RainbowPhaseFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Rainbow Phase", "Rainbow Phase", "Rainbow Phase");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void RainbowPhaseFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *hu = desc.defineDoubleParam("hue");
  hu->setLabels("Hue drift", "Hue drift", "Hue drift");
  hu->setRange(0, 45); hu->setDisplayRange(0, 30); hu->setDefault(10);

  DoubleParamDescriptor *cr = desc.defineDoubleParam("cross");
  cr->setLabels("Cross colour", "Cross colour", "Cross colour");
  cr->setRange(0, 1); cr->setDisplayRange(0, 1); cr->setDefault(0.4);

  DoubleParamDescriptor *dc = desc.defineDoubleParam("crawl");
  dc->setLabels("Dot crawl", "Dot crawl", "Dot crawl");
  dc->setRange(0, 1); dc->setDisplayRange(0, 1); dc->setDefault(0.35);

  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed");
  sp->setRange(0, 4); sp->setDisplayRange(0, 2); sp->setDefault(1);
}

OFX::ImageEffect *RainbowPhaseFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new RainbowPhasePlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static RainbowPhaseFactory p("org.purzos.rainbowPhase", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
