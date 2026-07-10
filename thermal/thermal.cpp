// Thermal — false-colour heat map. Collapse the pixel to luma, shape it with
// contrast/offset, then read that brightness through one of four thermographic
// gradient LUTs (Iron / Rainbow / Predator / Arctic) — the look of a FLIR
// camera or an 80s heat-vision HUD.
//
// Strictly per-pixel colour map — luma in, gradient out. No neighbours, no
// randomness. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
struct Grad { const char *name; int n; float rgb[6][3]; }; // sRGB 0..255 stops
static const Grad LUTS[] = {
  {"Iron",     6, {{0,0,0},{40,0,90},{160,20,100},{230,90,20},{255,200,40},{255,255,235}}},
  {"Rainbow",  5, {{10,0,80},{0,170,220},{40,210,60},{240,230,40},{230,30,30}}},
  {"Predator", 6, {{0,0,0},{40,0,80},{20,120,150},{40,200,60},{240,210,40},{255,255,255}}},
  {"Arctic",   5, {{0,0,20},{20,40,110},{20,150,170},{120,220,230},{255,255,255}}},
};
static const int N_LUTS = sizeof(LUTS) / sizeof(LUTS[0]);

static inline void gradAt(const Grad &G, float t, float o[3]) {
  t = clamp01(t) * (G.n - 1);
  int i = (int)std::floor(t); if (i >= G.n - 1) i = G.n - 2; if (i < 0) i = 0;
  float f = t - i;
  for (int k = 0; k < 3; k++)
    o[k] = lerp(G.rgb[i][k] / 255.f, G.rgb[i + 1][k] / 255.f, f);
}
} // namespace

class ThermalProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int palette = 0;
  float contrast = 1.1f, offset = 0.f, mix = 1.f;
  explicit ThermalProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const Grad &G = LUTS[palette];
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float L = clamp01(luma(c[0], c[1], c[2]) * contrast + offset);
        float o[3]; gradAt(G, L, o);
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ThermalPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_palette = nullptr;
  OFX::DoubleParam *_contrast = nullptr, *_offset = nullptr, *_mix = nullptr;
public:
  explicit ThermalPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _palette = fetchChoiceParam("palette");
    _contrast = fetchDoubleParam("contrast"); _offset = fetchDoubleParam("offset");
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

    int palette; double contrast, offset, mix;
    _palette->getValueAtTime(args.time, palette);
    _contrast->getValueAtTime(args.time, contrast);
    _offset->getValueAtTime(args.time, offset);
    _mix->getValueAtTime(args.time, mix);

    ThermalProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.palette = clampv(palette, 0, N_LUTS - 1);
    proc.contrast = (float)contrast; proc.offset = (float)offset; proc.mix = (float)mix;

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

mDeclarePluginFactory(ThermalFactory, {}, {});
using namespace OFX;
void ThermalFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Thermal", true); }
void ThermalFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  ChoiceParamDescriptor *pl = desc.defineChoiceParam("palette");
  pl->setLabels("Palette", "Palette", "Palette");
  for (int i = 0; i < N_LUTS; i++) pl->appendOption(LUTS[i].name);
  pl->setDefault(0);
  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast"); co->setRange(0.5, 2); co->setDisplayRange(0.5, 2); co->setDefault(1.1);
  DoubleParamDescriptor *of = desc.defineDoubleParam("offset");
  of->setLabels("Offset", "Offset", "Offset"); of->setRange(-0.5, 0.5); of->setDisplayRange(-0.5, 0.5); of->setDefault(0.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ThermalFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ThermalPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ThermalFactory p("org.purzos.thermal", 1, 0); ids.push_back(&p);
}
} }
