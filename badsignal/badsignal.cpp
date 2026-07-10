// Bad Signal — a dying broadcast. Whole scanlines occasionally drop into snow
// (a static burst), surviving rows get peppered with white/black tape-dropout
// dashes, and the entire picture carries an additive static fizz. The burst /
// dash decisions hold for `hold` frames so the fault looks like it's tearing
// through the tape rather than sparkling uniformly.
//
// Strictly per-pixel (each output reads only its own source pixel + hashes) ->
// tiles are safe. Deterministic: every "random" term is a hash of coord + held
// state (or frame, for the buzzing static), never rand().

#include "../common/purzfx.hpp"

using namespace purz;

class BadSignalProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int state = 0, frame = 0, seed = 0, H = 0;
  float dropout = 0.15f, noise = 0.2f, burst = 0.1f, mix = 1.f;
  explicit BadSignalProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      const float rowH = hash2(ty, state * 13 + seed);
      const bool snowRow = rowH < burst;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int tx = x - s.b.x1;
        float c[4]; s.at(x, y, c);
        float o[3];
        if (snowRow) {                                  // whole scanline -> snow
          float g = hash3(tx, ty, frame);
          o[0] = o[1] = o[2] = g;
        } else {
          o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
          if (hash3(tx / 8, ty, state) < dropout) {     // tape-dropout dash
            float d = hash2(tx / 8, ty + 7) < 0.5f ? 0.f : 1.f;
            o[0] = lerp(o[0], d, 0.9f); o[1] = lerp(o[1], d, 0.9f); o[2] = lerp(o[2], d, 0.9f);
          }
          float st = (hash3(tx, ty, frame) - 0.5f) * noise; // additive static fizz
          o[0] = clamp01(o[0] + st); o[1] = clamp01(o[1] + st); o[2] = clamp01(o[2] + st);
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

class BadSignalPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_hold = nullptr, *_seed = nullptr;
  OFX::DoubleParam *_dropout = nullptr, *_noise = nullptr, *_burst = nullptr, *_mix = nullptr;
public:
  explicit BadSignalPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _hold = fetchIntParam("hold"); _seed = fetchIntParam("seed");
    _dropout = fetchDoubleParam("dropout"); _noise = fetchDoubleParam("noise");
    _burst = fetchDoubleParam("burst"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int hold, seed; double dropout, noise, burst, mix;
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);
    _dropout->getValueAtTime(args.time, dropout);
    _noise->getValueAtTime(args.time, noise);
    _burst->getValueAtTime(args.time, burst);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds();
    BadSignalProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.dropout = (float)dropout; proc.noise = (float)noise; proc.burst = (float)burst;
    proc.mix = (float)mix; proc.seed = seed; proc.frame = frame;
    proc.state = frame / std::max(1, hold);
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

mDeclarePluginFactory(BadSignalFactory, {}, {});
using namespace OFX;
void BadSignalFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Bad Signal", true); }
void BadSignalFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *dr = desc.defineDoubleParam("dropout");
  dr->setLabels("Dropout", "Dropout", "Dropout"); dr->setRange(0, 1); dr->setDisplayRange(0, 1); dr->setDefault(0.15);
  DoubleParamDescriptor *no = desc.defineDoubleParam("noise");
  no->setLabels("Noise", "Noise", "Noise"); no->setRange(0, 1); no->setDisplayRange(0, 1); no->setDefault(0.2);
  DoubleParamDescriptor *bu = desc.defineDoubleParam("burst");
  bu->setLabels("Burst", "Burst", "Burst"); bu->setRange(0, 1); bu->setDisplayRange(0, 1); bu->setDefault(0.1);
  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold", "Hold", "Hold"); ho->setRange(1, 30); ho->setDisplayRange(1, 30); ho->setDefault(3);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *BadSignalFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new BadSignalPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static BadSignalFactory p("org.purzos.badSignal", 1, 0); ids.push_back(&p);
}
} }
