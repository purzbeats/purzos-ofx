// Bulge — localised pinch / punch. Within a circular region of `radius` the
// sampling coordinate is pulled toward or pushed away from the centre by a
// factor that eases smoothly to nothing at the region edge, so a lens-like
// bump (positive `amount`) or dimple (negative) sits over one spot and the rest
// of the frame is untouched.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (no time term).

#include "../common/purzfx.hpp"

using namespace purz;

class BulgeProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float cx = 0, cy = 0, radiusPx = 100, amount = 0.5f, mix = 1.f;
  explicit BulgeProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invR = radiusPx > 0 ? 1.f / radiusPx : 0.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float d = std::sqrt(dx * dx + dy * dy) * invR;   // 0 at centre, 1 at edge
        float sxf, syf;
        if (d < 1.f) {
          float t = 1.f - d;
          float factor = 1.f - amount * t * t;           // eases to 1 at the edge
          sxf = cx + dx * factor - 0.5f;
          syf = cy + dy * factor - 0.5f;
        } else {
          sxf = x + 0.5f - 0.5f;                          // outside radius: unchanged
          syf = y + 0.5f - 0.5f;
        }
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class BulgePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_radius = nullptr, *_cx = nullptr, *_cy = nullptr, *_mix = nullptr;
public:
  explicit BulgePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _radius = fetchDoubleParam("radius");
    _cx = fetchDoubleParam("centerX"); _cy = fetchDoubleParam("centerY");
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

    double amount, radius, ncx, ncy, mix;
    _amount->getValueAtTime(args.time, amount);
    _radius->getValueAtTime(args.time, radius);
    _cx->getValueAtTime(args.time, ncx);
    _cy->getValueAtTime(args.time, ncy);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    const float W = (float)(b.x2 - b.x1), H = (float)(b.y2 - b.y1);
    BulgeProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.cx = b.x1 + (float)ncx * W; proc.cy = b.y1 + (float)ncy * H;
    proc.radiusPx = (float)(radius * W * 0.5);           // radius is 0..1.5 of half-width
    proc.amount = (float)amount; proc.mix = (float)mix;

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

mDeclarePluginFactory(BulgeFactory, {}, {});
using namespace OFX;
void BulgeFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Bulge", false); }
void BulgeFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(-1, 1); am->setDisplayRange(-1, 1); am->setDefault(0.5);
  am->setHint("Positive = bulge out, negative = pinch in");
  DoubleParamDescriptor *ra = desc.defineDoubleParam("radius");
  ra->setLabels("Radius", "Radius", "Radius"); ra->setRange(0.05, 1.5); ra->setDisplayRange(0.05, 1.5); ra->setDefault(0.6);
  DoubleParamDescriptor *cx = desc.defineDoubleParam("centerX");
  cx->setLabels("Center X", "Center X", "Center X"); cx->setRange(0, 1); cx->setDisplayRange(0, 1); cx->setDefault(0.5);
  DoubleParamDescriptor *cy = desc.defineDoubleParam("centerY");
  cy->setLabels("Center Y", "Center Y", "Center Y"); cy->setRange(0, 1); cy->setDisplayRange(0, 1); cy->setDefault(0.5);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *BulgeFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new BulgePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static BulgeFactory p("org.purzos.bulge", 1, 0); ids.push_back(&p);
}
} }
