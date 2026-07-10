// Edge Enhance — the crunchy oversharpening a VCR (and cheap TV "sharpness"
// knobs) bake in: an unsharp mask that throws bright/dark ringing halos around
// every edge. A 5-tap cross blur gives the low-pass, `amount` sets the boost,
// and `ring` adds a second-order overshoot for the tell-tale halo.
//
// Reads neighbour taps at +/- radius -> tiles OFF. Deterministic (no time term).

#include "../common/purzfx.hpp"

using namespace purz;

class EdgeEnhanceProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int radius = 2;
  float amount = 1.2f, ring = 0.4f, mix = 1.f;
  explicit EdgeEnhanceProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int r = std::max(1, radius);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float l[4], rt[4], u[4], d[4];
        s.at(x - r, y, l); s.at(x + r, y, rt); s.at(x, y - r, u); s.at(x, y + r, d);
        float o[4];
        for (int k = 0; k < 3; k++) {
          float blur = (c[k] + l[k] + rt[k] + u[k] + d[k]) * 0.2f;
          float hp = c[k] - blur;                         // high-pass
          float v = c[k] + amount * hp;                   // unsharp boost
          v += amount * ring * hp * std::fabs(hp) * 4.f;  // second-order overshoot halo
          o[k] = clamp01(v);
        }
        o[3] = c[3];
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(c[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class EdgeEnhancePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_ring = nullptr, *_mix = nullptr;
  OFX::IntParam *_radius = nullptr;
public:
  explicit EdgeEnhancePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _radius = fetchIntParam("radius");
    _ring = fetchDoubleParam("ring"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double amount, ring, mix; int radius;
    _amount->getValueAtTime(args.time, amount);
    _radius->getValueAtTime(args.time, radius);
    _ring->getValueAtTime(args.time, ring);
    _mix->getValueAtTime(args.time, mix);

    EdgeEnhanceProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.amount = (float)amount; proc.ring = (float)ring; proc.mix = (float)mix;
    proc.radius = std::max(1, (int)std::floor(radius * args.renderScale.x + 0.5));

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

mDeclarePluginFactory(EdgeEnhanceFactory, {}, {});
using namespace OFX;
void EdgeEnhanceFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Edge Enhance", false); }
void EdgeEnhanceFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 3); am->setDisplayRange(0, 3); am->setDefault(1.2);
  am->setHint("Sharpening boost");
  IntParamDescriptor *ra = desc.defineIntParam("radius");
  ra->setLabels("Radius", "Radius", "Radius"); ra->setRange(1, 8); ra->setDisplayRange(1, 8); ra->setDefault(2);
  ra->setHint("Blur tap distance in pixels");
  DoubleParamDescriptor *ri = desc.defineDoubleParam("ring");
  ri->setLabels("Ring", "Ring", "Ring"); ri->setRange(0, 1); ri->setDisplayRange(0, 1); ri->setDefault(0.4);
  ri->setHint("Overshoot halo strength");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *EdgeEnhanceFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new EdgeEnhancePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static EdgeEnhanceFactory p("org.purzos.edgeEnhance", 1, 0); ids.push_back(&p);
}
} }
