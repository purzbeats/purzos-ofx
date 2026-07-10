// Error Diffuse — Floyd–Steinberg error-diffusion dithering to a small fixed
// retro palette with chunky pixels. The whole frame is downsampled to a block
// grid (mean RGB per block, top-down order), F–S diffusion runs per channel
// snapping every cell to the nearest palette ink and pushing the quantisation
// error into the not-yet-visited neighbours, then the chosen colours are blown
// back up nearest-neighbour.
//
// Whole-frame two-pass (build grid, then write) like dither.cpp — the grid is
// immutable once built. Needs the whole frame, so tiles are OFF. Deterministic:
// the diffusion is a pure function of (source, params).

#include "../common/purzfx.hpp"
#include <vector>

using namespace purz;

namespace {
struct Pal { const char *name; int n; float rgb[8][3]; }; // sRGB 0..255
static const Pal PALS[] = {
  {"1-bit B/W", 2, {{15,15,25},{235,235,245}}},
  {"Game Boy",  4, {{15,56,15},{48,98,48},{139,172,15},{155,188,15}}},
  {"CGA",       4, {{0,0,0},{85,255,255},{255,85,255},{255,255,255}}},
  {"Amber",     4, {{20,10,0},{120,60,0},{200,120,10},{255,190,80}}},
  {"C64",       8, {{0,0,0},{255,255,255},{136,57,50},{103,182,189},
                    {139,63,150},{85,160,73},{64,49,141},{191,206,114}}},
};
static const int N_PALS = sizeof(PALS) / sizeof(PALS[0]);
} // namespace

class ErrorDiffuseProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _b{};
  int _dw = 1, _dh = 1;
  const Pal *_pal = &PALS[0];
  std::vector<float> _work;   // dw*dh*3 mutable mean RGB during diffusion
  std::vector<float> _cell;   // dw*dh*3 chosen palette colour (normalised)
