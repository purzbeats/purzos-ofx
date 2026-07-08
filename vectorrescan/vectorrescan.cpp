// Vector Rescan — oscillographics: the image redrawn by a beam. Sparse scan
// lines ride the picture's brightness like terrain (Rutt/Etra deflection),
// the beam is DOTTED along each line (intensity follows luma, dark areas draw
// nothing), an optional wave makes adjacent lines undulate out of step, and
// the whole trace gets a two-tier phosphor glow: hot near-white cores inside
// a huge soft coloured halo, additive over black.
//
// Unlike the rest of the pack this is FORWARD-rendered: dots are splatted
// into a float accumulator, then bloomed and tinted. Content-driven and
// periodic — no randomness, fully deterministic. Runs on the host's render
// thread. All geometry is computed in top-down space.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace {
template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Beam { const char *name; float core[3], halo[3]; };
static const Beam BEAMS[] = {
  {"Cyan scope",      {0.85f, 1.00f, 1.00f}, {0.15f, 0.50f, 1.00f}},
  {"Green phosphor",  {0.90f, 1.00f, 0.90f}, {0.15f, 0.90f, 0.30f}},
  {"Amber",           {1.00f, 0.95f, 0.80f}, {1.00f, 0.55f, 0.10f}},
  {"White",           {1.00f, 1.00f, 1.00f}, {0.60f, 0.70f, 0.90f}},
};
static const int N_BEAMS = sizeof(BEAMS) / sizeof(BEAMS[0]);
// choice index N_BEAMS == "Custom" -> RGB params take over
} // namespace

