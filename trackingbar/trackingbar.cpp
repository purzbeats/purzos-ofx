// Tracking Bar — the band of torn, desaturated noise that slides up (or down)
// a mistracked VHS. A soft-edged band centred at `pos` drifts vertically over
// time; inside it lines tear sideways, colour drains out, and static creeps in
// toward the band's core.
//
// Reads a horizontally SHIFTED source (atWrap) -> tiles OFF. Deterministic:
// randomness is hash on integer coords + seed + frame index.

#include "../common/purzfx.hpp"

using namespace purz;

class TrackingBarProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0, seed = 1;
  float pos = 0.85f, height = 0.12f, speed = 0.15f, tear = 60.f, noise = 0.6f, mix = 1.f;
  explicit TrackingBarProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invH = H > 0 ? 1.f / H : 0.f;
    const float centre = fracf(pos + speed * (float)frame * 0.05f);
    const float halfH = std::max(1e-4f, height * 0.5f);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);
      float tyN = (float)ty * invH;
      float dist = std::fabs(tyN - centre);
      dist = std::min(dist, 1.f - dist);          // wrap-around distance
      float t = dist < halfH ? (1.f - dist / halfH) : 0.f;   // 0 at edge .. 1 at core
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float o[4] = { c[0], c[1], c[2], c[3] };
        if (t > 0.f) {
          float disp = tear * sh2(ty, frame * 7 + seed) * (0.5f + 0.5f * t);
          int sx = x + (int)std::floor(disp + 0.5f);
          float sm[4]; s.atWrap(sx, y, sm);
          float l = luma(sm[0], sm[1], sm[2]);
          float des = 0.7f * t;                    // drain colour toward grey
          float r = lerp(sm[0], l, des), g = lerp(sm[1], l, des), bb = lerp(sm[2], l, des);
          float st = hash3(x - s.b.x1, ty, frame * 17 + seed);
          float n = clamp01(noise * t);
          o[0] = lerp(r, st, n); o[1] = lerp(g, st, n); o[2] = lerp(bb, st, n); o[3] = sm[3];
        }
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(c[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class TrackingBarPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_pos = nullptr, *_height = nullptr, *_speed = nullptr, *_tear = nullptr, *_noise = nullptr, *_mix = nullptr;
  OFX::IntParam *_seed = nullptr;
public:
  explicit TrackingBarPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _pos = fetchDoubleParam("pos"); _height = fetchDoubleParam("height");
    _speed = fetchDoubleParam("speed"); _tear = fetchDoubleParam("tear");
    _noise = fetchDoubleParam("noise"); _seed = fetchIntParam("seed");
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

    double pos, height, speed, tear, noise, mix; int seed;
    _pos->getValueAtTime(args.time, pos);
    _height->getValueAtTime(args.time, height);
    _speed->getValueAtTime(args.time, speed);
    _tear->getValueAtTime(args.time, tear);
    _noise->getValueAtTime(args.time, noise);
    _seed->getValueAtTime(args.time, seed);
    _mix->getValueAtTime(args.time, mix);

    TrackingBarProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    proc.frame = (int)std::floor(args.time); proc.seed = seed;
    proc.pos = (float)pos; proc.height = (float)height; proc.speed = (float)speed;
    proc.tear = (float)(tear * args.renderScale.x); proc.noise = (float)noise; proc.mix = (float)mix;

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

mDeclarePluginFactory(TrackingBarFactory, {}, {});
using namespace OFX;
void TrackingBarFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Tracking Bar", false); }
void TrackingBarFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *po = desc.defineDoubleParam("pos");
  po->setLabels("Position", "Position", "Position"); po->setRange(0, 1); po->setDisplayRange(0, 1); po->setDefault(0.85);
  po->setHint("Band centre (0 top .. 1 bottom)");
  DoubleParamDescriptor *he = desc.defineDoubleParam("height");
  he->setLabels("Height", "Height", "Height"); he->setRange(0.02, 0.4); he->setDisplayRange(0.02, 0.4); he->setDefault(0.12);
  he->setHint("Band height as a fraction of the frame");
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-2, 2); sp->setDisplayRange(-2, 2); sp->setDefault(0.15);
  sp->setHint("Vertical drift speed of the band");
  DoubleParamDescriptor *te = desc.defineDoubleParam("tear");
  te->setLabels("Tear", "Tear", "Tear"); te->setRange(0, 150); te->setDisplayRange(0, 150); te->setDefault(60);
  te->setHint("Horizontal line displacement in pixels");
  DoubleParamDescriptor *no = desc.defineDoubleParam("noise");
  no->setLabels("Noise", "Noise", "Noise"); no->setRange(0, 1); no->setDisplayRange(0, 1); no->setDefault(0.6);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *TrackingBarFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new TrackingBarPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static TrackingBarFactory p("org.purzos.trackingBar", 1, 0); ids.push_back(&p);
}
} }