public:
  explicit ErrorDiffuseProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void buildGrid(int palIdx, int block, bool serpentine, float strength) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    _pal = &PALS[clampv(palIdx, 0, N_PALS - 1)];
    _b = s.b; const int W = s.W, H = s.H;
    if (W <= 0 || H <= 0) return;
    const int bl = clampv(block, 1, 8);
    _dw = std::max(1, (int)std::lround((double)W / bl));
    _dh = std::max(1, (int)std::lround((double)H / bl));

    std::vector<float> sr(_dw * _dh, 0.f), sg(_dw * _dh, 0.f), sb(_dw * _dh, 0.f);
    std::vector<int>   cn(_dw * _dh, 0);
    for (int y = 0; y < H; y++) {
      const int ty = H - 1 - y;                      // OFX bottom-up -> top-down
      const int cy = std::min(_dh - 1, ((ty + 1) * _dh - 1) / H);
      for (int x = 0; x < W; x++) {
        float c[4]; s.at(_b.x1 + x, _b.y1 + y, c);
        const int cx = std::min(_dw - 1, ((x + 1) * _dw - 1) / W);
        const int ci = cy * _dw + cx;
        sr[ci] += c[0]; sg[ci] += c[1]; sb[ci] += c[2]; cn[ci]++;
      }
    }
    _work.assign(_dw * _dh * 3, 0.f);
    for (int i = 0; i < _dw * _dh; i++) {
      const float inv = cn[i] ? 1.f / cn[i] : 0.f;
      _work[i * 3] = sr[i] * inv; _work[i * 3 + 1] = sg[i] * inv; _work[i * 3 + 2] = sb[i] * inv;
    }

    _cell.assign(_dw * _dh * 3, 0.f);
    for (int ly = 0; ly < _dh; ly++) {
      const bool ltr = !serpentine || (ly % 2 == 0);
      const int dir = ltr ? 1 : -1;
      for (int i = 0; i < _dw; i++) {
        const int lx = ltr ? i : (_dw - 1 - i);
        const int ci = ly * _dw + lx;
        const float o0 = clamp01(_work[ci * 3]), o1 = clamp01(_work[ci * 3 + 1]), o2 = clamp01(_work[ci * 3 + 2]);
        int best = 0; float bd = 1e9f;
        for (int p = 0; p < _pal->n; p++) {
          const float dr = o0 - _pal->rgb[p][0] / 255.f, dg = o1 - _pal->rgb[p][1] / 255.f,
                      db = o2 - _pal->rgb[p][2] / 255.f;
          const float dd = dr * dr + dg * dg + db * db;
          if (dd < bd) { bd = dd; best = p; }
        }
        const float c0 = _pal->rgb[best][0] / 255.f, c1 = _pal->rgb[best][1] / 255.f, c2 = _pal->rgb[best][2] / 255.f;
        _cell[ci * 3] = c0; _cell[ci * 3 + 1] = c1; _cell[ci * 3 + 2] = c2;
        const float e0 = (o0 - c0) * strength, e1 = (o1 - c1) * strength, e2 = (o2 - c2) * strength;
        auto push = [&](int nx, int ny, float w) {
          if (nx < 0 || nx >= _dw || ny < 0 || ny >= _dh) return;
          const int ni = ny * _dw + nx;
          _work[ni * 3] += e0 * w; _work[ni * 3 + 1] += e1 * w; _work[ni * 3 + 2] += e2 * w;
        };
        push(lx + dir, ly,     7.f / 16.f);
        push(lx - dir, ly + 1, 3.f / 16.f);
        push(lx,       ly + 1, 5.f / 16.f);
        push(lx + dir, ly + 1, 1.f / 16.f);
      }
    }
  }

  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &win, float mix) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int W = s.W, H = s.H;
    if (W <= 0 || H <= 0) return;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - clampv(y - _b.y1, 0, H - 1);
      const int cy = std::min(_dh - 1, (2 * ty + 1) * _dh / (2 * H));
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int sx = clampv(x - _b.x1, 0, W - 1);
        const int cx = std::min(_dw - 1, (2 * sx + 1) * _dw / (2 * W));
        const int ci = cy * _dw + cx;
        float src[4]; s.at(x, y, src);
        dst[0] = q<PIX, maxv>(lerp(src[0], _cell[ci * 3],     mix));
        dst[1] = q<PIX, maxv>(lerp(src[1], _cell[ci * 3 + 1], mix));
        dst[2] = q<PIX, maxv>(lerp(src[2], _cell[ci * 3 + 2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(src[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ErrorDiffusePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_pal = nullptr;
  OFX::IntParam *_pixel = nullptr;
  OFX::BooleanParam *_serp = nullptr;
  OFX::DoubleParam *_strength = nullptr, *_mix = nullptr;
public:
  explicit ErrorDiffusePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _pal = fetchChoiceParam("palette"); _pixel = fetchIntParam("pixel");
    _serp = fetchBooleanParam("serpentine");
    _strength = fetchDoubleParam("strength"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int pal, pixel; bool serp; double strength, mix;
    _pal->getValueAtTime(args.time, pal);
    _pixel->getValueAtTime(args.time, pixel);
    pixel = std::max(1, (int)std::lround(pixel * args.renderScale.x));
    _serp->getValueAtTime(args.time, serp);
    _strength->getValueAtTime(args.time, strength);
    _mix->getValueAtTime(args.time, mix);

    ErrorDiffuseProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define GO(PIX, MAXV) do { \
      if (nc == 4) { proc.buildGrid<PIX, 4, MAXV>(pal, pixel, serp, (float)strength); \
                     proc.writePixels<PIX, 4, MAXV>(args.renderWindow, (float)mix); } \
      else         { proc.buildGrid<PIX, 3, MAXV>(pal, pixel, serp, (float)strength); \
                     proc.writePixels<PIX, 3, MAXV>(args.renderWindow, (float)mix); } } while (0)
    switch (depth) {
      case OFX::eBitDepthUByte:  GO(unsigned char, 255); break;
      case OFX::eBitDepthUShort: GO(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  GO(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef GO
  }
};

mDeclarePluginFactory(ErrorDiffuseFactory, {}, {});
using namespace OFX;
void ErrorDiffuseFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Error Diffuse", false); }
void ErrorDiffuseFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  ChoiceParamDescriptor *pl = desc.defineChoiceParam("palette");
  pl->setLabels("Palette", "Palette", "Palette");
  for (int i = 0; i < N_PALS; i++) pl->appendOption(PALS[i].name);
  pl->setDefault(0);
  IntParamDescriptor *px = desc.defineIntParam("pixel");
  px->setLabels("Pixel size", "Pixel size", "Pixel size");
  px->setRange(1, 8); px->setDisplayRange(1, 8); px->setDefault(1);
  BooleanParamDescriptor *se = desc.defineBooleanParam("serpentine");
  se->setLabels("Serpentine", "Serpentine", "Serpentine");
  se->setDefault(true); se->setHint("Alternate scan direction each row (fewer worm artefacts)");
  DoubleParamDescriptor *st = desc.defineDoubleParam("strength");
  st->setLabels("Strength", "Strength", "Strength");
  st->setRange(0, 1); st->setDisplayRange(0, 1); st->setDefault(1.0);
  st->setHint("Fraction of quantisation error propagated to neighbours");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ErrorDiffuseFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ErrorDiffusePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ErrorDiffuseFactory p("org.purzos.errorDiffuse", 1, 0); ids.push_back(&p);
}
} }
