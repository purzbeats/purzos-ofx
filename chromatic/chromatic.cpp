// Chromatic — VHS colour abuse in HSV: crank saturation, rotate the hue wheel,
// then crush the saturation into a few bands the way a low-bandwidth chroma
// channel would, and trim brightness. Cheap, lurid, tape-deck colour.
//
// Strictly per-pixel colour map — no neighbours, no randomness. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

class ChromaticProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int chromaCrush = 16;
  float satBoost = 1.4f, hueShift = 0.f, bright = 1.f, mix = 1.f;
  explicit ChromaticProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int levels = clampv(chromaCrush, 2, 16);
    const float qn = (float)(levels - 1);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float h, sat, v; rgb2hsv(c[0], c[1], c[2], h, sat, v);
        sat = clamp01(sat * satBoost);
        h = fracf(h + hueShift);
        sat = std::floor(sat * qn + 0.5f) / qn;               // chroma quantise
        v = clamp01(v * bright);
        float o[3]; hsv2rgb(h, sat, v, o[0], o[1], o[2]);
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ChromaticPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_satBoost = nullptr, *_hueShift = nullptr, *_bright = nullptr, *_mix = nullptr;
  OFX::IntParam *_chromaCrush = nullptr;
public:
  explicit ChromaticPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _satBoost = fetchDoubleParam("satBoost"); _hueShift = fetchDoubleParam("hueShift");
    _chromaCrush = fetchIntParam("chromaCrush"); _bright = fetchDoubleParam("bright");
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

    int chromaCrush; double satBoost, hueShift, bright, mix;
    _satBoost->getValueAtTime(args.time, satBoost);
    _hueShift->getValueAtTime(args.time, hueShift);
    _chromaCrush->getValueAtTime(args.time, chromaCrush);
    _bright->getValueAtTime(args.time, bright);
    _mix->getValueAtTime(args.time, mix);

    ChromaticProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.satBoost = (float)satBoost; proc.hueShift = (float)hueShift;
    proc.chromaCrush = chromaCrush; proc.bright = (float)bright; proc.mix = (float)mix;

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

mDeclarePluginFactory(ChromaticFactory, {}, {});
using namespace OFX;
void ChromaticFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Chromatic", true); }
void ChromaticFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *sb = desc.defineDoubleParam("satBoost");
  sb->setLabels("Saturation boost", "Saturation boost", "Saturation boost");
  sb->setRange(0, 3); sb->setDisplayRange(0, 3); sb->setDefault(1.4);
  DoubleParamDescriptor *hs = desc.defineDoubleParam("hueShift");
  hs->setLabels("Hue shift", "Hue shift", "Hue shift");
  hs->setRange(-0.5, 0.5); hs->setDisplayRange(-0.5, 0.5); hs->setDefault(0.0);
  hs->setHint("Hue rotation in turns");
  IntParamDescriptor *cc = desc.defineIntParam("chromaCrush");
  cc->setLabels("Chroma crush", "Chroma crush", "Chroma crush");
  cc->setRange(2, 16); cc->setDisplayRange(2, 16); cc->setDefault(16);
  cc->setHint("Saturation quantise levels (16 = off)");
  DoubleParamDescriptor *br = desc.defineDoubleParam("bright");
  br->setLabels("Brightness", "Brightness", "Brightness");
  br->setRange(0.5, 2); br->setDisplayRange(0.5, 2); br->setDefault(1.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ChromaticFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ChromaticPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ChromaticFactory p("org.purzos.chromatic", 1, 0); ids.push_back(&p);
}
} }
