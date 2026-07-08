// Halation — the glow film can't help making. Light punches through the
// emulsion, bounces off the base, and re-exposes the red-sensitive layer in a
// soft ring around every highlight. Same physics reads as CRT phosphor bloom
// with a white tint.
//
//   highlights above THRESHOLD -> big soft blur (two box passes) -> TINT ->
//   screened back over the frame.
//
// No randomness — fully deterministic. Whole-frame separable blur, so this
// one carries full-frame float buffers. Runs on the host's render thread.
// The blur is symmetric, so OFX's bottom-up rows need no conversion here.

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

struct Tint { const char *name; float r, g, b; };
static const Tint TINTS[] = {
  {"Film red",   1.00f, 0.32f, 0.12f},  // classic emulsion halation
  {"Warm amber", 1.00f, 0.62f, 0.22f},
  {"Phosphor",   0.88f, 0.94f, 1.00f},  // CRT bloom
  {"Green tube", 0.45f, 1.00f, 0.55f},
  {"Toxic",      0.75f, 1.00f, 0.20f},
};
static const int N_TINTS = sizeof(TINTS) / sizeof(TINTS[0]);
} // namespace

// ---------------------------------------------------------------------------
class HalationProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _mask, _tmp, _pref;
public:
  explicit HalationProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  void blurH(std::vector<float> &img, std::vector<float> &out, int r) {
    for (int y = 0; y < _H; y++) {
      const float *row = &img[(size_t)y * _W];
      _pref[0] = 0.0f;
      for (int x = 0; x < _W; x++) _pref[x + 1] = _pref[x] + row[x];
      float *orow = &out[(size_t)y * _W];
      for (int x = 0; x < _W; x++) {
        const int a = std::max(0, x - r), b = std::min(_W - 1, x + r);
        orow[x] = (_pref[b + 1] - _pref[a]) / (float)(b - a + 1);
      }
    }
  }
  void blurV(std::vector<float> &img, std::vector<float> &out, int r) {
    for (int x = 0; x < _W; x++) {
      _pref[0] = 0.0f;
      for (int y = 0; y < _H; y++) _pref[y + 1] = _pref[y] + img[(size_t)y * _W + x];
      for (int y = 0; y < _H; y++) {
        const int a = std::max(0, y - r), b = std::min(_H - 1, y + r);
        out[(size_t)y * _W + x] = (_pref[b + 1] - _pref[a]) / (float)(b - a + 1);
      }
    }
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float thr, float amount, int radius, int tintIdx) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    _mask.assign((size_t)_W * _H, 0.0f);
    _tmp.resize((size_t)_W * _H);
    _pref.resize((size_t)std::max(_W, _H) + 1);
    const Tint &tint = TINTS[clampv(tintIdx, 0, N_TINTS - 1)];

    // highlight mask from luma
    for (int y = 0; y < _H; y++) {
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, _bounds.y1 + y);
      if (!row) continue;
      float *m = &_mask[(size_t)y * _W];
      for (int x = 0; x < _W; x++) {
        PIX *p = row + x * nComps;
        const float L = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / (float)maxv;
        const float k = clampv((L - thr) / std::max(0.001f, 1.0f - thr), 0.0f, 1.0f);
        m[x] = k * k;
      }
    }

    // two box passes each way ~ smooth bloom
    if (_effect.abort()) return;
    blurH(_mask, _tmp, radius);
    blurV(_tmp, _mask, radius);
    if (_effect.abort()) return;
    blurH(_mask, _tmp, radius / 2 + 1);
    blurV(_tmp, _mask, radius / 2 + 1);

    // screen the tinted halo back over the source
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
      const float *m = &_mask[(size_t)(y - _bounds.y1) * _W];

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        PIX *p = row + lx * nComps;
        const float h = m[lx] * amount;
        const float hr = clampv(h * tint.r, 0.0f, 1.0f);
        const float hg = clampv(h * tint.g, 0.0f, 1.0f);
        const float hb = clampv(h * tint.b, 0.0f, 1.0f);
        const float r = p[0] / (float)maxv, g = p[1] / (float)maxv, b = p[2] / (float)maxv;
        dst[0] = q<PIX, nComps, maxv>(1.0f - (1.0f - r) * (1.0f - hr));
        dst[1] = q<PIX, nComps, maxv>(1.0f - (1.0f - g) * (1.0f - hg));
        dst[2] = q<PIX, nComps, maxv>(1.0f - (1.0f - b) * (1.0f - hb));
        if (nComps == 4) dst[3] = p[3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class HalationPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_thr = nullptr, *_amount = nullptr;
  OFX::IntParam *_radius = nullptr;
  OFX::ChoiceParam *_tint = nullptr;
public:
  explicit HalationPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _thr = fetchDoubleParam("threshold");
    _amount = fetchDoubleParam("amount");
    _radius = fetchIntParam("radius");
    _tint = fetchChoiceParam("tint");
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

    double thr, amount; int radius, tint;
    _thr->getValueAtTime(args.time, thr);
    _amount->getValueAtTime(args.time, amount);
    _radius->getValueAtTime(args.time, radius);
    _tint->getValueAtTime(args.time, tint);

    radius = std::max(1, (int)std::lround(radius * args.renderScale.x));

    HalationProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)thr,      \
                     (float)amount, radius, tint);                                \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)thr,      \
                     (float)amount, radius, tint);                                \
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
mDeclarePluginFactory(HalationFactory, {}, {});

using namespace OFX;

void HalationFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Halation", "Halation", "Halation");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // the bloom is whole-frame
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void HalationFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *th = desc.defineDoubleParam("threshold");
  th->setLabels("Threshold", "Threshold", "Threshold");
  th->setRange(0, 0.99); th->setDisplayRange(0.3, 0.95); th->setDefault(0.7);

  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount");
  am->setRange(0, 3); am->setDisplayRange(0, 2); am->setDefault(0.9);

  IntParamDescriptor *ra = desc.defineIntParam("radius");
  ra->setLabels("Radius", "Radius", "Radius");
  ra->setRange(1, 256); ra->setDisplayRange(4, 96); ra->setDefault(24);

  ChoiceParamDescriptor *ti = desc.defineChoiceParam("tint");
  ti->setLabels("Tint", "Tint", "Tint");
  for (int i = 0; i < N_TINTS; i++) ti->appendOption(TINTS[i].name);
  ti->setDefault(0);
}

OFX::ImageEffect *HalationFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new HalationPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static HalationFactory p("org.purzos.halation", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
