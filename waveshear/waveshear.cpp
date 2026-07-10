// Wave Shear — a sine ripple that shears rows sideways (and/or columns
// vertically). Each row's sampling x is displaced by a sine of its top-down
// position; the wave scrolls over time via `speed` so the picture wobbles like
// heat-haze or a jelly-roll shutter. `axis` picks horizontal, vertical, or both.
//
// A coordinate remap read with a bilinear sample; reaches across rows/columns
// -> tiles are off. Deterministic (time only enters through the scroll term).

#include "../common/purzfx.hpp"

using namespace purz;

class WaveShearProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int axis = 0, H = 0, W = 0;
  float amp = 12.f, freq = 4.f, ph = 0.f, mix = 1.f;
  explicit WaveShearProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float twoPi = 6.2831853f;
    const float invH = H > 0 ? 1.f / (float)H : 0.f;
    const float invW = W > 0 ? 1.f / (float)W : 0.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      const float rowShift = amp * std::sin(freq * twoPi * ty * invH + ph);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int tx = x - s.b.x1;
        float sx = (float)x, sy = (float)y;
        if (axis == 0 || axis == 2) sx += rowShift;                        // horizontal shear
        if (axis == 1 || axis == 2) sy += amp * std::sin(freq * twoPi * tx * invW + ph); // vertical shear
        float o[4]; s.bilin(sx, sy, o);
        float src[4]; s.at(x, y, src);
        dst[0] = q<PIX, maxv>(lerp(src[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(src[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(src[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(src[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class WaveShearPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_axis = nullptr;
  OFX::DoubleParam *_amp = nullptr, *_freq = nullptr, *_speed = nullptr, *_phase = nullptr, *_mix = nullptr;
public:
  explicit WaveShearPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _axis = fetchChoiceParam("axis");
    _amp = fetchDoubleParam("amp"); _freq = fetchDoubleParam("freq");
    _speed = fetchDoubleParam("speed"); _phase = fetchDoubleParam("phase");
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

    int axis; double amp, freq, speed, phase, mix;
    _axis->getValueAtTime(args.time, axis);
    _amp->getValueAtTime(args.time, amp);
    _freq->getValueAtTime(args.time, freq);
    _speed->getValueAtTime(args.time, speed);
    _phase->getValueAtTime(args.time, phase);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds();
    WaveShearProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.axis = axis;
    proc.amp = (float)amp * (float)args.renderScale.x;
    proc.freq = (float)freq;
    proc.ph = (float)(speed * frame * 0.1 + phase);
    proc.mix = (float)mix;
    proc.H = b.y2 - b.y1; proc.W = b.x2 - b.x1;

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

mDeclarePluginFactory(WaveShearFactory, {}, {});
using namespace OFX;
void WaveShearFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Wave Shear", false); }
void WaveShearFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  ChoiceParamDescriptor *ax = desc.defineChoiceParam("axis");
  ax->setLabels("Axis", "Axis", "Axis");
  ax->appendOption("Horizontal"); ax->appendOption("Vertical"); ax->appendOption("Both");
  ax->setDefault(0);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amp");
  am->setLabels("Amp", "Amp", "Amp"); am->setRange(0, 80); am->setDisplayRange(0, 80); am->setDefault(12);
  DoubleParamDescriptor *fr = desc.defineDoubleParam("freq");
  fr->setLabels("Freq", "Freq", "Freq"); fr->setRange(0.2, 20); fr->setDisplayRange(0.2, 20); fr->setDefault(4);
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-10, 10); sp->setDisplayRange(-10, 10); sp->setDefault(0);
  DoubleParamDescriptor *ph = desc.defineDoubleParam("phase");
  ph->setLabels("Phase", "Phase", "Phase"); ph->setRange(-6.2832, 6.2832); ph->setDisplayRange(-3.1416, 3.1416); ph->setDefault(0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *WaveShearFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new WaveShearPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static WaveShearFactory p("org.purzos.waveShear", 1, 0); ids.push_back(&p);
}
} }
