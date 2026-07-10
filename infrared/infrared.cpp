// Infrared — Aerochrome / colour-IR false colour. Kodak's infrared film routed
// the near-IR that foliage reflects into the red layer, so healthy vegetation
// blazed pink/magenta while skies went cyan. We emulate that channel routing
// (green -> red, red -> green) and push greens toward magenta and blues toward
// cyan.
//
// Strictly per-pixel colour map — no neighbours, no randomness. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

class InfraredProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float strength = 0.8f, foliage = 0.8f, sky = 0.5f, mix = 1.f;
  explicit InfraredProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const float r = c[0], g = c[1], b = c[2];
        // classic aerochrome channel swap: greens feed red, reds feed green.
        float o[3] = { g, r, b };
        // vegetation (green-dominant) -> magenta/pink
        const float veg = clamp01(g - std::max(r, b));
        o[0] = clamp01(o[0] + veg * foliage * 0.6f);
        o[2] = clamp01(o[2] + veg * foliage * 0.5f);
        o[1] = clamp01(o[1] - veg * foliage * 0.2f);
        // sky (blue-dominant) -> cyan
        const float skyness = clamp01(b - std::max(r, g));
        o[1] = clamp01(o[1] + skyness * sky * 0.5f);
        o[0] = clamp01(o[0] - skyness * sky * 0.3f);
        // blend the false-colour by strength, then overall mix
        for (int k = 0; k < 3; k++) o[k] = lerp(c[k], o[k], strength);
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class InfraredPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_strength = nullptr, *_foliage = nullptr, *_sky = nullptr, *_mix = nullptr;
public:
  explicit InfraredPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _strength = fetchDoubleParam("strength"); _foliage = fetchDoubleParam("foliage");
    _sky = fetchDoubleParam("sky"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double strength, foliage, sky, mix;
    _strength->getValueAtTime(args.time, strength);
    _foliage->getValueAtTime(args.time, foliage);
    _sky->getValueAtTime(args.time, sky);
    _mix->getValueAtTime(args.time, mix);

    InfraredProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.strength = (float)strength; proc.foliage = (float)foliage;
    proc.sky = (float)sky; proc.mix = (float)mix;

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

mDeclarePluginFactory(InfraredFactory, {}, {});
using namespace OFX;
void InfraredFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Infrared", true); }
void InfraredFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength"); st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.8);
  DoubleParamDescriptor *fo = desc.defineDoubleParam("foliage");
  fo->setLabels("Foliage", "Foliage", "Foliage"); fo->setRange(0, 1); fo->setDisplayRange(0, 1); fo->setDefault(0.8);
  fo->setHint("How far greens push toward magenta/pink");
  DoubleParamDescriptor *sk = desc.defineDoubleParam("sky");
  sk->setLabels("Sky", "Sky", "Sky"); sk->setRange(0, 1); sk->setDisplayRange(0, 1); sk->setDefault(0.5);
  sk->setHint("Cyan push of blue skies");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *InfraredFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new InfraredPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static InfraredFactory p("org.purzos.infrared", 1, 0); ids.push_back(&p);
}
} }
