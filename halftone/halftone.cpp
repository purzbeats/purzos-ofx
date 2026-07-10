// Halftone — rotated dot screens, a port of purzos/tools/effects/halftone. For
// every output pixel we look up the nearest dot centre of a screen rotated by
// `angle`, read the source darkness there, and lay down ink if the pixel falls
// within that dot's grown radius. Newsprint is a single black screen on cream
// paper; CMY Colour multiplies three angled screens (cyan/magenta/yellow) the
// way a real four-colour press builds an image.
//
// A coordinate lookup that reads source points off-pixel (like twirl), so tiles
// are OFF. No randomness, no time — fully deterministic.

#include "../common/purzfx.hpp"

using namespace purz;

namespace {
static const float PI = 3.14159265358979323846f;
static const float D2R = PI / 180.f;
} // namespace

class HalftoneProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int mode = 0;
  float dotf = 6.f, angleRad = 0.f, contrast = 1.1f, mix = 1.f;
  explicit HalftoneProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  // Is this pixel inked by a screen at angle A, sampling channel `chan`
  // (chan < 0 -> mono luma)? px,py are TOP-DOWN centre-space pixel coords.
  template <class PIX, int nComps, int maxv>
  inline bool covered(const Src<PIX, nComps, maxv> &s, float px, float py,
                      float A, int chan) const {
    const float W = (float)s.W, H = (float)s.H;
    const float dx = px - W * 0.5f, dy = py - H * 0.5f;
    const float ca = std::cos(A), sa = std::sin(A);
    const float u = ca * dx + sa * dy, v = -sa * dx + ca * dy;      // rotated grid
    const float ru = std::round(u / dotf) * dotf, rv = std::round(v / dotf) * dotf;
    const float dist = std::sqrt((u - ru) * (u - ru) + (v - rv) * (v - rv));
    const float dcx = ca * ru - sa * rv, dcy = sa * ru + ca * rv;   // back to image
    const float scx = W * 0.5f + dcx, scy = H * 0.5f + dcy;         // top-down centre
    const float ax = s.b.x1 + (scx - 0.5f);
    const float ay = s.b.y1 + (H - 0.5f - scy);                     // top-down -> OFX
    float c[4]; s.bilin(ax, ay, c);
    const float val = (chan < 0) ? luma(c[0], c[1], c[2]) : c[chan];
    float d = 1.f - val;
    d = clamp01((d - 0.5f) * contrast + 0.5f);
    const float rad = std::sqrt(clamp01(d)) * dotf * 0.72f;
    return dist <= rad;
  }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int H = s.H;
    const float paper[3] = {244.f/255.f, 240.f/255.f, 228.f/255.f};
    const float ink[3]   = {22.f/255.f, 20.f/255.f, 15.f/255.f};
    const float cyan[3]  = {0.f, 174.f/255.f, 239.f/255.f};
    const float mag[3]   = {236.f/255.f, 0.f, 140.f/255.f};
    const float yel[3]   = {255.f/255.f, 241.f/255.f, 0.f};
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const float px = (x - s.b.x1) + 0.5f;
        const float py = (H - 1 - (y - s.b.y1)) + 0.5f;
        float src[4]; s.at(x, y, src);
        float o[3];
        if (mode == 0) {                                  // Newsprint (mono)
          const bool hit = covered(s, px, py, angleRad, -1);
          for (int k = 0; k < 3; k++) o[k] = hit ? ink[k] : paper[k];
        } else {                                          // CMY Colour
          float r = 1.f, g = 1.f, b = 1.f;
          if (covered(s, px, py, angleRad + 15.f * D2R, 0)) { r *= cyan[0]; g *= cyan[1]; b *= cyan[2]; }
          if (covered(s, px, py, angleRad + 75.f * D2R, 1)) { r *= mag[0];  g *= mag[1];  b *= mag[2];  }
          if (covered(s, px, py, angleRad + 0.f,        2)) { r *= yel[0];  g *= yel[1];  b *= yel[2];  }
          o[0] = r; o[1] = g; o[2] = b;
        }
        dst[0] = q<PIX, maxv>(lerp(src[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(src[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(src[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(src[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class HalftonePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_mode = nullptr;
  OFX::IntParam *_dot = nullptr, *_angle = nullptr;
  OFX::DoubleParam *_contrast = nullptr, *_mix = nullptr;
public:
  explicit HalftonePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _mode = fetchChoiceParam("mode"); _dot = fetchIntParam("dot");
    _angle = fetchIntParam("angle");
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

    int mode, dot, angle; double contrast, mix;
    _mode->getValueAtTime(args.time, mode);
    _dot->getValueAtTime(args.time, dot);
    dot = std::max(1, (int)std::lround(dot * args.renderScale.x));
    _angle->getValueAtTime(args.time, angle);
    _contrast->getValueAtTime(args.time, contrast);
    _mix->getValueAtTime(args.time, mix);

    HalftoneProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.mode = mode; proc.dotf = (float)dot; proc.angleRad = (float)angle * D2R;
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

mDeclarePluginFactory(HalftoneFactory, {}, {});
using namespace OFX;
void HalftoneFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Halftone", false); }
void HalftoneFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  ChoiceParamDescriptor *mo = desc.defineChoiceParam("mode");
  mo->setLabels("Mode", "Mode", "Mode");
  mo->appendOption("Newsprint"); mo->appendOption("CMY Colour");
  mo->setDefault(0);
  IntParamDescriptor *dt = desc.defineIntParam("dot");
  dt->setLabels("Dot size", "Dot size", "Dot size");
  dt->setRange(3, 16); dt->setDisplayRange(3, 16); dt->setDefault(6);
  IntParamDescriptor *an = desc.defineIntParam("angle");
  an->setLabels("Angle", "Angle", "Angle");
  an->setRange(0, 90); an->setDisplayRange(0, 90); an->setDefault(15);
  DoubleParamDescriptor *ct = desc.defineDoubleParam("contrast");
  ct->setLabels("Contrast", "Contrast", "Contrast");
  ct->setRange(0.5, 2); ct->setDisplayRange(0.5, 2); ct->setDefault(1.1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *HalftoneFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new HalftonePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static HalftoneFactory p("org.purzos.halftone", 1, 0); ids.push_back(&p);
}
} }
