// Gradient Map — remap luminance through a three-stop gradient (shadow / mid /
// highlight ink). Ships a set of graded presets (Vaporwave, Sunset, Matrix,
// Ice, Fire, Mono); pick "Custom" and the three colour wells take over. The
// classic Photoshop gradient-map look for cheap cinematic colour.
//
// Strictly per-pixel colour map — luma in, gradient out. Deterministic. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
struct Ramp { const char *name; float lo[3], mid[3], hi[3]; }; // sRGB 0..255
static const Ramp PRESETS[] = {
  {"Vaporwave", {20, 10, 50},  {200, 70, 180}, {120, 240, 245}},
  {"Sunset",    {30, 10, 40},  {220, 90, 60},  {255, 225, 120}},
  {"Matrix",    {0, 8, 0},     {20, 130, 40},  {170, 255, 150}},
  {"Ice",       {5, 15, 40},   {60, 150, 210}, {230, 245, 255}},
  {"Fire",      {10, 0, 0},    {200, 50, 10},  {255, 230, 120}},
  {"Mono",      {0, 0, 0},     {128, 128, 128},{255, 255, 255}},
};
static const int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);
// choice index N_PRESETS == "Custom" -> low/mid/high RGB params are used.
} // namespace

class GradientMapProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float lo[3] = {0,0,0}, mid[3] = {0.5f,0.5f,0.5f}, hi[3] = {1,1,1};
  float contrast = 1.f, mix = 1.f;
  explicit GradientMapProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
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
        float L = clamp01((luma(c[0], c[1], c[2]) - 0.5f) * contrast + 0.5f);
        float o[3];
        if (L < 0.5f) { float t = L * 2.f; for (int k = 0; k < 3; k++) o[k] = lerp(lo[k], mid[k], t); }
        else          { float t = (L - 0.5f) * 2.f; for (int k = 0; k < 3; k++) o[k] = lerp(mid[k], hi[k], t); }
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class GradientMapPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_preset = nullptr;
  OFX::RGBParam *_low = nullptr, *_mid = nullptr, *_high = nullptr;
  OFX::DoubleParam *_contrast = nullptr, *_mix = nullptr;
public:
  explicit GradientMapPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _preset = fetchChoiceParam("preset");
    _low = fetchRGBParam("low"); _mid = fetchRGBParam("mid"); _high = fetchRGBParam("high");
    _contrast = fetchDoubleParam("contrast"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int preset; double contrast, mix;
    _preset->getValueAtTime(args.time, preset);
    _contrast->getValueAtTime(args.time, contrast);
    _mix->getValueAtTime(args.time, mix);

    GradientMapProcessor proc(*this);
    if (preset < N_PRESETS) {
      for (int k = 0; k < 3; k++) {
        proc.lo[k]  = PRESETS[preset].lo[k]  / 255.f;
        proc.mid[k] = PRESETS[preset].mid[k] / 255.f;
        proc.hi[k]  = PRESETS[preset].hi[k]  / 255.f;
      }
    } else {
      double r, g, b;
      _low->getValueAtTime(args.time, r, g, b);  proc.lo[0] = (float)r;  proc.lo[1] = (float)g;  proc.lo[2] = (float)b;
      _mid->getValueAtTime(args.time, r, g, b);  proc.mid[0] = (float)r; proc.mid[1] = (float)g; proc.mid[2] = (float)b;
      _high->getValueAtTime(args.time, r, g, b); proc.hi[0] = (float)r;  proc.hi[1] = (float)g;  proc.hi[2] = (float)b;
    }
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.contrast = (float)contrast; proc.mix = (float)mix;

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

mDeclarePluginFactory(GradientMapFactory, {}, {});
using namespace OFX;
void GradientMapFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Gradient Map", true); }
void GradientMapFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  ChoiceParamDescriptor *pr = desc.defineChoiceParam("preset");
  pr->setLabels("Preset", "Preset", "Preset");
  for (int i = 0; i < N_PRESETS; i++) pr->appendOption(PRESETS[i].name);
  pr->appendOption("Custom");
  pr->setDefault(0);
  RGBParamDescriptor *lo = desc.defineRGBParam("low");
  lo->setLabels("Low", "Low", "Low"); lo->setDefault(0.08, 0.04, 0.20); lo->setHint("Shadow ink (Custom preset)");
  RGBParamDescriptor *md = desc.defineRGBParam("mid");
  md->setLabels("Mid", "Mid", "Mid"); md->setDefault(0.78, 0.27, 0.70); md->setHint("Midtone ink (Custom preset)");
  RGBParamDescriptor *hi = desc.defineRGBParam("high");
  hi->setLabels("High", "High", "High"); hi->setDefault(0.47, 0.94, 0.96); hi->setHint("Highlight ink (Custom preset)");
  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast"); co->setRange(0.25, 3); co->setDisplayRange(0.5, 2); co->setDefault(1.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *GradientMapFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new GradientMapPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static GradientMapFactory p("org.purzos.gradientMap", 1, 0); ids.push_back(&p);
}
} }
