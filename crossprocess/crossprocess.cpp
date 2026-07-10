// Cross Process — the look of developing film in the wrong chemistry (C-41 in
// E-6 and vice versa): skewed per-channel tone curves that lift one colour's
// shadows, crush another's highlights and blow out contrast. Plus a Faded matte
// grade and a Teal-Orange blockbuster split.
//
// Strictly per-pixel colour map — per-channel curves, no neighbours, no
// randomness. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

class CrossProcessProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int preset = 0;
  float strength = 0.8f, contrast = 1.15f, mix = 1.f;
  explicit CrossProcessProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
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
        float o[3];
        switch (preset) {
          case 0: // C41-in-E6: crush red highlights, green mids, lift blue shadows
            o[0] = std::pow(r, 1.35f);
            o[1] = clamp01(g * 1.05f);
            o[2] = clamp01(b * 0.80f + 0.15f);
            break;
          case 1: // E6-in-C41: punchy warm contrast, crushed blue blacks
            o[0] = clamp01((r - 0.5f) * 1.3f + 0.55f);
            o[1] = clamp01((g - 0.5f) * 1.2f + 0.5f);
            o[2] = clamp01(std::pow(b, 1.3f) * 0.90f);
            break;
          case 2: // Faded: lift blacks, pull whites down (matte), faint warmth
            o[0] = clamp01(r * 0.80f + 0.12f);
            o[1] = clamp01(g * 0.80f + 0.11f);
            o[2] = clamp01(b * 0.80f + 0.14f);
            break;
          default: { // Teal-Orange: shadows teal, highlights orange
            const float L = luma(r, g, b) - 0.5f;
            o[0] = clamp01(r + L * 0.25f);
            o[1] = clamp01(g + L * 0.05f);
            o[2] = clamp01(b - L * 0.25f);
          } break;
        }
        for (int k = 0; k < 3; k++) {
          float v = lerp(c[k], o[k], strength);                // blend by strength
          o[k] = clamp01((v - 0.5f) * contrast + 0.5f);        // contrast about 0.5
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

class CrossProcessPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_preset = nullptr;
  OFX::DoubleParam *_strength = nullptr, *_contrast = nullptr, *_mix = nullptr;
public:
  explicit CrossProcessPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _preset = fetchChoiceParam("preset");
    _strength = fetchDoubleParam("strength"); _contrast = fetchDoubleParam("contrast");
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

    int preset; double strength, contrast, mix;
    _preset->getValueAtTime(args.time, preset);
    _strength->getValueAtTime(args.time, strength);
    _contrast->getValueAtTime(args.time, contrast);
    _mix->getValueAtTime(args.time, mix);

    CrossProcessProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.preset = clampv(preset, 0, 3);
    proc.strength = (float)strength; proc.contrast = (float)contrast; proc.mix = (float)mix;

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

mDeclarePluginFactory(CrossProcessFactory, {}, {});
using namespace OFX;
void CrossProcessFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Cross Process", true); }
void CrossProcessFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  ChoiceParamDescriptor *pr = desc.defineChoiceParam("preset");
  pr->setLabels("Preset", "Preset", "Preset");
  pr->appendOption("C41-in-E6");
  pr->appendOption("E6-in-C41");
  pr->appendOption("Faded");
  pr->appendOption("Teal-Orange");
  pr->setDefault(0);
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength"); st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(0.8);
  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast"); co->setRange(0.5, 2); co->setDisplayRange(0.5, 2); co->setDefault(1.15);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *CrossProcessFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new CrossProcessPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static CrossProcessFactory p("org.purzos.crossProcess", 1, 0); ids.push_back(&p);
}
} }
