// CRT Screen — an all-in-one tube emulation. The sampling coordinate is bent
// through a barrel distortion (glass curvature), read back with a bilinear
// sample, and anything that curves past the screen edge falls to black so the
// picture sits inside a rounded bulge. Onto that we lay scanlines, an RGB triad
// mask, a radial vignette and a brightness lift.
//
// Warps the coordinate + reads neighbours -> needs the whole frame, tiles OFF.
// Deterministic: time never enters the maths.

#include "../common/purzfx.hpp"

using namespace purz;

static const float TAU = 6.28318530718f;

class CrtScreenProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int scanPeriod = 2, H = 0;
  float cx = 0, cy = 0, halfW = 1, halfH = 1;
  float curve = 0.15f, scan = 0.4f, mask = 0.3f, vignette = 0.4f, bright = 1.2f, mix = 1.f;
  explicit CrtScreenProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float lh = (float)std::max(1, scanPeriod);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                 // top-down
      const float sc = 0.5f + 0.5f * std::cos(TAU * ty / lh);
      const float scanF = 1.f - scan * (1.f - sc);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const float nx = (x + 0.5f - cx) / halfW;
        const float ny = (y + 0.5f - cy) / halfH;
        const float r2 = nx * nx + ny * ny;
        const float bx = nx * (1.f + curve * r2);
        const float by = ny * (1.f + curve * r2);
        float o[4];
        if (std::fabs(bx) > 1.f || std::fabs(by) > 1.f) {  // curved off the tube
          o[0] = o[1] = o[2] = 0.f; o[3] = c[3];
        } else {
          const float sx = cx + bx * halfW - 0.5f;
          const float sy = cy + by * halfH - 0.5f;
          s.bilin(sx, sy, o);
          const int ph = (((x - s.b.x1) % 3) + 3) % 3;
          const float vig = clamp01(1.f - vignette * r2);
          for (int k = 0; k < 3; k++) {
            const float tm = (ph == k) ? 1.f : (1.f - mask);
            o[k] = o[k] * scanF * tm * vig * bright;
          }
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

class CrtScreenPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_curve = nullptr, *_scan = nullptr, *_mask = nullptr,
                   *_vignette = nullptr, *_bright = nullptr, *_mix = nullptr;
public:
  explicit CrtScreenPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _curve = fetchDoubleParam("curve"); _scan = fetchDoubleParam("scan");
    _mask = fetchDoubleParam("mask"); _vignette = fetchDoubleParam("vignette");
    _bright = fetchDoubleParam("bright"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double curve, scan, mask, vignette, bright, mix;
    _curve->getValueAtTime(args.time, curve);
    _scan->getValueAtTime(args.time, scan);
    _mask->getValueAtTime(args.time, mask);
    _vignette->getValueAtTime(args.time, vignette);
    _bright->getValueAtTime(args.time, bright);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    CrtScreenProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.H = b.y2 - b.y1;
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    proc.halfW = 0.5f * (b.x2 - b.x1); proc.halfH = 0.5f * (b.y2 - b.y1);
    proc.scanPeriod = std::max(1, (int)std::lround(2 * args.renderScale.x));
    proc.curve = (float)curve; proc.scan = (float)scan; proc.mask = (float)mask;
    proc.vignette = (float)vignette; proc.bright = (float)bright; proc.mix = (float)mix;

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

mDeclarePluginFactory(CrtScreenFactory, {}, {});
using namespace OFX;
void CrtScreenFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "CRT Screen", false); }
void CrtScreenFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *cu = desc.defineDoubleParam("curve");
  cu->setLabels("Curvature", "Curvature", "Curvature"); cu->setRange(0, 0.5); cu->setDisplayRange(0, 0.5); cu->setDefault(0.15);
  DoubleParamDescriptor *sc = desc.defineDoubleParam("scan");
  sc->setLabels("Scanlines", "Scanlines", "Scanlines"); sc->setRange(0, 1); sc->setDisplayRange(0, 1); sc->setDefault(0.4);
  DoubleParamDescriptor *ma = desc.defineDoubleParam("mask");
  ma->setLabels("Mask", "Mask", "Mask"); ma->setRange(0, 1); ma->setDisplayRange(0, 1); ma->setDefault(0.3);
  DoubleParamDescriptor *vi = desc.defineDoubleParam("vignette");
  vi->setLabels("Vignette", "Vignette", "Vignette"); vi->setRange(0, 1); vi->setDisplayRange(0, 1); vi->setDefault(0.4);
  DoubleParamDescriptor *br = desc.defineDoubleParam("bright");
  br->setLabels("Brightness", "Brightness", "Brightness"); br->setRange(0.8, 2); br->setDisplayRange(0.8, 2); br->setDefault(1.2);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *CrtScreenFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new CrtScreenPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static CrtScreenFactory p("org.purzos.crtScreen", 1, 0); ids.push_back(&p);
}
} }
