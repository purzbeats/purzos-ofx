// Phosphor Melt — bright pixels burn in and DRIP. Everything above the
// threshold leaves a decaying trail in the chosen direction, like phosphor
// burn smeared by a long exposure, or a laser show dragged down the tube.
//
// One directional IIR per line: trail = max(seed, trail * decay), taken per
// RGB channel so trails keep their colour. Continuous, content-driven, no
// randomness — fully deterministic. Runs on the host's render thread.
// Directions are in top-down screen space (Down means down).

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
class PhosphorMeltProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _melt; // W*H*3, top-down
public:
  explicit PhosphorMeltProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  // seed value: the pixel's channels if its luma clears the threshold, else 0
  template <class PIX, int nComps, int maxv>
  inline void seedAt(int lx, int ly, float thr, float s[3]) const {
    PIX *p = (PIX *)_src->getPixelAddress(_bounds.x1 + lx, _bounds.y2 - 1 - ly);
    if (!p) { s[0] = s[1] = s[2] = 0; return; }
    const float L = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / (float)maxv;
    if (L >= thr) { s[0] = p[0] / (float)maxv; s[1] = p[1] / (float)maxv; s[2] = p[2] / (float)maxv; }
    else s[0] = s[1] = s[2] = 0;
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float decay, float thr, float strength, int dir) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    _melt.assign((size_t)_W * _H * 3, 0.0f);

    // build the trail field with a directional IIR (top-down logical space)
    const bool vertical = (dir == 0 || dir == 1);
    const int outer = vertical ? _W : _H;
    const int inner = vertical ? _H : _W;
    const bool reverse = (dir == 1 || dir == 3); // Up / Left run backwards
    float s[3];
    for (int o = 0; o < outer; o++) {
      if (_effect.abort()) return;
      float acc0 = 0, acc1 = 0, acc2 = 0;
      for (int i = 0; i < inner; i++) {
        const int ii = reverse ? inner - 1 - i : i;
        const int lx = vertical ? o : ii;
        const int ly = vertical ? ii : o;
        seedAt<PIX, nComps, maxv>(lx, ly, thr, s);
        acc0 = std::max(s[0], acc0 * decay);
        acc1 = std::max(s[1], acc1 * decay);
        acc2 = std::max(s[2], acc2 * decay);
        float *m = &_melt[((size_t)ly * _W + lx) * 3];
        m[0] = acc0; m[1] = acc1; m[2] = acc2;
      }
    }

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
      const int ly = _bounds.y2 - 1 - y;

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        PIX *p = row + lx * nComps;
        const float *m = &_melt[((size_t)ly * _W + lx) * 3];
        for (int c = 0; c < 3; c++) {
          const float v = p[c] / (float)maxv;
          dst[c] = q<PIX, nComps, maxv>(std::max(v, m[c] * strength));
        }
        if (nComps == 4) dst[3] = p[3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class PhosphorMeltPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_decay = nullptr, *_thr = nullptr, *_strength = nullptr;
  OFX::ChoiceParam *_dir = nullptr;
public:
  explicit PhosphorMeltPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _decay = fetchDoubleParam("decay");
    _thr = fetchDoubleParam("threshold");
    _strength = fetchDoubleParam("strength");
    _dir = fetchChoiceParam("dir");
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

    double decay, thr, strength; int dir;
    _decay->getValueAtTime(args.time, decay);
    _thr->getValueAtTime(args.time, thr);
    _strength->getValueAtTime(args.time, strength);
    _dir->getValueAtTime(args.time, dir);

    // trail LENGTH ~ 1/(1-decay) px: rescale the pole for proxy renders
    const double rs = args.renderScale.x;
    decay = clampv(1.0 - (1.0 - decay) / std::max(0.01, rs), 0.0, 0.999);

    PhosphorMeltProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)decay,    \
                     (float)thr, (float)strength, dir);                           \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)decay,    \
                     (float)thr, (float)strength, dir);                           \
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
mDeclarePluginFactory(PhosphorMeltFactory, {}, {});

using namespace OFX;

void PhosphorMeltFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Phosphor Melt", "Phosphor Melt", "Phosphor Melt");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // trails run the full line
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void PhosphorMeltFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *de = desc.defineDoubleParam("decay");
  de->setLabels("Decay", "Decay", "Decay");
  de->setRange(0.5, 0.999); de->setDisplayRange(0.85, 0.995); de->setDefault(0.96);

  DoubleParamDescriptor *th = desc.defineDoubleParam("threshold");
  th->setLabels("Threshold", "Threshold", "Threshold");
  th->setRange(0, 1); th->setDisplayRange(0, 1); th->setDefault(0.55);

  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength");
  st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.9);

  ChoiceParamDescriptor *di = desc.defineChoiceParam("dir");
  di->setLabels("Direction", "Direction", "Direction");
  di->appendOption("Down");
  di->appendOption("Up");
  di->appendOption("Right");
  di->appendOption("Left");
  di->setDefault(0);
}

OFX::ImageEffect *PhosphorMeltFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new PhosphorMeltPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static PhosphorMeltFactory p("org.purzos.phosphorMelt", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
