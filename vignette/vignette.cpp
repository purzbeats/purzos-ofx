// Vignette — corner darkening with an edge tint and a dusting of film grain.
// Distance from frame centre drives a smooth falloff: the middle stays clean
// while the corners fade toward a tint colour (black by default, but tintable
// for a warm/cool edge). Deterministic per-pixel grain keeps it from banding.
//
// Strictly per-pixel (radial term + hashed grain on top-down coords) -> tiles
// are safe. Deterministic: grain is a pure hash of (x, ty, frame).

#include "../common/purzfx.hpp"

using namespace purz;

class VignetteProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int frame = 0, H = 0;
  float cx = 0, cy = 0;
  float amount = 0.5f, radius = 0.9f, softness = 0.5f, grain = 0.03f, mix = 1.f;
  float tint[3] = {0.f, 0.f, 0.f};
  explicit VignetteProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float halfDiag = 0.5f * std::sqrt((float)s.W * s.W + (float)s.H * s.H);
    const float denom = std::max(1e-4f, halfDiag * radius);
    const float e0 = 1.f - softness;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                 // top-down (grain)
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        const float r = std::sqrt(dx * dx + dy * dy) / denom;
        const float v = 1.f - amount * smoothstep(e0, 1.f, r);
        const float g = (hash3(x - s.b.x1, ty, frame) - 0.5f) * grain;
        float o[3];
        for (int k = 0; k < 3; k++) o[k] = lerp(tint[k], c[k], v) + g;
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class VignettePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_radius = nullptr, *_softness = nullptr,
                   *_grain = nullptr, *_mix = nullptr;
  OFX::RGBParam *_tint = nullptr;
public:
  explicit VignettePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _radius = fetchDoubleParam("radius");
    _softness = fetchDoubleParam("softness"); _tint = fetchRGBParam("tint");
    _grain = fetchDoubleParam("grain"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double amount, radius, softness, grain, mix, tr, tg, tb;
    _amount->getValueAtTime(args.time, amount);
    _radius->getValueAtTime(args.time, radius);
    _softness->getValueAtTime(args.time, softness);
    _tint->getValueAtTime(args.time, tr, tg, tb);
    _grain->getValueAtTime(args.time, grain);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    VignetteProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.amount = (float)amount; proc.radius = (float)radius; proc.softness = (float)softness;
    proc.grain = (float)grain; proc.mix = (float)mix;
    proc.tint[0] = (float)tr; proc.tint[1] = (float)tg; proc.tint[2] = (float)tb;
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    proc.frame = (int)std::floor(args.time);
    proc.H = b.y2 - b.y1;

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

mDeclarePluginFactory(VignetteFactory, {}, {});
using namespace OFX;
void VignetteFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Vignette", true); }
void VignetteFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.5);
  DoubleParamDescriptor *ra = desc.defineDoubleParam("radius");
  ra->setLabels("Radius", "Radius", "Radius"); ra->setRange(0.2, 1.5); ra->setDisplayRange(0.2, 1.5); ra->setDefault(0.9);
  DoubleParamDescriptor *so = desc.defineDoubleParam("softness");
  so->setLabels("Softness", "Softness", "Softness"); so->setRange(0.05, 1); so->setDisplayRange(0.05, 1); so->setDefault(0.5);
  RGBParamDescriptor *ti = desc.defineRGBParam("tint");
  ti->setLabels("Edge tint", "Edge tint", "Edge tint"); ti->setDefault(0.0, 0.0, 0.0);
  DoubleParamDescriptor *gr = desc.defineDoubleParam("grain");
  gr->setLabels("Grain", "Grain", "Grain"); gr->setRange(0, 0.3); gr->setDisplayRange(0, 0.3); gr->setDefault(0.03);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *VignetteFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new VignettePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static VignetteFactory p("org.purzos.vignette", 1, 0); ids.push_back(&p);
}
} }
