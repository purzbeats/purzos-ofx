// Crosshatch — pen-and-ink shading driven by luma. Darkness picks how many sets
// of parallel diagonal pen strokes are laid down: light areas get one loose
// hatch, the darkest areas get all of them crossing at different angles, exactly
// like building up tone with a pen. Ink where any active stroke lands, paper
// everywhere else.
//
// Strictly per-pixel — a stroke is a function of the pixel's own coordinate and
// its luma, no neighbour reads — so tiles are ON. Deterministic (no time).

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
static const float PI = 3.14159265358979323846f;
} // namespace

class CrosshatchProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, spacing = 8, levels = 4;
  float angle = 0.7f, contrast = 1.1f, mix = 1.f;
  float ink[3] = {0.1f, 0.08f, 0.06f}, paper[3] = {0.96f, 0.94f, 0.86f};
  explicit CrosshatchProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float off[5] = {0.f, PI * 0.5f, PI * 0.25f, -PI * 0.25f, PI * 0.375f};
    const float sp = (float)std::max(1, spacing);
    const float band = clampv(sp * 0.14f, 1.0f, sp);   // pen line thickness (px)
    const int nlev = clampv(levels, 2, 5);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float dk = 1.f - luma(c[0], c[1], c[2]);
        dk = clamp01((dk - 0.5f) * contrast + 0.5f);
        const float X = (float)(x - s.b.x1), Y = (float)ty;
        bool hit = false;
        for (int k = 0; k < nlev; k++) {
          if (dk > (float)k / nlev) {
            const float A = angle + off[k];
            const float rc = std::cos(A) * X + std::sin(A) * Y;
            if (wrapf(rc, sp) < band) { hit = true; break; }
          }
        }
        float o[3];
        for (int j = 0; j < 3; j++) o[j] = hit ? ink[j] : paper[j];
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class CrosshatchPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_spacing = nullptr, *_levels = nullptr;
  OFX::DoubleParam *_angle = nullptr, *_contrast = nullptr, *_mix = nullptr;
  OFX::RGBParam *_ink = nullptr, *_paper = nullptr;
public:
  explicit CrosshatchPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _spacing = fetchIntParam("spacing"); _levels = fetchIntParam("levels");
    _angle = fetchDoubleParam("angle"); _contrast = fetchDoubleParam("contrast");
    _ink = fetchRGBParam("ink"); _paper = fetchRGBParam("paper");
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

    int spacing, levels; double angle, contrast, mix;
    _spacing->getValueAtTime(args.time, spacing);
    spacing = std::max(1, (int)std::lround(spacing * args.renderScale.x));
    _levels->getValueAtTime(args.time, levels);
    _angle->getValueAtTime(args.time, angle);
    _contrast->getValueAtTime(args.time, contrast);
    _mix->getValueAtTime(args.time, mix);
    double ir, ig, ib, pr, pg, pb;
    _ink->getValueAtTime(args.time, ir, ig, ib);
    _paper->getValueAtTime(args.time, pr, pg, pb);

    OfxRectI b = src->getBounds();
    CrosshatchProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.H = b.y2 - b.y1; proc.spacing = spacing; proc.levels = levels;
    proc.angle = (float)angle; proc.contrast = (float)contrast; proc.mix = (float)mix;
    proc.ink[0] = (float)ir; proc.ink[1] = (float)ig; proc.ink[2] = (float)ib;
    proc.paper[0] = (float)pr; proc.paper[1] = (float)pg; proc.paper[2] = (float)pb;

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

mDeclarePluginFactory(CrosshatchFactory, {}, {});
using namespace OFX;
void CrosshatchFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Crosshatch", true); }
void CrosshatchFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  IntParamDescriptor *sp = desc.defineIntParam("spacing");
  sp->setLabels("Spacing", "Spacing", "Spacing");
  sp->setRange(4, 24); sp->setDisplayRange(4, 24); sp->setDefault(8);
  IntParamDescriptor *lv = desc.defineIntParam("levels");
  lv->setLabels("Levels", "Levels", "Levels");
  lv->setRange(2, 5); lv->setDisplayRange(2, 5); lv->setDefault(4);
  DoubleParamDescriptor *an = desc.defineDoubleParam("angle");
  an->setLabels("Angle", "Angle", "Angle");
  an->setRange(-PI, PI); an->setDisplayRange(-PI, PI); an->setDefault(0.7);
  RGBParamDescriptor *ik = desc.defineRGBParam("ink");
  ik->setLabels("Ink", "Ink", "Ink"); ik->setDefault(0.1, 0.08, 0.06);
  RGBParamDescriptor *pp = desc.defineRGBParam("paper");
  pp->setLabels("Paper", "Paper", "Paper"); pp->setDefault(0.96, 0.94, 0.86);
  DoubleParamDescriptor *ct = desc.defineDoubleParam("contrast");
  ct->setLabels("Contrast", "Contrast", "Contrast");
  ct->setRange(0.5, 2.5); ct->setDisplayRange(0.5, 2.5); ct->setDefault(1.1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *CrosshatchFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new CrosshatchPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static CrosshatchFactory p("org.purzos.crosshatch", 1, 0); ids.push_back(&p);
}
} }
