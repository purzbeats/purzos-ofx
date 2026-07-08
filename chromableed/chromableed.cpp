// Chroma Bleed — the VHS colour look. Composite-era tape stores chroma at a
// fraction of luma bandwidth, through a delay line, so colour SMEARS to the
// right and lags behind the sharp luma edge. Per scanline: RGB -> YIQ, the
// chroma is delayed, band-limited, then dragged through a one-pole trailing
// smear; luma passes through untouched.
//
// Pure per-row signal processing — no randomness at all, fully deterministic.
// Runs on the host's render thread. OFX rows are bottom-up; each row is an
// independent scanline so orientation only matters for row addressing.

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

// Rec.601 YIQ (NTSC) — the colour space the artifacts actually live in.
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
class ChromaBleedProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _Y, _I, _Q, _I2, _Q2;
public:
  explicit ChromaBleedProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float bleed, int delay, float sat) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    _Y.resize(_W); _I.resize(_W); _Q.resize(_W); _I2.resize(_W); _Q2.resize(_W);

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }

      // scanline -> YIQ, chroma read through the delay line
      for (int lx = 0; lx < _W; lx++) {
        PIX *p = row + lx * nComps;
        rgb2yiq(p[0] / (float)maxv, p[1] / (float)maxv, p[2] / (float)maxv,
                _Y[lx], _I[lx], _Q[lx]);
      }
      for (int lx = 0; lx < _W; lx++) {
        const int d = clampv(lx - delay, 0, _W - 1);
        _I2[lx] = _I[d]; _Q2[lx] = _Q[d];
      }
      // gentle band-limit (1-2-1) into _I/_Q (their originals are spent), THEN
      // the one-pole trailing smear — two passes so the blur never reads its
      // own smeared output back in.
      for (int lx = 0; lx < _W; lx++) {
        const int l = std::max(0, lx - 1), r = std::min(_W - 1, lx + 1);
        _I[lx] = (_I2[l] + 2.0f * _I2[lx] + _I2[r]) * 0.25f;
        _Q[lx] = (_Q2[l] + 2.0f * _Q2[lx] + _Q2[r]) * 0.25f;
      }
      float pi = _I[0], pq = _Q[0];
      for (int lx = 0; lx < _W; lx++) {
        pi = _I[lx] + (pi - _I[lx]) * bleed;
        pq = _Q[lx] + (pq - _Q[lx]) * bleed;
        _I2[lx] = pi * sat; _Q2[lx] = pq * sat;
      }

      dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        float r, g, b;
        yiq2rgb(_Y[lx], _I2[lx], _Q2[lx], r, g, b);
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
class ChromaBleedPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_bleed = nullptr, *_sat = nullptr;
  OFX::IntParam *_delay = nullptr;
public:
  explicit ChromaBleedPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _bleed = fetchDoubleParam("bleed");
    _delay = fetchIntParam("delay");
    _sat = fetchDoubleParam("sat");
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

    double bleed, sat; int delay;
    _bleed->getValueAtTime(args.time, bleed);
    _delay->getValueAtTime(args.time, delay);
    _sat->getValueAtTime(args.time, sat);

    // keep the smear the same LENGTH in full-res pixels under proxy renders:
    // smear length ~ 1/(1-bleed), so rescale the pole, and scale the delay.
    const double rs = args.renderScale.x;
    bleed = clampv(1.0 - (1.0 - bleed) / std::max(0.01, rs), 0.0, 0.98);
    delay = (int)std::lround(delay * rs);

    ChromaBleedProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)bleed,    \
                     delay, (float)sat);                                          \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)bleed,    \
                     delay, (float)sat);                                          \
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
mDeclarePluginFactory(ChromaBleedFactory, {}, {});

using namespace OFX;

void ChromaBleedFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Chroma Bleed", "Chroma Bleed", "Chroma Bleed");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // the smear runs the whole scanline
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void ChromaBleedFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *bl = desc.defineDoubleParam("bleed");
  bl->setLabels("Bleed", "Bleed", "Bleed");
  bl->setRange(0, 0.95); bl->setDisplayRange(0, 0.95); bl->setDefault(0.65);

  IntParamDescriptor *de = desc.defineIntParam("delay");
  de->setLabels("Chroma delay", "Chroma delay", "Chroma delay");
  de->setRange(0, 32); de->setDisplayRange(0, 16); de->setDefault(4);

  DoubleParamDescriptor *sa = desc.defineDoubleParam("sat");
  sa->setLabels("Saturation", "Saturation", "Saturation");
  sa->setRange(0, 2); sa->setDisplayRange(0, 2); sa->setDefault(1.05);
}

OFX::ImageEffect *ChromaBleedFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ChromaBleedPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ChromaBleedFactory p("org.purzos.chromaBleed", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
