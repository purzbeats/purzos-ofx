// Ripple — radial sine ripple, like a stone dropped in a pond. Each output pixel
// is displaced along the vector from the centre by a sine wave of the radius,
// so concentric rings push the raster in and out. `speed` animates the rings
// over time and `decay` fades them with distance.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (time only enters via the `speed`
// term, sampled at the integer frame index).

#include "../common/purzfx.hpp"

using namespace purz;

class RippleProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float cx = 0, cy = 0, halfW = 1, amp = 0.02f, freq = 12.f, phase = 0.f, decay = 1.f, mix = 1.f;
  explicit RippleProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invH = halfW > 0 ? 1.f / halfW : 0.f;
    const float ampPx = amp * halfW;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float rp = std::sqrt(dx * dx + dy * dy);
        float r = rp * invH;                              // normalised radius
        float inv = rp > 1e-6f ? 1.f / rp : 0.f;
        float ux = dx * inv, uy = dy * inv;               // unit direction
        float disp = ampPx * std::sin(freq * r - phase) * std::exp(-decay * r);
        float sxf = (x + 0.5f) + ux * disp - 0.5f;
        float syf = (y + 0.5f) + uy * disp - 0.5f;
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class RipplePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amp = nullptr, *_freq = nullptr, *_speed = nullptr,
                   *_cx = nullptr, *_cy = nullptr, *_decay = nullptr, *_mix = nullptr;
public:
  explicit RipplePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amp = fetchDoubleParam("amp"); _freq = fetchDoubleParam("freq");
    _speed = fetchDoubleParam("speed"); _cx = fetchDoubleParam("centerX");
    _cy = fetchDoubleParam("centerY"); _decay = fetchDoubleParam("decay");
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

    double amp, freq, speed, ncx, ncy, decay, mix;
    _amp->getValueAtTime(args.time, amp);
    _freq->getValueAtTime(args.time, freq);
    _speed->getValueAtTime(args.time, speed);
    _cx->getValueAtTime(args.time, ncx);
    _cy->getValueAtTime(args.time, ncy);
    _decay->getValueAtTime(args.time, decay);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    const float W = (float)(b.x2 - b.x1), H = (float)(b.y2 - b.y1);
    const int frame = (int)std::floor(args.time);
    RippleProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.cx = b.x1 + (float)ncx * W; proc.cy = b.y1 + (float)ncy * H;
    proc.halfW = (float)(W * 0.5);
    proc.amp = (float)amp; proc.freq = (float)freq;
    proc.phase = (float)(speed * frame * 0.1);
    proc.decay = (float)decay; proc.mix = (float)mix;

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

mDeclarePluginFactory(RippleFactory, {}, {});
using namespace OFX;
void RippleFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Ripple", false); }
void RippleFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *ap = desc.defineDoubleParam("amp");
  ap->setLabels("Amplitude", "Amplitude", "Amplitude"); ap->setRange(0, 0.1); ap->setDisplayRange(0, 0.1); ap->setDefault(0.02);
  DoubleParamDescriptor *fr = desc.defineDoubleParam("freq");
  fr->setLabels("Frequency", "Frequency", "Frequency"); fr->setRange(1, 40); fr->setDisplayRange(1, 40); fr->setDefault(12.0);
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-8, 8); sp->setDisplayRange(-8, 8); sp->setDefault(2.0);
  sp->setHint("Time-driven ring motion (sampled at the integer frame)");
  DoubleParamDescriptor *cx = desc.defineDoubleParam("centerX");
  cx->setLabels("Center X", "Center X", "Center X"); cx->setRange(0, 1); cx->setDisplayRange(0, 1); cx->setDefault(0.5);
  DoubleParamDescriptor *cy = desc.defineDoubleParam("centerY");
  cy->setLabels("Center Y", "Center Y", "Center Y"); cy->setRange(0, 1); cy->setDisplayRange(0, 1); cy->setDefault(0.5);
  DoubleParamDescriptor *dc = desc.defineDoubleParam("decay");
  dc->setLabels("Decay", "Decay", "Decay"); dc->setRange(0, 4); dc->setDisplayRange(0, 4); dc->setDefault(1.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *RippleFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new RipplePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static RippleFactory p("org.purzos.ripple", 1, 0); ids.push_back(&p);
}
} }
