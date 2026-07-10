// Glass — frosted-glass refraction. Each output pixel is nudged by the local
// gradient of a value-noise field (two octaves), so the raster warps in soft
// irregular cells the way light bends through pebbled glass. `scale` sets the
// cell size, `detail` weights the finer octave and `speed` drifts the pattern
// over time.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (value noise + integer frame index).

#include "../common/purzfx.hpp"

using namespace purz;

class GlassProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float x1 = 0, y1 = 0, amount = 16.f, scale = 24.f, drift = 0.f, detail = 2.f, mix = 1.f;
  explicit GlassProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float ns = scale > 0.5f ? scale : 0.5f;
    const float ns2 = ns * 0.5f;
    const float w2 = detail > 0 ? 1.f / detail : 0.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float px = (x - x1), py = (y - y1);
        // octave 1: finite-difference gradient of value noise
        float d1x = vnoise(px / ns + drift, py / ns, 11) - vnoise((px + 2) / ns + drift, py / ns, 11);
        float d1y = vnoise(px / ns + drift, py / ns, 23) - vnoise(px / ns + drift, (py + 2) / ns, 23);
        // octave 2 at half the cell size, weighted by 1/detail
        float d2x = vnoise(px / ns2 + drift, py / ns2, 11) - vnoise((px + 2) / ns2 + drift, py / ns2, 11);
        float d2y = vnoise(px / ns2 + drift, py / ns2, 23) - vnoise(px / ns2 + drift, (py + 2) / ns2, 23);
        float dx = amount * (d1x + w2 * d2x);
        float dy = amount * (d1y + w2 * d2y);
        float sxf = (x + 0.5f) + dx - 0.5f;
        float syf = (y + 0.5f) + dy - 0.5f;
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class GlassPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_scale = nullptr, *_speed = nullptr,
                   *_detail = nullptr, *_mix = nullptr;
public:
  explicit GlassPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _scale = fetchDoubleParam("scale");
    _speed = fetchDoubleParam("speed"); _detail = fetchDoubleParam("detail");
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

    double amount, scale, speed, detail, mix;
    _amount->getValueAtTime(args.time, amount);
    _scale->getValueAtTime(args.time, scale);
    _speed->getValueAtTime(args.time, speed);
    _detail->getValueAtTime(args.time, detail);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    const int frame = (int)std::floor(args.time);
    GlassProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.x1 = (float)b.x1; proc.y1 = (float)b.y1;
    proc.amount = (float)(amount * args.renderScale.x);   // px displacement (renderScale-scaled)
    proc.scale = (float)scale;
    proc.drift = (float)(speed * frame * 0.1);
    proc.detail = (float)detail; proc.mix = (float)mix;

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

mDeclarePluginFactory(GlassFactory, {}, {});
using namespace OFX;
void GlassFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Glass", false); }
void GlassFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 80); am->setDisplayRange(0, 80); am->setDefault(16.0);
  am->setHint("Displacement in pixels");
  DoubleParamDescriptor *sc = desc.defineDoubleParam("scale");
  sc->setLabels("Scale", "Scale", "Scale"); sc->setRange(2, 80); sc->setDisplayRange(2, 80); sc->setDefault(24.0);
  sc->setHint("Noise cell size");
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-4, 4); sp->setDisplayRange(-4, 4); sp->setDefault(0.2);
  sp->setHint("Time-driven drift (sampled at the integer frame)");
  DoubleParamDescriptor *dt = desc.defineDoubleParam("detail");
  dt->setLabels("Detail", "Detail", "Detail"); dt->setRange(1, 3); dt->setDisplayRange(1, 3); dt->setDefault(2.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *GlassFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new GlassPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static GlassFactory p("org.purzos.glass", 1, 0); ids.push_back(&p);
}
} }
