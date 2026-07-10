// Hum — mains AC hum leaking into the signal: slow horizontal brightness bars
// that roll down the frame, each bar giving a little sideways push as the
// picture's timing wobbles. `bars` sets how many bars stack down the frame,
// `drift` rolls them over time.
//
// Reads a horizontally SHIFTED source (bilin) -> tiles OFF. Deterministic
// (time enters only through the roll phase, frame index).

#include "../common/purzfx.hpp"

using namespace purz;

namespace { const float PI = 3.14159265358979f; }

class HumProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0;
  float bars = 2.f, strength = 0.3f, drift = 0.5f, warp = 6.f, mix = 1.f;
  explicit HumProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invH = H > 0 ? 1.f / H : 0.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);
      float b = std::sin(2.f * PI * bars * (float)ty * invH - drift * (float)frame * 0.1f);
      float bright = 1.f + strength * 0.5f * b;
      float push = warp * b;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float o[4]; s.bilin((float)x - push, (float)y, o);
        o[0] = clamp01(o[0] * bright);
        o[1] = clamp01(o[1] * bright);
        o[2] = clamp01(o[2] * bright);
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(c[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class HumPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_bars = nullptr, *_strength = nullptr, *_drift = nullptr, *_warp = nullptr, *_mix = nullptr;
public:
  explicit HumPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _bars = fetchDoubleParam("bars"); _strength = fetchDoubleParam("strength");
    _drift = fetchDoubleParam("drift"); _warp = fetchDoubleParam("warp");
    _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double bars, strength, drift, warp, mix;
    _bars->getValueAtTime(args.time, bars);
    _strength->getValueAtTime(args.time, strength);
    _drift->getValueAtTime(args.time, drift);
    _warp->getValueAtTime(args.time, warp);
    _mix->getValueAtTime(args.time, mix);

    HumProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    proc.frame = (int)std::floor(args.time);
    proc.bars = (float)bars; proc.strength = (float)strength; proc.drift = (float)drift;
    proc.warp = (float)(warp * args.renderScale.x); proc.mix = (float)mix;

#define GO(PIX, MAXV) do { if (nc == 4) proc.run<PIX, 4, MAXV>(args.renderWindow); \
                           else         proc.run<PIX, 3, MAXV>(args.renderWindow); } while (0)
    switch (depth) {
      case OFX::eBitDepthUByte:  GO(unsigned char, 255); break;
      case OFX::eBitDepthUShort: GO(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  GO(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef GO
  }
};

mDeclarePluginFactory(HumFactory, {}, {});
using namespace OFX;
void HumFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Hum", false); }
void HumFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *ba = desc.defineDoubleParam("bars");
  ba->setLabels("Bars", "Bars", "Bars"); ba->setRange(1, 6); ba->setDisplayRange(1, 6); ba->setDefault(2);
  ba->setHint("How many brightness bars stack down the frame");
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength"); st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.3);
  st->setHint("Brightness ripple amount");
  DoubleParamDescriptor *dr = desc.defineDoubleParam("drift");
  dr->setLabels("Drift", "Drift", "Drift"); dr->setRange(-4, 4); dr->setDisplayRange(-4, 4); dr->setDefault(0.5);
  dr->setHint("Vertical roll speed of the bars");
  DoubleParamDescriptor *wa = desc.defineDoubleParam("warp");
  wa->setLabels("Warp", "Warp", "Warp"); wa->setRange(0, 30); wa->setDisplayRange(0, 30); wa->setDefault(6);
  wa->setHint("Horizontal push in pixels per bar");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *HumFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new HumPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static HumFactory p("org.purzos.hum", 1, 0); ids.push_back(&p);
}
} }