// ---------------------------------------------------------------------------
class VectorRescanProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<float> _L, _beam, _tmp, _halo, _pref; // all top-down
public:
  explicit VectorRescanProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  void blurH(std::vector<float> &img, std::vector<float> &out, int r) {
    for (int y = 0; y < _H; y++) {
      const float *row = &img[(size_t)y * _W];
      _pref[0] = 0.0f;
      for (int x = 0; x < _W; x++) _pref[x + 1] = _pref[x] + row[x];
      float *o = &out[(size_t)y * _W];
      for (int x = 0; x < _W; x++) {
        const int a = std::max(0, x - r), b = std::min(_W - 1, x + r);
        o[x] = (_pref[b + 1] - _pref[a]) / (float)(b - a + 1);
      }
    }
  }
  void blurV(std::vector<float> &img, std::vector<float> &out, int r) {
    for (int x = 0; x < _W; x++) {
      _pref[0] = 0.0f;
      for (int y = 0; y < _H; y++) _pref[y + 1] = _pref[y] + img[(size_t)y * _W + x];
      for (int y = 0; y < _H; y++) {
        const int a = std::max(0, y - r), b = std::min(_H - 1, y + r);
        out[(size_t)y * _W + x] = (_pref[b + 1] - _pref[a]) / (float)(b - a + 1);
      }
    }
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, int vertical, int spacing, int pitch, float dotR,
               float lift, float zero, float thr, int smooth,
               float wobble, float waveLen, float wavePhase,
               int glowR, float coreGain, float haloGain,
               const float core[3], const float halo[3], float bg) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    const size_t N = (size_t)_W * _H;
    _L.assign(N, 0.0f); _beam.assign(N, 0.0f);
    _tmp.resize(N); _halo.resize(N);
    _pref.resize((size_t)std::max(_W, _H) + 1);

    // terrain: softened top-down luma
    for (int ly = 0; ly < _H; ly++) {
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, _bounds.y2 - 1 - ly);
      if (!row) continue;
      float *m = &_L[(size_t)ly * _W];
      for (int x = 0; x < _W; x++) {
        PIX *p = row + x * nComps;
        m[x] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / (float)maxv;
      }
    }
    if (smooth > 0) {
      blurH(_L, _tmp, smooth);
      blurV(_tmp, _L, smooth);
    }
    if (_effect.abort()) return;

    // splat the beam: every `spacing`-th scan line, dots every `pitch` px.
    // Horizontal: rows deflected UP by brightness. Vertical: columns
    // deflected LEFT — same terrain, rotated 90°.
    const int ri = std::max(1, (int)std::ceil(dotR));
    const float invR2 = 1.0f / (dotR * dotR);
    const float omega = 6.2831853f / std::max(4.0f, waveLen);
    const int lines = vertical ? _W : _H;   // one scan line per `spacing`
    const int span = vertical ? _H : _W;    // the axis the beam marches along
    for (int k = 0; k * spacing < lines; k++) {
      const int l0 = k * spacing;
      const float linePhase = wavePhase + k * 2.39996f; // golden-angle offset
      for (int s = 0; s < span; s += pitch) {
        const int sx = vertical ? l0 : s, sy = vertical ? s : l0;
        const float b = _L[(size_t)sy * _W + sx];
        if (b < thr) continue;
        const float defl = l0 - lift * (b - zero)
                         + wobble * std::sin(s * omega + linePhase);
        const int cx = vertical ? (int)std::lround(defl) : sx;
        const int cy = vertical ? sy : (int)std::lround(defl);
        const float inten = b * b; // beam current follows luma, squared = punch
        for (int dy = -ri; dy <= ri; dy++) {
          const int py = cy + dy;
          if (py < 0 || py >= _H) continue;
          float *brow = &_beam[(size_t)py * _W];
          for (int dx = -ri; dx <= ri; dx++) {
            const int px = cx + dx;
            if (px < 0 || px >= _W) continue;
            const float d2 = (dx * dx + dy * dy) * invR2;
            if (d2 > 1.0f) continue;
            const float w = (1.0f - d2) * (1.0f - d2);
            brow[px] += inten * w;
          }
        }
      }
    }
    if (_effect.abort()) return;

    // two-tier glow: tight bloom around the cores, huge soft halo
    const int tightR = std::max(1, (int)std::lround(dotR));
    blurH(_beam, _tmp, tightR);
    blurV(_tmp, _halo, tightR);                 // _halo = tight bloom (borrowed)
    for (size_t i = 0; i < N; i++) _tmp[i] = _beam[i] + _halo[i];
    std::swap(_beam, _tmp);                     // _beam = core + tight bloom
    blurH(_beam, _tmp, glowR);
    blurV(_tmp, _halo, glowR);
    blurH(_halo, _tmp, glowR / 2 + 1);
    blurV(_tmp, _halo, glowR / 2 + 1);          // _halo = big soft halo

    // composite: additive tinted beam over (optionally faint) source
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      const int ly = _bounds.y2 - 1 - y;
      const float *bm = &_beam[(size_t)ly * _W];
      const float *hl = &_halo[(size_t)ly * _W];

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        const float c = bm[lx] * coreGain;
        const float h = hl[lx] * haloGain * (float)glowR; // blur dilutes by ~1/r
        float r = c * core[0] + h * halo[0];
        float g = c * core[1] + h * halo[1];
        float b = c * core[2] + h * halo[2];
        if (bg > 0.0f && row) {
          PIX *p = row + lx * nComps;
          r += bg * p[0] / (float)maxv;
          g += bg * p[1] / (float)maxv;
          b += bg * p[2] / (float)maxv;
        }
        dst[0] = q<PIX, nComps, maxv>(r);
        dst[1] = q<PIX, nComps, maxv>(g);
        dst[2] = q<PIX, nComps, maxv>(b);
        if (nComps == 4) dst[3] = row ? row[lx * nComps + 3] : (PIX)maxv;
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class VectorRescanPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_spacing = nullptr, *_pitch = nullptr, *_smooth = nullptr, *_glow = nullptr;
  OFX::DoubleParam *_dot = nullptr, *_lift = nullptr, *_zero = nullptr, *_thr = nullptr,
                   *_wobble = nullptr, *_waveLen = nullptr, *_speed = nullptr,
                   *_coreGain = nullptr, *_haloGain = nullptr, *_bg = nullptr;
  OFX::ChoiceParam *_beam = nullptr, *_orient = nullptr;
  OFX::RGBParam *_coreCol = nullptr, *_haloCol = nullptr;
