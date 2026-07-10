// Night Vision — image-intensifier tube look: collapse to luma, gain it up,
// push it through a phosphor tint (green by default), then dirty it with
// deterministic sensor noise, faint scanlines and a radial vignette — the
// classic light-amplified goggle image.
//
// Per-pixel: the only spatial terms are a hashed grain, a scanline on the row
// and a vignette on position. Noise is hash3(x,ty,frame) so it is byte-identical
// on any machine. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

class NightVisionProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0;
  float gain = 1.6f, noise = 0.12f, vignette = 0.5f, scan = 0.3f, mix = 1.f;
  float tint[3] = {0.2f, 1.0f, 0.3f};
  float cx = 0.f, cy = 0.f, invDiag = 0.f;
  explicit NightVisionProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float twoPiOver3 = 6.2831853f / 3.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                    // top-down stable grain
      const float sc = 0.5f + 0.5f * std::cos((float)ty * twoPiOver3);
      const float scanF = 1.f - scan * 0.6f * sc;
      const float dy = (y + 0.5f - cy) * invDiag;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float L = luma(c[0], c[1], c[2]) * gain;
        L += (hash3(x - s.b.x1, ty, frame) - 0.5f) * noise;    // sensor grain
        const float dx = (x + 0.5f - cx) * invDiag;
        const float rn = clamp01(std::sqrt(dx * dx + dy * dy)); // 0 centre .. 1 corner
        const float vigF = 1.f - vignette * rn * rn;
        const float e = clamp01(L) * scanF * vigF;
        float o[3] = { tint[0] * e, tint[1] * e, tint[2] * e };
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class NightVisionPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_gain = nullptr, *_noise = nullptr, *_vignette = nullptr, *_scan = nullptr, *_mix = nullptr;
  OFX::RGBParam *_tint = nullptr;
public:
  explicit NightVisionPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _gain = fetchDoubleParam("gain"); _noise = fetchDoubleParam("noise");
    _vignette = fetchDoubleParam("vignette"); _scan = fetchDoubleParam("scan");
    _tint = fetchRGBParam("tint"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double gain, noise, vignette, scan, mix, tr, tg, tb;
    _gain->getValueAtTime(args.time, gain);
    _noise->getValueAtTime(args.time, noise);
    _vignette->getValueAtTime(args.time, vignette);
    _scan->getValueAtTime(args.time, scan);
    _tint->getValueAtTime(args.time, tr, tg, tb);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    NightVisionProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.H = b.y2 - b.y1; proc.frame = (int)args.time;
    proc.gain = (float)gain; proc.noise = (float)noise;
    proc.vignette = (float)vignette; proc.scan = (float)scan; proc.mix = (float)mix;
    proc.tint[0] = (float)tr; proc.tint[1] = (float)tg; proc.tint[2] = (float)tb;
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    const float hw = 0.5f * (b.x2 - b.x1), hh = 0.5f * (b.y2 - b.y1);
    const float diag = std::sqrt(hw * hw + hh * hh);
    proc.invDiag = diag > 0.f ? 1.f / diag : 0.f;

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

mDeclarePluginFactory(NightVisionFactory, {}, {});
using namespace OFX;
void NightVisionFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Night Vision", true); }
void NightVisionFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *ga = desc.defineDoubleParam("gain");
  ga->setLabels("Gain", "Gain", "Gain"); ga->setRange(0.5, 3); ga->setDisplayRange(0.5, 3); ga->setDefault(1.6);
  DoubleParamDescriptor *no = desc.defineDoubleParam("noise");
  no->setLabels("Noise", "Noise", "Noise"); no->setRange(0, 0.4); no->setDisplayRange(0, 0.4); no->setDefault(0.12);
  DoubleParamDescriptor *vi = desc.defineDoubleParam("vignette");
  vi->setLabels("Vignette", "Vignette", "Vignette"); vi->setRange(0, 1); vi->setDisplayRange(0, 1); vi->setDefault(0.5);
  DoubleParamDescriptor *sc = desc.defineDoubleParam("scan");
  sc->setLabels("Scanlines", "Scanlines", "Scanlines"); sc->setRange(0, 1); sc->setDisplayRange(0, 1); sc->setDefault(0.3);
  RGBParamDescriptor *ti = desc.defineRGBParam("tint");
  ti->setLabels("Tint", "Tint", "Tint"); ti->setDefault(0.2, 1.0, 0.3);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *NightVisionFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new NightVisionPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static NightVisionFactory p("org.purzos.nightVision", 1, 0); ids.push_back(&p);
}
} }
