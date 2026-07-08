// Scan Warp — a Rutt/Etra scan processor in a box. The 1972 video synth that
// deflected the raster vertically by the signal's own brightness, turning
// pictures into glowing terrain: bright areas physically LIFT the scanlines.
//
// The image displaces ITSELF: a softened luma map drives per-pixel vertical
// displacement, with two fixed-point refinement steps so the warp follows the
// terrain rather than tearing. Content-driven, no randomness, deterministic.
// Runs on the host's render thread. Displacement is computed in top-down
// space so positive Amount lifts bright pixels UP, like the original.

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
} // namespace

// ---------------------------------------------------------------------------
class ScanWarpProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _L, _tmp, _pref; // top-down softened luma
public:
  explicit ScanWarpProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  inline float lumaAt(int lx, int ly) const {
    lx = clampv(lx, 0, _W - 1); ly = clampv(ly, 0, _H - 1);
    return _L[(size_t)ly * _W + lx];
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float amount, int smooth, float lift) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    _L.assign((size_t)_W * _H, 0.0f);
    _tmp.resize((size_t)_W * _H);
    _pref.resize((size_t)std::max(_W, _H) + 1);

    // top-down luma map
    for (int ly = 0; ly < _H; ly++) {
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, _bounds.y2 - 1 - ly);
      if (!row) continue;
      float *m = &_L[(size_t)ly * _W];
      for (int x = 0; x < _W; x++) {
        PIX *p = row + x * nComps;
        m[x] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / (float)maxv;
      }
    }
    // soften it so the terrain flows instead of tearing (H then V box)
    if (smooth > 0) {
      for (int y = 0; y < _H; y++) {
        const float *rw = &_L[(size_t)y * _W];
        _pref[0] = 0.0f;
        for (int x = 0; x < _W; x++) _pref[x + 1] = _pref[x] + rw[x];
        float *o = &_tmp[(size_t)y * _W];
        for (int x = 0; x < _W; x++) {
          const int a = std::max(0, x - smooth), b = std::min(_W - 1, x + smooth);
          o[x] = (_pref[b + 1] - _pref[a]) / (float)(b - a + 1);
        }
      }
      for (int x = 0; x < _W; x++) {
        _pref[0] = 0.0f;
        for (int y = 0; y < _H; y++) _pref[y + 1] = _pref[y] + _tmp[(size_t)y * _W + x];
        for (int y = 0; y < _H; y++) {
          const int a = std::max(0, y - smooth), b = std::min(_H - 1, y + smooth);
          _L[(size_t)y * _W + x] = (_pref[b + 1] - _pref[a]) / (float)(b - a + 1);
        }
      }
    }

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y; // top-down row

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        // fixed-point iteration: where must the source row be so that, after
        // ITS lift, it lands here? (bright lifts UP => source is BELOW)
        float sy = (float)ly;
        for (int it = 0; it < 3; it++)
          sy = ly + amount * (lumaAt(lx, (int)std::lround(sy)) - lift);
        const int isy = clampv((int)std::lround(sy), 0, _H - 1);

        PIX *p = (PIX *)_src->getPixelAddress(_bounds.x1 + clampv(lx, 0, _W - 1),
                                              _bounds.y2 - 1 - isy);
        if (!p) { for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
        for (int c = 0; c < nComps; c++) dst[c] = p[c];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class ScanWarpPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_lift = nullptr;
  OFX::IntParam *_smooth = nullptr;
public:
  explicit ScanWarpPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount");
    _smooth = fetchIntParam("smooth");
    _lift = fetchDoubleParam("lift");
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

    double amount, lift; int smooth;
    _amount->getValueAtTime(args.time, amount);
    _smooth->getValueAtTime(args.time, smooth);
    _lift->getValueAtTime(args.time, lift);

    // pixel-unit params scale for proxy/half-res renders
    const double rs = args.renderScale.x;
    amount *= rs;
    smooth = (int)std::lround(smooth * rs);

    ScanWarpProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)amount,   \
                     smooth, (float)lift);                                        \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)amount,   \
                     smooth, (float)lift);                                        \
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
mDeclarePluginFactory(ScanWarpFactory, {}, {});

using namespace OFX;

void ScanWarpFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Scan Warp", "Scan Warp", "Scan Warp");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // the luma terrain is whole-frame
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void ScanWarpFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount");
  am->setRange(-400, 400); am->setDisplayRange(-150, 150); am->setDefault(60);

  IntParamDescriptor *sm = desc.defineIntParam("smooth");
  sm->setLabels("Smooth", "Smooth", "Smooth");
  sm->setRange(0, 64); sm->setDisplayRange(0, 32); sm->setDefault(6);

  DoubleParamDescriptor *li = desc.defineDoubleParam("lift");
  li->setLabels("Zero level", "Zero level", "Zero level");
  li->setRange(0, 1); li->setDisplayRange(0, 1); li->setDefault(0.5);
}

OFX::ImageEffect *ScanWarpFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ScanWarpPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ScanWarpFactory p("org.purzos.scanWarp", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
