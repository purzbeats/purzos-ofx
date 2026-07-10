// Twirl — swirl the raster around a centre. Each output pixel rotates the
// sampling coordinate by an angle that falls off with radius, so the middle
// spins hard and the edges sit still. `Spin` adds a time-driven rotation so a
// still image corkscrews over a clip.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (time only enters via the Spin term).

#include "../common/purzfx.hpp"

using namespace purz;

class TwirlProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float cx = 0, cy = 0, radius = 100, angle = 3.f, mix = 1.f;
  explicit TwirlProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invR = radius > 0 ? 1.f / radius : 0.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float r = std::sqrt(dx * dx + dy * dy);
        float t = 1.f - clamp01(r * invR);
        float a = angle * t * t;                 // smooth falloff
        float ca = std::cos(a), sa = std::sin(a);
        float sxf = cx + dx * ca - dy * sa - 0.5f;
        float syf = cy + dx * sa + dy * ca - 0.5f;
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class TwirlPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_radius = nullptr, *_angle = nullptr, *_spin = nullptr, *_mix = nullptr;
public:
  explicit TwirlPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _radius = fetchDoubleParam("radius"); _angle = fetchDoubleParam("angle");
    _spin = fetchDoubleParam("spin"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double radius, angle, spin, mix;
    _radius->getValueAtTime(args.time, radius);
    _angle->getValueAtTime(args.time, angle);
    _spin->getValueAtTime(args.time, spin);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    TwirlProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    proc.radius = (float)(radius * (b.x2 - b.x1) * 0.5); // radius is 0..1 of half-width
    proc.angle = (float)(angle + spin * args.time * 0.10);
    proc.mix = (float)mix;

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

mDeclarePluginFactory(TwirlFactory, {}, {});
using namespace OFX;
void TwirlFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Twirl", false); }
void TwirlFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *ra = desc.defineDoubleParam("radius");
  ra->setLabels("Radius", "Radius", "Radius"); ra->setRange(0.01, 2); ra->setDisplayRange(0.05, 1.5); ra->setDefault(0.9);
  DoubleParamDescriptor *an = desc.defineDoubleParam("angle");
  an->setLabels("Angle", "Angle", "Angle"); an->setRange(-12, 12); an->setDisplayRange(-8, 8); an->setDefault(3.0);
  DoubleParamDescriptor *sp = desc.defineDoubleParam("spin");
  sp->setLabels("Spin", "Spin", "Spin"); sp->setRange(-10, 10); sp->setDisplayRange(-5, 5); sp->setDefault(0.0);
  sp->setHint("Time-driven rotation (radians/sec-ish) on top of Angle");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *TwirlFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new TwirlPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static TwirlFactory p("org.purzos.twirl", 1, 0); ids.push_back(&p);
}
} }
