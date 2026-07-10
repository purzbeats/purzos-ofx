// Bleach — bleach-bypass / skip-bleach film look: pull most of the colour out,
// then overlay-blend the luminance back on top so the image gains a harsh,
// silvery, high-contrast metallic sheen — the classic desaturated war-film /
// music-video grade.
//
// Strictly per-pixel colour map — no neighbours, no randomness. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
static inline float overlay(float base, float blend) {
  return base < 0.5f ? 2.f * base * blend : 1.f - 2.f * (1.f - base) * (1.f - blend);
}
} // namespace

class BleachProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float amount = 0.7f, contrast = 1.2f, saturation = 0.5f, mix = 1.f;
  explicit BleachProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float keep = 1.f - saturation;                       // toward gray
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const float gray = luma(c[0], c[1], c[2]);
        float o[3];
        for (int k = 0; k < 3; k++) {
          float desat = lerp(c[k], gray, keep);                // desaturate
          float ov = overlay(desat, gray);                     // silver sheen
          float v = lerp(desat, ov, amount);
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

class BleachPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_contrast = nullptr, *_saturation = nullptr, *_mix = nullptr;
public:
  explicit BleachPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _contrast = fetchDoubleParam("contrast");
    _saturation = fetchDoubleParam("saturation"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double amount, contrast, saturation, mix;
    _amount->getValueAtTime(args.time, amount);
    _contrast->getValueAtTime(args.time, contrast);
    _saturation->getValueAtTime(args.time, saturation);
    _mix->getValueAtTime(args.time, mix);

    BleachProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.amount = (float)amount; proc.contrast = (float)contrast;
    proc.saturation = (float)saturation; proc.mix = (float)mix;

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

mDeclarePluginFactory(BleachFactory, {}, {});
using namespace OFX;
void BleachFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Bleach", true); }
void BleachFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.7);
  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast"); co->setRange(0.5, 2); co->setDisplayRange(0.5, 2); co->setDefault(1.2);
  DoubleParamDescriptor *sa = desc.defineDoubleParam("saturation");
  sa->setLabels("Saturation", "Saturation", "Saturation"); sa->setRange(0, 1); sa->setDisplayRange(0, 1); sa->setDefault(0.5);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *BleachFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new BleachPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static BleachFactory p("org.purzos.bleach", 1, 0); ids.push_back(&p);
}
} }
