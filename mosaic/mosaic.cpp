// Mosaic — chunky pixelation. The frame is chopped into fixed square blocks;
// each block averages to one colour. With a Square shape every pixel takes the
// block mean (classic pixelation); with Circle / Diamond only the pixels inside
// the inscribed shape take the mean and the rest keep the original source, so
// the image reads as a grid of coloured dots on the live picture. `round`
// blends the block from a hard square (0) toward the pure shape (1).
//
// Whole-frame two-pass (build block grid, then write). Tiles OFF. Deterministic.

#include "../common/purzfx.hpp"
#include <vector>

using namespace purz;

class MosaicProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _b{};
  int _dw = 1, _dh = 1, _cell = 8;
  std::vector<float> _mean;   // dw*dh*3 block-average RGB (normalised)
public:
  explicit MosaicProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void buildGrid(int cellPx) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    _b = s.b; const int W = s.W, H = s.H;
    if (W <= 0 || H <= 0) return;
    _cell = clampv(cellPx, 2, 64);
    _dw = std::max(1, (W + _cell - 1) / _cell);
    _dh = std::max(1, (H + _cell - 1) / _cell);
    std::vector<float> sr(_dw * _dh, 0.f), sg(_dw * _dh, 0.f), sb(_dw * _dh, 0.f);
    std::vector<int>   cn(_dw * _dh, 0);
    for (int y = 0; y < H; y++) {
      const int ty = H - 1 - y;                       // top-down
      const int cy = std::min(_dh - 1, ty / _cell);
      for (int x = 0; x < W; x++) {
        float c[4]; s.at(_b.x1 + x, _b.y1 + y, c);
        const int cx = std::min(_dw - 1, x / _cell);
        const int ci = cy * _dw + cx;
        sr[ci] += c[0]; sg[ci] += c[1]; sb[ci] += c[2]; cn[ci]++;
      }
    }
    _mean.assign(_dw * _dh * 3, 0.f);
    for (int i = 0; i < _dw * _dh; i++) {
      const float inv = cn[i] ? 1.f / cn[i] : 0.f;
      _mean[i * 3] = sr[i] * inv; _mean[i * 3 + 1] = sg[i] * inv; _mean[i * 3 + 2] = sb[i] * inv;
    }
  }

  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &win, int shape, float round, float mix) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int W = s.W, H = s.H;
    if (W <= 0 || H <= 0) return;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - clampv(y - _b.y1, 0, H - 1);
      const int cy = std::min(_dh - 1, ty / _cell);
      const int lty = ty - cy * _cell;                 // row within block (top-down)
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int sx = clampv(x - _b.x1, 0, W - 1);
        const int cx = std::min(_dw - 1, sx / _cell);
        const int ltx = sx - cx * _cell;
        const int ci = cy * _dw + cx;
        float src[4]; s.at(x, y, src);
        // normalised offset from block centre in [-1,1]
        const float du = (ltx + 0.5f) / _cell * 2.f - 1.f;
        const float dv = (lty + 0.5f) / _cell * 2.f - 1.f;
        const float cheb = std::max(std::fabs(du), std::fabs(dv)); // square metric
        float sm;
        if (shape == 1)      sm = std::sqrt(du * du + dv * dv);      // circle
        else if (shape == 2) sm = std::fabs(du) + std::fabs(dv);     // diamond
        else                 sm = cheb;                              // square
        const float metric = lerp(cheb, sm, round);
        const bool inside = (shape == 0) ? true : (metric <= 1.f);
        float o0 = src[0], o1 = src[1], o2 = src[2];
        if (inside) { o0 = _mean[ci * 3]; o1 = _mean[ci * 3 + 1]; o2 = _mean[ci * 3 + 2]; }
        dst[0] = q<PIX, maxv>(lerp(src[0], o0, mix));
        dst[1] = q<PIX, maxv>(lerp(src[1], o1, mix));
        dst[2] = q<PIX, maxv>(lerp(src[2], o2, mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(src[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class MosaicPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_size = nullptr;
  OFX::ChoiceParam *_shape = nullptr;
  OFX::DoubleParam *_round = nullptr, *_mix = nullptr;
public:
  explicit MosaicPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _size = fetchIntParam("size"); _shape = fetchChoiceParam("shape");
    _round = fetchDoubleParam("round"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int size, shape; double round, mix;
    _size->getValueAtTime(args.time, size);
    size = std::max(1, (int)std::lround(size * args.renderScale.x));
    _shape->getValueAtTime(args.time, shape);
    _round->getValueAtTime(args.time, round);
    _mix->getValueAtTime(args.time, mix);

    MosaicProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define GO(PIX, MAXV) do { \
      if (nc == 4) { proc.buildGrid<PIX, 4, MAXV>(size); \
                     proc.writePixels<PIX, 4, MAXV>(args.renderWindow, shape, (float)round, (float)mix); } \
      else         { proc.buildGrid<PIX, 3, MAXV>(size); \
                     proc.writePixels<PIX, 3, MAXV>(args.renderWindow, shape, (float)round, (float)mix); } } while (0)
    switch (depth) {
      case OFX::eBitDepthUByte:  GO(unsigned char, 255); break;
      case OFX::eBitDepthUShort: GO(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  GO(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef GO
  }
};

mDeclarePluginFactory(MosaicFactory, {}, {});
using namespace OFX;
void MosaicFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Mosaic", false); }
void MosaicFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  IntParamDescriptor *sz = desc.defineIntParam("size");
  sz->setLabels("Block size", "Block size", "Block size");
  sz->setRange(2, 64); sz->setDisplayRange(2, 64); sz->setDefault(8);
  ChoiceParamDescriptor *sh = desc.defineChoiceParam("shape");
  sh->setLabels("Shape", "Shape", "Shape");
  sh->appendOption("Square"); sh->appendOption("Circle"); sh->appendOption("Diamond");
  sh->setDefault(0);
  DoubleParamDescriptor *rd = desc.defineDoubleParam("round");
  rd->setLabels("Round", "Round", "Round"); rd->setRange(0, 1); rd->setDisplayRange(0, 1); rd->setDefault(1.0);
  rd->setHint("0 = fill the whole block, 1 = pure inscribed shape");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *MosaicFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new MosaicPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static MosaicFactory p("org.purzos.mosaic", 1, 0); ids.push_back(&p);
}
} }
