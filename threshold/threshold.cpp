// Threshold — 1-bit posterisation. Luma is compared against a level and mapped
// to one of two inks with a soft edge. The dither method breaks the hard cut so
// gradients survive as a stipple: Bayer adds an ordered 4x4 pattern, Noise adds
// a stable per-pixel hash (no frame term -> no flicker across a clip).
//
// Strictly per-pixel + a fixed Bayer cell / coordinate hash -> tiles are ON.
// Deterministic (no time, no rand).

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
static inline float bayer4(int x, int y) {
  static const int B[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
  return (B[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
}
} // namespace

class ThresholdProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, method = 0;
  float level = 0.5f, softness = 0.03f, amount = 0.5f, mix = 1.f;
  float dark[3] = {0.f, 0.f, 0.f}, light[3] = {0.95f, 0.95f, 0.97f};
  explicit ThresholdProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down (stable dither)
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float L = luma(c[0], c[1], c[2]);
        const int lx = x - s.b.x1;
        if (method == 1)      L += bayer4(lx, ty) * amount;
        else if (method == 2) L += (hash2(lx, ty) - 0.5f) * amount;
        const float f = smoothstep(level - softness, level + softness, L);
        float o[3];
        for (int k = 0; k < 3; k++) o[k] = lerp(dark[k], light[k], f);
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ThresholdPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_method = nullptr;
  OFX::DoubleParam *_level = nullptr, *_softness = nullptr, *_amount = nullptr, *_mix = nullptr;
  OFX::RGBParam *_dark = nullptr, *_light = nullptr;
public:
  explicit ThresholdPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _level = fetchDoubleParam("level"); _softness = fetchDoubleParam("softness");
    _method = fetchChoiceParam("method"); _amount = fetchDoubleParam("amount");
    _dark = fetchRGBParam("dark"); _light = fetchRGBParam("light");
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

    int method; double level, softness, amount, mix;
    _level->getValueAtTime(args.time, level);
    _softness->getValueAtTime(args.time, softness);
    _method->getValueAtTime(args.time, method);
    _amount->getValueAtTime(args.time, amount);
    _mix->getValueAtTime(args.time, mix);
    double dr, dg, db, lr, lg, lb;
    _dark->getValueAtTime(args.time, dr, dg, db);
    _light->getValueAtTime(args.time, lr, lg, lb);

    OfxRectI b = src->getBounds();
    ThresholdProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.H = b.y2 - b.y1; proc.method = method;
    proc.level = (float)level; proc.softness = (float)softness;
    proc.amount = (float)amount; proc.mix = (float)mix;
    proc.dark[0] = (float)dr; proc.dark[1] = (float)dg; proc.dark[2] = (float)db;
    proc.light[0] = (float)lr; proc.light[1] = (float)lg; proc.light[2] = (float)lb;

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

mDeclarePluginFactory(ThresholdFactory, {}, {});
using namespace OFX;
void ThresholdFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Threshold", true); }
void ThresholdFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *lv = desc.defineDoubleParam("level");
  lv->setLabels("Level", "Level", "Level"); lv->setRange(0, 1); lv->setDisplayRange(0, 1); lv->setDefault(0.5);
  DoubleParamDescriptor *so = desc.defineDoubleParam("softness");
  so->setLabels("Softness", "Softness", "Softness"); so->setRange(0, 0.5); so->setDisplayRange(0, 0.5); so->setDefault(0.03);
  ChoiceParamDescriptor *me = desc.defineChoiceParam("method");
  me->setLabels("Method", "Method", "Method");
  me->appendOption("Hard"); me->appendOption("Bayer"); me->appendOption("Noise");
  me->setDefault(0);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Dither", "Dither", "Dither"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.5);
  am->setHint("Strength of the Bayer / Noise dither");
  RGBParamDescriptor *dk = desc.defineRGBParam("dark");
  dk->setLabels("Dark", "Dark", "Dark"); dk->setDefault(0.0, 0.0, 0.0);
  RGBParamDescriptor *li = desc.defineRGBParam("light");
  li->setLabels("Light", "Light", "Light"); li->setDefault(0.95, 0.95, 0.97);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ThresholdFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ThresholdPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ThresholdFactory p("org.purzos.threshold", 1, 0); ids.push_back(&p);
}
} }
