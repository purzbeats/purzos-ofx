// Roll Bar — the hum bar of a mistuned CRT: a soft horizontal band of altered
// brightness that rolls up (or down) the frame over time, the beat you get when
// the mains frequency drifts against the field rate. Softness feathers the band
// edges; speed and sign set the roll.
//
// Strictly per-pixel (top-down `ty`, a per-row band factor) -> tiles are safe.
// Deterministic: the only time input is the integer frame index.

#include "../common/purzfx.hpp"

using namespace purz;

class RollBarProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int frame = 0, H = 0;
  float height = 0.3f, speed = 0.4f, strength = 0.35f, softness = 0.5f, mix = 1.f;
  explicit RollBarProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float Hf = (float)std::max(1, H);
    const float phase = speed * frame * 0.05f;
    const float edge = std::max(1e-4f, height * softness * 0.5f);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                 // top-down
      const float p = fracf(ty / Hf - phase);
      float band = 0.f;
      if (p < height) {
        const float up = smoothstep(0.f, edge, p);
        const float dn = smoothstep(0.f, edge, height - p);
        band = up * dn;
      }
      const float factor = 1.f + strength * band;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        dst[0] = q<PIX, maxv>(lerp(c[0], c[0] * factor, mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], c[1] * factor, mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], c[2] * factor, mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class RollBarPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_height = nullptr, *_speed = nullptr, *_strength = nullptr,
                   *_softness = nullptr, *_mix = nullptr;
public:
  explicit RollBarPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _height = fetchDoubleParam("height"); _speed = fetchDoubleParam("speed");
    _strength = fetchDoubleParam("strength"); _softness = fetchDoubleParam("softness");
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

    double height, speed, strength, softness, mix;
    _height->getValueAtTime(args.time, height);
    _speed->getValueAtTime(args.time, speed);
    _strength->getValueAtTime(args.time, strength);
    _softness->getValueAtTime(args.time, softness);
    _mix->getValueAtTime(args.time, mix);

    RollBarProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.height = (float)height; proc.speed = (float)speed; proc.strength = (float)strength;
    proc.softness = (float)softness; proc.mix = (float)mix;
    proc.frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;

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

mDeclarePluginFactory(RollBarFactory, {}, {});
using namespace OFX;
void RollBarFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Roll Bar", true); }
void RollBarFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *he = desc.defineDoubleParam("height");
  he->setLabels("Height", "Height", "Height"); he->setRange(0.05, 1); he->setDisplayRange(0.05, 1); he->setDefault(0.3);
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-4, 4); sp->setDisplayRange(-4, 4); sp->setDefault(0.4);
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength"); st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.35);
  DoubleParamDescriptor *so = desc.defineDoubleParam("softness");
  so->setLabels("Softness", "Softness", "Softness"); so->setRange(0, 1); so->setDisplayRange(0, 1); so->setDefault(0.5);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *RollBarFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new RollBarPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static RollBarFactory p("org.purzos.rollBar", 1, 0); ids.push_back(&p);
}
} }
