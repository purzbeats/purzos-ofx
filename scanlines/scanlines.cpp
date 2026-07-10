// Scanlines — CRT scanline darkening + phosphor triad. Each output line is
// modulated by a cosine falling off with the beam gap so alternating rows read
// darker, and an optional RGB triad boosts one channel per column so the
// picture picks up the vertical phosphor-stripe shimmer of a shadow-mask tube.
//
// Strictly per-pixel (top-down `ty` + column phase) -> tiles are safe.
// Deterministic: no randomness, time never enters the maths.

#include "../common/purzfx.hpp"

using namespace purz;

static const float TAU = 6.28318530718f;

class ScanlinesProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int lineHeight = 3, H = 0;
  float strength = 0.5f, triad = 0.f, bright = 1.15f, mix = 1.f;
  explicit ScanlinesProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float lh = (float)std::max(1, lineHeight);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                 // top-down
      const float scan = 0.5f + 0.5f * std::cos(TAU * ty / lh);
      const float factor = (1.f - strength * (1.f - scan)) * bright;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const int ph = (((x - s.b.x1) % 3) + 3) % 3;       // RGB column phase
        float o[3];
        for (int k = 0; k < 3; k++) {
          const float tm = (ph == k) ? 1.f : (1.f - triad);
          o[k] = c[k] * factor * tm;
        }
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ScanlinesPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_lineHeight = nullptr;
  OFX::DoubleParam *_strength = nullptr, *_triad = nullptr, *_bright = nullptr, *_mix = nullptr;
public:
  explicit ScanlinesPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _lineHeight = fetchIntParam("lineHeight"); _strength = fetchDoubleParam("strength");
    _triad = fetchDoubleParam("triad"); _bright = fetchDoubleParam("bright");
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

    int lineHeight; double strength, triad, bright, mix;
    _lineHeight->getValueAtTime(args.time, lineHeight);
    _strength->getValueAtTime(args.time, strength);
    _triad->getValueAtTime(args.time, triad);
    _bright->getValueAtTime(args.time, bright);
    _mix->getValueAtTime(args.time, mix);

    ScanlinesProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.lineHeight = std::max(1, (int)std::lround(lineHeight * args.renderScale.x));
    proc.strength = (float)strength; proc.triad = (float)triad;
    proc.bright = (float)bright; proc.mix = (float)mix;
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

mDeclarePluginFactory(ScanlinesFactory, {}, {});
using namespace OFX;
void ScanlinesFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Scanlines", true); }
void ScanlinesFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  IntParamDescriptor *lh = desc.defineIntParam("lineHeight");
  lh->setLabels("Line height", "Line height", "Line height");
  lh->setRange(1, 8); lh->setDisplayRange(1, 8); lh->setDefault(3);
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength"); st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.5);
  DoubleParamDescriptor *tr = desc.defineDoubleParam("triad");
  tr->setLabels("Triad", "Triad", "Triad"); tr->setRange(0, 1); tr->setDisplayRange(0, 1); tr->setDefault(0.0);
  DoubleParamDescriptor *br = desc.defineDoubleParam("bright");
  br->setLabels("Brightness", "Brightness", "Brightness"); br->setRange(0.5, 2); br->setDisplayRange(0.5, 2); br->setDefault(1.15);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ScanlinesFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ScanlinesPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ScanlinesFactory p("org.purzos.scanlines", 1, 0); ids.push_back(&p);
}
} }
