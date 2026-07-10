// Kaleidoscope — mirror-wedge symmetry. Each output pixel is converted to polar
// coordinates around the centre, its angle folded into a single wedge with
// mirroring (a triangle wave), then mapped back to cartesian so the frame
// repeats as `segments` mirrored slices. `angle` spins the wedge, `zoom` scales
// the sampled radius.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (no time term).

#include "../common/purzfx.hpp"

using namespace purz;

class KaleidoscopeProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float cx = 0, cy = 0, angle = 0.f, zoom = 1.f, mix = 1.f;
  int segments = 6;
  explicit KaleidoscopeProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int seg = segments < 1 ? 1 : segments;
    const float wedge = (2.f * 3.14159265358979f) / seg;
    const float invZ = zoom != 0 ? 1.f / zoom : 1.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float r = std::sqrt(dx * dx + dy * dy);
        float th = std::atan2(dy, dx) + angle;
        // fold angle into [0,wedge] with mirroring (triangle wave)
        float f = wrapf(th, 2.f * wedge);
        if (f > wedge) f = 2.f * wedge - f;
        float sxf = cx + std::cos(f) * r * invZ - 0.5f;
        float syf = cy + std::sin(f) * r * invZ - 0.5f;
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class KaleidoscopePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_segments = nullptr;
  OFX::DoubleParam *_angle = nullptr, *_zoom = nullptr, *_mix = nullptr;
public:
  explicit KaleidoscopePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _segments = fetchIntParam("segments"); _angle = fetchDoubleParam("angle");
    _zoom = fetchDoubleParam("zoom"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int segments; double angle, zoom, mix;
    _segments->getValueAtTime(args.time, segments);
    _angle->getValueAtTime(args.time, angle);
    _zoom->getValueAtTime(args.time, zoom);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    KaleidoscopeProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    proc.segments = std::max(1, segments);
    proc.angle = (float)angle; proc.zoom = (float)zoom; proc.mix = (float)mix;

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

mDeclarePluginFactory(KaleidoscopeFactory, {}, {});
using namespace OFX;
void KaleidoscopeFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Kaleidoscope", false); }
void KaleidoscopeFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  IntParamDescriptor *sg = desc.defineIntParam("segments");
  sg->setLabels("Segments", "Segments", "Segments"); sg->setRange(2, 24); sg->setDisplayRange(2, 24); sg->setDefault(6);
  DoubleParamDescriptor *an = desc.defineDoubleParam("angle");
  an->setLabels("Angle", "Angle", "Angle"); an->setRange(-3.15, 3.15); an->setDisplayRange(-3.15, 3.15); an->setDefault(0.0);
  DoubleParamDescriptor *zo = desc.defineDoubleParam("zoom");
  zo->setLabels("Zoom", "Zoom", "Zoom"); zo->setRange(0.3, 3); zo->setDisplayRange(0.3, 3); zo->setDefault(1.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *KaleidoscopeFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new KaleidoscopePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static KaleidoscopeFactory p("org.purzos.kaleidoscope", 1, 0); ids.push_back(&p);
}
} }
