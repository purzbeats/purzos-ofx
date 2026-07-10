// Posterize — bit-depth crush + retro palette snap. Per channel the signal is
// quantised to N levels (optionally gamma-weighted so the steps land where the
// eye notices), then optionally snapped to a fixed vintage hardware palette
// (CGA/EGA/NES/PICO-8/C64/Game Boy) with a touch of ordered dither to break the
// banding the way a real 4-bit framebuffer would.
//
// Strictly per-pixel + a fixed Bayer cell -> tiles are safe. Deterministic.

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
struct Pal { const char *name; int n; float rgb[16][3]; };
static const Pal PALS[] = {
  {"CGA",     4,  {{0,0,0},{85,255,255},{255,85,255},{255,255,255}}},
  {"EGA",     16, {{0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},
                   {170,85,0},{170,170,170},{85,85,85},{85,85,255},{85,255,85},
                   {85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}}},
  {"NES",     8,  {{0,0,0},{124,124,124},{188,188,188},{252,252,252},{0,88,248},
                   {248,56,0},{88,216,84},{248,152,248}}},
  {"PICO-8",  16, {{0,0,0},{29,43,83},{126,37,83},{0,135,81},{171,82,54},{95,87,79},
                   {194,195,199},{255,241,232},{255,0,77},{255,163,0},{255,236,39},
                   {0,228,54},{41,173,255},{131,118,156},{255,119,168},{255,204,170}}},
  {"C64",     8,  {{0,0,0},{255,255,255},{136,57,50},{103,182,189},{139,63,150},
                   {85,160,73},{64,49,141},{191,206,114}}},
  {"Game Boy",4,  {{15,56,15},{48,98,48},{139,172,15},{155,188,15}}},
};
static const int N_PALS = sizeof(PALS) / sizeof(PALS[0]);

static inline float bayer4(int x, int y) {
  static const int B[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
  return (B[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
}
} // namespace

class PosterizeProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int levels = 6, pal = -1, H = 0;
  float gamma = 1.f, dither = 0.f, mix = 1.f;
  explicit PosterizeProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invg = 1.f / std::max(0.01f, gamma);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);         // top-down for stable dither
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const float d = dither * bayer4(x - s.b.x1, ty);
        float o[3];
        for (int k = 0; k < 3; k++) {
          float v = std::pow(clamp01(c[k]), gamma);
          v = std::floor(v * (levels - 1) + 0.5f + d) / (levels - 1);
          o[k] = std::pow(clamp01(v), invg);
        }
        if (pal >= 0) {                              // snap to nearest palette ink
          const Pal &P = PALS[pal];
          int best = 0; float bd = 1e9f;
          for (int i = 0; i < P.n; i++) {
            float dr = o[0] - P.rgb[i][0] / 255.f, dg = o[1] - P.rgb[i][1] / 255.f,
                  db = o[2] - P.rgb[i][2] / 255.f;
            float dd = dr * dr * 1.2f + dg * dg + db * db * 0.8f;
            if (dd < bd) { bd = dd; best = i; }
          }
          o[0] = P.rgb[best][0] / 255.f; o[1] = P.rgb[best][1] / 255.f; o[2] = P.rgb[best][2] / 255.f;
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

class PosterizePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_levels = nullptr;
  OFX::ChoiceParam *_pal = nullptr;
  OFX::DoubleParam *_gamma = nullptr, *_dither = nullptr, *_mix = nullptr;
public:
  explicit PosterizePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _levels = fetchIntParam("levels"); _pal = fetchChoiceParam("palette");
    _gamma = fetchDoubleParam("gamma"); _dither = fetchDoubleParam("dither");
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

    int levels, pal; double gamma, dither, mix;
    _levels->getValueAtTime(args.time, levels);
    _pal->getValueAtTime(args.time, pal);
    _gamma->getValueAtTime(args.time, gamma);
    _dither->getValueAtTime(args.time, dither);
    _mix->getValueAtTime(args.time, mix);

    PosterizeProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.levels = std::max(2, levels); proc.pal = pal - 1; // 0 == "Off"
    proc.gamma = (float)gamma; proc.dither = (float)dither; proc.mix = (float)mix;
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

mDeclarePluginFactory(PosterizeFactory, {}, {});
using namespace OFX;
void PosterizeFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Posterize", true); }
void PosterizeFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  IntParamDescriptor *lv = desc.defineIntParam("levels");
  lv->setLabels("Levels", "Levels", "Levels");
  lv->setRange(2, 32); lv->setDisplayRange(2, 16); lv->setDefault(5);
  ChoiceParamDescriptor *pl = desc.defineChoiceParam("palette");
  pl->setLabels("Palette", "Palette", "Palette");
  pl->appendOption("Off"); for (int i = 0; i < N_PALS; i++) pl->appendOption(PALS[i].name);
  pl->setDefault(0);
  DoubleParamDescriptor *ga = desc.defineDoubleParam("gamma");
  ga->setLabels("Gamma", "Gamma", "Gamma"); ga->setRange(0.2, 3); ga->setDisplayRange(0.4, 2.2); ga->setDefault(1.0);
  DoubleParamDescriptor *di = desc.defineDoubleParam("dither");
  di->setLabels("Dither", "Dither", "Dither"); di->setRange(0, 1); di->setDisplayRange(0, 1); di->setDefault(0.3);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *PosterizeFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new PosterizePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static PosterizeFactory p("org.purzos.posterize", 1, 0); ids.push_back(&p);
}
} }