public:
  explicit VectorRescanPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _orient = fetchChoiceParam("orient");
    _spacing = fetchIntParam("spacing");
    _pitch = fetchIntParam("pitch");
    _dot = fetchDoubleParam("dot");
    _lift = fetchDoubleParam("lift");
    _zero = fetchDoubleParam("zero");
    _thr = fetchDoubleParam("threshold");
    _smooth = fetchIntParam("smooth");
    _wobble = fetchDoubleParam("wobble");
    _waveLen = fetchDoubleParam("wavelen");
    _speed = fetchDoubleParam("speed");
    _glow = fetchIntParam("glow");
    _coreGain = fetchDoubleParam("coreGain");
    _haloGain = fetchDoubleParam("haloGain");
    _beam = fetchChoiceParam("beam");
    _coreCol = fetchRGBParam("coreCol");
    _haloCol = fetchRGBParam("haloCol");
    _bg = fetchDoubleParam("bg");
  }

  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);

    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4
                 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int orient, spacing, pitch, smooth, glow, beam;
    double dot, lift, zero, thr, wobble, waveLen, speed, coreGain, haloGain, bg;
    _orient->getValueAtTime(args.time, orient);
    _spacing->getValueAtTime(args.time, spacing);
    _pitch->getValueAtTime(args.time, pitch);
    _dot->getValueAtTime(args.time, dot);
    _lift->getValueAtTime(args.time, lift);
    _zero->getValueAtTime(args.time, zero);
    _thr->getValueAtTime(args.time, thr);
    _smooth->getValueAtTime(args.time, smooth);
    _wobble->getValueAtTime(args.time, wobble);
    _waveLen->getValueAtTime(args.time, waveLen);
    _speed->getValueAtTime(args.time, speed);
    _glow->getValueAtTime(args.time, glow);
    _coreGain->getValueAtTime(args.time, coreGain);
    _haloGain->getValueAtTime(args.time, haloGain);
    _beam->getValueAtTime(args.time, beam);
    _bg->getValueAtTime(args.time, bg);

    float core[3], halo[3];
    if (beam < N_BEAMS) {
      for (int c = 0; c < 3; c++) { core[c] = BEAMS[beam].core[c]; halo[c] = BEAMS[beam].halo[c]; }
    } else {
      double r, g, b;
      _coreCol->getValueAtTime(args.time, r, g, b);
      core[0] = (float)r; core[1] = (float)g; core[2] = (float)b;
      _haloCol->getValueAtTime(args.time, r, g, b);
      halo[0] = (float)r; halo[1] = (float)g; halo[2] = (float)b;
    }

    // every geometric unit is full-res pixels; scale for proxy renders
    const double rs = args.renderScale.x;
    spacing = std::max(2, (int)std::lround(spacing * rs));
    pitch = std::max(1, (int)std::lround(pitch * rs));
    dot = std::max(0.5, dot * rs);
    lift *= rs; wobble *= rs; waveLen *= rs;
    smooth = (int)std::lround(smooth * rs);
    glow = std::max(2, (int)std::lround(glow * rs));

    const float wavePhase = (float)(args.time * speed * 0.35);

    VectorRescanProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, orient, spacing, \
                     pitch, (float)dot, (float)lift, (float)zero, (float)thr,     \
                     smooth, (float)wobble, (float)waveLen, wavePhase, glow,      \
                     (float)coreGain, (float)haloGain, core, halo, (float)bg);    \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, orient, spacing, \
                     pitch, (float)dot, (float)lift, (float)zero, (float)thr,     \
                     smooth, (float)wobble, (float)waveLen, wavePhase, glow,      \
                     (float)coreGain, (float)haloGain, core, halo, (float)bg);    \
    } while (0)

    switch (depth) {
      case OFX::eBitDepthUByte:  DISPATCH(unsigned char, 255); break;
      case OFX::eBitDepthUShort: DISPATCH(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  DISPATCH(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef DISPATCH
  }
};

// ---------------------------------------------------------------------------
mDeclarePluginFactory(VectorRescanFactory, {}, {});

using namespace OFX;

void VectorRescanFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Vector Rescan", "Vector Rescan", "Vector Rescan");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // beam + glow are whole-frame
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void VectorRescanFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  ChoiceParamDescriptor *orp = desc.defineChoiceParam("orient");
  orp->setLabels("Orientation", "Orientation", "Orientation");
  orp->appendOption("Horizontal");
  orp->appendOption("Vertical");
  orp->setDefault(0);

  IntParamDescriptor *sp = desc.defineIntParam("spacing");
  sp->setLabels("Line spacing", "Line spacing", "Line spacing");
  sp->setRange(4, 64); sp->setDisplayRange(8, 32); sp->setDefault(14);

  IntParamDescriptor *pi = desc.defineIntParam("pitch");
  pi->setLabels("Dot pitch", "Dot pitch", "Dot pitch");
  pi->setRange(1, 16); pi->setDisplayRange(2, 8); pi->setDefault(4);

  DoubleParamDescriptor *dt = desc.defineDoubleParam("dot");
  dt->setLabels("Dot size", "Dot size", "Dot size");
  dt->setRange(0.5, 6); dt->setDisplayRange(1, 4); dt->setDefault(1.7);

  DoubleParamDescriptor *li = desc.defineDoubleParam("lift");
  li->setLabels("Lift", "Lift", "Lift");
  li->setRange(-300, 300); li->setDisplayRange(-150, 150); li->setDefault(80);

  DoubleParamDescriptor *ze = desc.defineDoubleParam("zero");
  ze->setLabels("Zero level", "Zero level", "Zero level");
  ze->setRange(0, 1); ze->setDisplayRange(0, 1); ze->setDefault(0.1);

  DoubleParamDescriptor *th = desc.defineDoubleParam("threshold");
  th->setLabels("Threshold", "Threshold", "Threshold");
  th->setRange(0, 1); th->setDisplayRange(0, 0.5); th->setDefault(0.12);

  IntParamDescriptor *sm = desc.defineIntParam("smooth");
  sm->setLabels("Smooth", "Smooth", "Smooth");
  sm->setRange(0, 32); sm->setDisplayRange(0, 16); sm->setDefault(5);

  DoubleParamDescriptor *wo = desc.defineDoubleParam("wobble");
  wo->setLabels("Wave", "Wave", "Wave");
  wo->setRange(0, 32); wo->setDisplayRange(0, 16); wo->setDefault(5);

  DoubleParamDescriptor *wl = desc.defineDoubleParam("wavelen");
  wl->setLabels("Wave length", "Wave length", "Wave length");
  wl->setRange(10, 400); wl->setDisplayRange(20, 200); wl->setDefault(90);

  DoubleParamDescriptor *spd = desc.defineDoubleParam("speed");
  spd->setLabels("Speed", "Speed", "Speed");
  spd->setRange(0, 4); spd->setDisplayRange(0, 2); spd->setDefault(1);

  IntParamDescriptor *gl = desc.defineIntParam("glow");
  gl->setLabels("Glow radius", "Glow radius", "Glow radius");
  gl->setRange(2, 128); gl->setDisplayRange(8, 64); gl->setDefault(26);

  DoubleParamDescriptor *cg = desc.defineDoubleParam("coreGain");
  cg->setLabels("Core gain", "Core gain", "Core gain");
  cg->setRange(0, 4); cg->setDisplayRange(0, 3); cg->setDefault(1.4);

  DoubleParamDescriptor *hg = desc.defineDoubleParam("haloGain");
  hg->setLabels("Halo gain", "Halo gain", "Halo gain");
  hg->setRange(0, 4); hg->setDisplayRange(0, 3); hg->setDefault(1.0);

  ChoiceParamDescriptor *be = desc.defineChoiceParam("beam");
  be->setLabels("Beam", "Beam", "Beam");
  for (int i = 0; i < N_BEAMS; i++) be->appendOption(BEAMS[i].name);
  be->appendOption("Custom");
  be->setDefault(0);

  RGBParamDescriptor *cc = desc.defineRGBParam("coreCol");
  cc->setLabels("Core colour", "Core colour", "Core colour");
  cc->setDefault(0.85, 1.0, 1.0);
  cc->setHint("Used when Beam is Custom");

  RGBParamDescriptor *hc = desc.defineRGBParam("haloCol");
  hc->setLabels("Halo colour", "Halo colour", "Halo colour");
  hc->setDefault(0.15, 0.5, 1.0);
  hc->setHint("Used when Beam is Custom");

  DoubleParamDescriptor *bg = desc.defineDoubleParam("bg");
  bg->setLabels("Background", "Background", "Background");
  bg->setRange(0, 1); bg->setDisplayRange(0, 0.5); bg->setDefault(0);
}

OFX::ImageEffect *VectorRescanFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new VectorRescanPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static VectorRescanFactory p("org.purzos.vectorRescan", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
