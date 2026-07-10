// Shadow Mask — the phosphor mask of a CRT tube. Each output pixel is multiplied
// by an RGB mask triple derived from its cell position: aperture-grille vertical
// RGB stripes, a slot mask (stripes broken by staggered vertical gaps), or a
// dot-triad matrix (RGB dots offset row-to-row). A brightness lift compensates
// for the light the mask swallows.
//
// Strictly per-pixel (top-down `ty` + cell arithmetic) -> tiles are safe.
// Deterministic: no randomness.

#include "../common/purzfx.hpp"

using namespace purz;

class ShadowMaskProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int type = 0, size = 3, H = 0;
  float strength = 0.5f, bright = 1.2f, mix = 1.f;
  explicit ShadowMaskProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int sz = std::max(1, size);
    const float lo = 1.f - strength;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                 // top-down
      const int row = ty / sz;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const int col = (x - s.b.x1) / sz;
        float m[3];
        if (type == 0) {                                   // aperture grille
          const int idx = (((col % 3) + 3) % 3);
          for (int k = 0; k < 3; k++) m[k] = (idx == k) ? 1.f : lo;
        } else if (type == 2) {                            // dot triad
          const int idx = ((((col + row) % 3) + 3) % 3);
          for (int k = 0; k < 3; k++) m[k] = (idx == k) ? 1.f : lo;
        } else {                                           // slot mask
          const int idx = (((col % 3) + 3) % 3);
          const int stagger = (col & 1) ? sz : 0;
          const int slotPhase = (((ty + stagger) / sz) & 1);
          const float rowDark = slotPhase ? 1.f : (1.f - strength * 0.5f);
          for (int k = 0; k < 3; k++) m[k] = ((idx == k) ? 1.f : lo) * rowDark;
        }
        float o[3];
        for (int k = 0; k < 3; k++) o[k] = c[k] * m[k] * bright;
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ShadowMaskPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_type = nullptr;
  OFX::IntParam *_size = nullptr;
  OFX::DoubleParam *_strength = nullptr, *_bright = nullptr, *_mix = nullptr;
public:
  explicit ShadowMaskPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _type = fetchChoiceParam("type"); _size = fetchIntParam("size");
    _strength = fetchDoubleParam("strength"); _bright = fetchDoubleParam("bright");
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

    int type, size; double strength, bright, mix;
    _type->getValueAtTime(args.time, type);
    _size->getValueAtTime(args.time, size);
    _strength->getValueAtTime(args.time, strength);
    _bright->getValueAtTime(args.time, bright);
    _mix->getValueAtTime(args.time, mix);

    ShadowMaskProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.type = type;
    proc.size = std::max(1, (int)std::lround(size * args.renderScale.x));
    proc.strength = (float)strength; proc.bright = (float)bright; proc.mix = (float)mix;
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

mDeclarePluginFactory(ShadowMaskFactory, {}, {});
using namespace OFX;
void ShadowMaskFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Shadow Mask", true); }
void ShadowMaskFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  ChoiceParamDescriptor *ty = desc.defineChoiceParam("type");
  ty->setLabels("Type", "Type", "Type");
  ty->appendOption("Aperture Grille"); ty->appendOption("Slot Mask"); ty->appendOption("Dot Triad");
  ty->setDefault(0);
  IntParamDescriptor *sz = desc.defineIntParam("size");
  sz->setLabels("Cell size", "Cell size", "Cell size");
  sz->setRange(2, 8); sz->setDisplayRange(2, 8); sz->setDefault(3);
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength"); st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.5);
  DoubleParamDescriptor *br = desc.defineDoubleParam("bright");
  br->setLabels("Brightness", "Brightness", "Brightness"); br->setRange(0.5, 2); br->setDisplayRange(0.5, 2); br->setDefault(1.2);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ShadowMaskFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ShadowMaskPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ShadowMaskFactory p("org.purzos.shadowMask", 1, 0); ids.push_back(&p);
}
} }
