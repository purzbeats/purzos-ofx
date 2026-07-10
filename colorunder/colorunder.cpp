// Color Under — the chroma-noise grief of VHS's "color-under" system, where the
// down-converted colour carrier is noisy and smeared, so rainbow speckle crawls
// through the picture and pools in the shadows. We push deterministic value
// noise into the I/Q chroma channels, weighted toward the dark parts and
// animated over time.
//
// Strictly per-pixel (own coords + noise) -> tiles ON. Deterministic.

#include "../common/purzfx.hpp"

using namespace purz;

class ColorUnderProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0, seed = 1;
  float amount = 0.5f, sat = 0.6f, scale = 8.f, shadowBias = 1.f, speed = 0.5f, mix = 1.f;
  explicit ColorUnderProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invS = 1.f / std::max(1e-3f, scale);
    const float t = speed * (float)frame;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float Y, I, Q; rgb2yiq(c[0], c[1], c[2], Y, I, Q);
        float w = std::pow(clamp01(1.f - luma(c[0], c[1], c[2])), shadowBias);
        float fx = (float)(x - s.b.x1) * invS, fy = (float)ty * invS;
        float nI = vnoise(fx + t * 0.13f, fy, seed) - 0.5f;
        float nQ = vnoise(fx, fy + t * 0.11f, seed + 55) - 0.5f;
        float k = amount * sat * w;
        I += nI * k; Q += nQ * k;
        float o[4]; yiq2rgb(Y, I, Q, o[0], o[1], o[2]); o[3] = c[3];
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ColorUnderPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_sat = nullptr, *_scale = nullptr, *_shadowBias = nullptr, *_speed = nullptr, *_mix = nullptr;
  OFX::IntParam *_seed = nullptr;
public:
  explicit ColorUnderPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _sat = fetchDoubleParam("sat");
    _scale = fetchDoubleParam("scale"); _shadowBias = fetchDoubleParam("shadowBias");
    _speed = fetchDoubleParam("speed"); _seed = fetchIntParam("seed");
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

    double amount, sat, scale, shadowBias, speed, mix; int seed;
    _amount->getValueAtTime(args.time, amount);
    _sat->getValueAtTime(args.time, sat);
    _scale->getValueAtTime(args.time, scale);
    _shadowBias->getValueAtTime(args.time, shadowBias);
    _speed->getValueAtTime(args.time, speed);
    _seed->getValueAtTime(args.time, seed);
    _mix->getValueAtTime(args.time, mix);

    ColorUnderProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    proc.frame = (int)std::floor(args.time); proc.seed = seed;
    proc.amount = (float)amount; proc.sat = (float)sat;
    proc.scale = (float)(scale * args.renderScale.x); proc.shadowBias = (float)shadowBias;
    proc.speed = (float)speed; proc.mix = (float)mix;

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

mDeclarePluginFactory(ColorUnderFactory, {}, {});
using namespace OFX;
void ColorUnderFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Color Under", true); }
void ColorUnderFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.5);
  DoubleParamDescriptor *sa = desc.defineDoubleParam("sat");
  sa->setLabels("Saturation", "Saturation", "Saturation"); sa->setRange(0, 1); sa->setDisplayRange(0, 1); sa->setDefault(0.6);
  sa->setHint("Chroma-noise saturation");
  DoubleParamDescriptor *sc = desc.defineDoubleParam("scale");
  sc->setLabels("Scale", "Scale", "Scale"); sc->setRange(1, 40); sc->setDisplayRange(1, 40); sc->setDefault(8);
  sc->setHint("Speckle cell size");
  DoubleParamDescriptor *sb = desc.defineDoubleParam("shadowBias");
  sb->setLabels("Shadow Bias", "Shadow Bias", "Shadow Bias"); sb->setRange(0, 2); sb->setDisplayRange(0, 2); sb->setDefault(1);
  sb->setHint("Concentrate the noise in the dark parts");
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-4, 4); sp->setDisplayRange(-4, 4); sp->setDefault(0.5);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ColorUnderFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ColorUnderPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ColorUnderFactory p("org.purzos.colorUnder", 1, 0); ids.push_back(&p);
}
} }
