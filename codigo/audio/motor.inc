/* ============================ Motor de vídeo (Windows) ============================
   Quem transmite: o Telão Audio captura o jogo (Windows Graphics Capture), comprime na placa (Media Foundation, codificador
   da própria placa) e entrega os quadros prontos ao Telão; o Chromium só os leva pela rede.
   Quem assiste: o Telão entrega os quadros comprimidos que chegam; o Telão Audio decodifica na placa e desenha numa
   janela própria, posicionada exatamente sobre a tela do amigo na sala.
   Tudo pelo mesmo WebSocket (o do "trabalhador" do motor dentro do Telão), só para a página local do app. */

#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>

// GUIDs definidos aqui (o mingw não traz a biblioteca com eles)
static const GUID kMF_MT_MAJOR_TYPE        = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID kMF_MT_SUBTYPE           = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID kMF_MT_FRAME_SIZE        = {0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
static const GUID kMF_MT_FRAME_RATE        = {0xc459a2e8, 0x3d2c, 0x4e44, {0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0}};
static const GUID kMF_MT_AVG_BITRATE       = {0x20332624, 0xfb0d, 0x4d9e, {0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e}};
static const GUID kMF_MT_INTERLACE_MODE    = {0xe2724bb8, 0xe676, 0x4806, {0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd}};
static const GUID kMF_MT_PIXEL_ASPECT      = {0xc6376a1e, 0x8d0a, 0x4027, {0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6}};
static const GUID kMF_MT_MPEG2_PROFILE     = {0xad76a80b, 0x2d5c, 0x4e0b, {0xb3, 0x75, 0x64, 0xe5, 0x20, 0x13, 0x70, 0x36}};
static const GUID kMF_MT_MPEG_SEQ_HEADER   = {0x3c036de7, 0x3ad0, 0x4c9e, {0x92, 0x16, 0xee, 0x6d, 0x6a, 0xc2, 0x1c, 0xb3}};
static const GUID kMF_MT_NOMINAL_RANGE     = {0xc21b8ee5, 0xb956, 0x4071, {0x8d, 0xaf, 0x32, 0x5e, 0xdf, 0x5c, 0xab, 0x11}};
static const GUID kMF_MT_YUV_MATRIX        = {0x3e23d450, 0x2c75, 0x4d25, {0xa0, 0x0e, 0xb9, 0x16, 0x70, 0xd1, 0x23, 0x27}};
static const GUID kMF_MT_MIN_APERTURE      = {0xd7388766, 0x18fe, 0x48c6, {0xa1, 0x77, 0xee, 0x89, 0x48, 0x67, 0xc8, 0xc4}};
static const GUID kMFT_CAT_VIDEO_ENCODER   = {0xf79eac7d, 0xe545, 0x4387, {0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a}};
static const GUID kMFT_CAT_VIDEO_DECODER   = {0xd6c02d4b, 0x6833, 0x45b4, {0x97, 0x1a, 0x05, 0xa4, 0xb0, 0x4b, 0xab, 0x91}};
static const GUID kMF_TRANSFORM_ASYNC_UNLOCK = {0xe5666d6b, 0x3422, 0x4eb6, {0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x07}};
static const GUID kMF_TRANSFORM_ASYNC      = {0xf81a699a, 0x649a, 0x497d, {0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a}};
static const GUID kMFT_ENUM_ADAPTER_LUID   = {0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};
static const GUID kMFT_FRIENDLY_NAME       = {0x314ffbae, 0x5b41, 0x4c95, {0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3}};
static const GUID kMF_SA_D3D11_AWARE       = {0x206b4fc8, 0xfcf9, 0x4c51, {0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0}};
static const GUID kMF_LOW_LATENCY          = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
static const GUID kMFSample_CleanPoint     = {0x9cdf01d8, 0xa0f0, 0x43ba, {0xb0, 0x77, 0xea, 0xa0, 0x6c, 0xbd, 0x72, 0x8a}};
static const GUID kMFMediaType_Video       = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kCLSID_MSH264Decoder     = {0x62ce7e72, 0x4c71, 0x4d20, {0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d}};
static const GUID kCAPI_RateControlMode    = {0x1c0608e9, 0x370c, 0x4710, {0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23}};
static const GUID kCAPI_MeanBitRate        = {0xf7222374, 0x2144, 0x4815, {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
static const GUID kCAPI_ForceKeyFrame      = {0x398c1b98, 0x8353, 0x475a, {0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45}};
static const GUID kCAPI_GOPSize            = {0x95f31b26, 0x95a4, 0x41aa, {0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1}};
static const GUID kCAPI_BPictureCount      = {0x8d390aac, 0xdc5c, 0x4200, {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};
static const GUID kCAPI_LowLatency         = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
static const GUID kIID_IMFTrackedSample    = {0x245bf8e9, 0x0755, 0x40f7, {0x88, 0xa5, 0xae, 0x0f, 0x18, 0xd5, 0x5e, 0x17}};
static GUID mfFourcc(DWORD f){ GUID g = {f, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}; return g; }
static const DWORD FCC_NV12 = 0x3231564E, FCC_H264 = 0x34363248, FCC_HEVC = 0x43564548;

struct IMFTrackedSampleT : public IUnknown { virtual HRESULT STDMETHODCALLTYPE SetAllocator(IMFAsyncCallback* cb, IUnknown* state) = 0; };
static const GUID kIID_IMFShutdown = {0x97ec2ea4, 0x0e42, 0x4937, {0x97, 0xac, 0x9d, 0x6d, 0x32, 0x88, 0x24, 0xe1}};
struct IMFShutdownT : public IUnknown { virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0; virtual HRESULT STDMETHODCALLTYPE GetShutdownStatus(int* st) = 0; };

// Media Foundation carregado na hora (Windows "N" sem o pacote de mídia: o motor só fica indisponível)
struct MFApi {
  bool tried = false, ok = false;
  HRESULT (WINAPI *Startup)(ULONG, DWORD) = nullptr;
  HRESULT (WINAPI *CreateMediaType)(IMFMediaType**) = nullptr;
  HRESULT (WINAPI *CreateSample)(IMFSample**) = nullptr;
  HRESULT (WINAPI *CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
  HRESULT (WINAPI *CreateDXGIDeviceManager)(UINT*, IMFDXGIDeviceManager**) = nullptr;
  HRESULT (WINAPI *CreateDXGISurfaceBuffer)(REFIID, IUnknown*, UINT, BOOL, IMFMediaBuffer**) = nullptr;
  HRESULT (WINAPI *TEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*) = nullptr;
  HRESULT (WINAPI *CreateTrackedSample)(IMFTrackedSampleT**) = nullptr;
} gMF;
static CRITICAL_SECTION gMFLock;
static bool mfReady(){
  EnterCriticalSection(&gMFLock);
  if (!gMF.tried){
    gMF.tried = true;
    HMODULE h = LoadLibraryW(L"mfplat.dll");
    if (h){
      gMF.Startup = (decltype(gMF.Startup))GetProcAddress(h, "MFStartup");
      gMF.CreateMediaType = (decltype(gMF.CreateMediaType))GetProcAddress(h, "MFCreateMediaType");
      gMF.CreateSample = (decltype(gMF.CreateSample))GetProcAddress(h, "MFCreateSample");
      gMF.CreateMemoryBuffer = (decltype(gMF.CreateMemoryBuffer))GetProcAddress(h, "MFCreateMemoryBuffer");
      gMF.CreateDXGIDeviceManager = (decltype(gMF.CreateDXGIDeviceManager))GetProcAddress(h, "MFCreateDXGIDeviceManager");
      gMF.CreateDXGISurfaceBuffer = (decltype(gMF.CreateDXGISurfaceBuffer))GetProcAddress(h, "MFCreateDXGISurfaceBuffer");
      gMF.TEnumEx = (decltype(gMF.TEnumEx))GetProcAddress(h, "MFTEnumEx");
      gMF.CreateTrackedSample = (decltype(gMF.CreateTrackedSample))GetProcAddress(h, "MFCreateTrackedSample");
    }
    gMF.ok = gMF.Startup && gMF.CreateMediaType && gMF.CreateSample && gMF.CreateMemoryBuffer && gMF.CreateDXGIDeviceManager &&
             gMF.CreateDXGISurfaceBuffer && gMF.TEnumEx && SUCCEEDED(gMF.Startup(MF_VERSION, MFSTARTUP_LITE));
    logf(gMF.ok ? "Motor de vídeo: Media Foundation pronto" : "Motor de vídeo: este Windows não tem Media Foundation (motor indisponível)");
  }
  LeaveCriticalSection(&gMFLock);
  return gMF.ok;
}
static HRESULT mtSize(IMFAttributes* t, const GUID& k, UINT32 a, UINT32 b){ return t->SetUINT64(k, ((UINT64)a << 32) | b); }
static void mtGetSize(IMFAttributes* t, const GUID& k, UINT32* a, UINT32* b){ UINT64 v = 0; if (SUCCEEDED(t->GetUINT64(k, &v))){ *a = (UINT32)(v >> 32); *b = (UINT32)v; } }
static HRESULT capiSet(ICodecAPI* api, const GUID& k, UINT32 v){
  if (!api) return E_POINTER;
  VARIANT var; memset(&var, 0, sizeof var); var.vt = VT_UI4; var.ulVal = v;
  return api->SetValue(&k, &var);
}
static std::string actName(IMFActivate* a){
  WCHAR* w = nullptr; UINT32 n = 0; std::string s;
  if (SUCCEEDED(a->GetAllocatedString(kMFT_FRIENDLY_NAME, &w, &n)) && w){ s = utf8(w); CoTaskMemFree(w); }
  return s;
}
static bool luidOfDevice(ID3D11Device* dev, LUID* out){
  IDXGIDevice* dx = nullptr; IDXGIAdapter* ad = nullptr; bool ok = false;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && SUCCEEDED(dx->GetAdapter(&ad))){ DXGI_ADAPTER_DESC d; if (SUCCEEDED(ad->GetDesc(&d))){ *out = d.AdapterLuid; ok = true; } }
  rel(ad); rel(dx);
  return ok;
}
// Lista os componentes (codificador/decodificador) que aceitam in → out; prefere o da mesma placa (LUID).
static IMFTransform* mftPick(const GUID& cat, UINT32 flags, DWORD inFcc, DWORD outFcc, const LUID* luid, std::string* name, std::string* err){
  MFT_REGISTER_TYPE_INFO in{kMFMediaType_Video, mfFourcc(inFcc)}, out{kMFMediaType_Video, mfFourcc(outFcc)};
  IMFActivate** acts = nullptr; UINT32 n = 0;
  HRESULT hr = gMF.TEnumEx(cat, flags, &in, &out, &acts, &n);
  if (FAILED(hr) || !n){ if (err) *err = FAILED(hr) ? "não consegui listar (" + hrText(hr) + ")" : "nenhum disponível"; if (acts) CoTaskMemFree(acts); return nullptr; }
  int pick = -1; bool anyLuid = false;
  for (UINT32 i = 0; i < n && luid; i++){
    LUID l{}; UINT32 sz = 0;
    if (SUCCEEDED(acts[i]->GetBlob(kMFT_ENUM_ADAPTER_LUID, (UINT8*)&l, sizeof l, &sz)) && sz == sizeof l){
      anyLuid = true;
      if (l.LowPart == luid->LowPart && l.HighPart == luid->HighPart && pick < 0) pick = (int)i;
    }
  }
  if (pick < 0 && anyLuid && luid){ if (err) *err = "a placa em uso não tem esse codificador"; }
  IMFTransform* t = nullptr;
  if (pick >= 0 || !anyLuid || !luid){
    if (pick < 0) pick = 0;
    hr = acts[pick]->ActivateObject(__uuidof(IMFTransform), (void**)&t);
    if (SUCCEEDED(hr) && t){ if (name) *name = actName(acts[pick]); }
    else if (err) *err = "não consegui abrir (" + hrText(hr) + ")";
  }
  for (UINT32 i = 0; i < n; i++) acts[i]->Release();
  CoTaskMemFree(acts);
  return t;
}
static int mftCount(const GUID& cat, UINT32 flags, DWORD inFcc, DWORD outFcc, std::string* names){
  MFT_REGISTER_TYPE_INFO in{kMFMediaType_Video, mfFourcc(inFcc)}, out{kMFMediaType_Video, mfFourcc(outFcc)};
  IMFActivate** acts = nullptr; UINT32 n = 0;
  if (FAILED(gMF.TEnumEx(cat, flags, &in, &out, &acts, &n))) return 0;
  for (UINT32 i = 0; i < n; i++){ if (names){ if (!names->empty()) *names += ", "; *names += actName(acts[i]); } acts[i]->Release(); }
  if (acts) CoTaskMemFree(acts);
  return (int)n;
}

/* ---------- avisa quando o codificador solta a imagem (para reaproveitar a mesma textura) ---------- */
// As marcas de "imagem em uso" vivem fora da sessão, com contagem de referência: o codificador pode soltar a última
// imagem DEPOIS que a sessão já foi fechada (driver da GTX 1050 Ti faz isso). Antes a marca ficava dentro da sessão e o
// aviso escrevia em memória já liberada (Telão Audio caía com 0xC0000005 em ntdll ao trocar de codificação).
struct SlotFlags {
  volatile LONG ref = 1; volatile LONG busy[8] = {};
  void add(){ InterlockedIncrement(&ref); }
  void drop(){ if (!InterlockedDecrement(&ref)) delete this; }
};
class SlotDone : public IMFAsyncCallback {
  volatile LONG ref = 1; SlotFlags* fl; int slot;
public:
  SlotDone(SlotFlags* f, int i) : fl(f), slot(i) { fl->add(); }
  virtual ~SlotDone(){ fl->drop(); }
  STDMETHODIMP QueryInterface(REFIID iid, void** pp) override {
    if (IsEqualGUID(iid, kIID_IUnknown) || IsEqualGUID(iid, __uuidof(IMFAsyncCallback))){ *pp = (IMFAsyncCallback*)this; AddRef(); return S_OK; }
    *pp = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
  STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
  STDMETHODIMP Invoke(IMFAsyncResult*) override { InterlockedExchange(&fl->busy[slot], 0); return S_OK; }
};

/* ---------- codificação (quem transmite) ---------- */
struct EncSes {
  uint32_t id = 0; int boxW = 1280, boxH = 720, fps = 60, kbps = 4000, profile = 1; bool hevc = false;
  int ow = 0, oh = 0;
  IMFTransform* mft = nullptr; IMFMediaEventGenerator* ev = nullptr; ICodecAPI* api = nullptr;
  bool async = false, provides = true; DWORD outCb = 0; int needInput = 0;
  std::vector<ID3D11Texture2D*> ring; SlotFlags* flags = nullptr; int slot = -1, lastSlot = -1;
  ID3D11VideoProcessorEnumerator* vpe = nullptr; ID3D11VideoProcessor* vp = nullptr; ID3D11VideoProcessorInputView* vin = nullptr;
  std::vector<ID3D11VideoProcessorOutputView*> views; int vpInW = 0, vpInH = 0;
  std::vector<uint8_t> seqHdr; bool key = true; int newKbps = 0;
  uint32_t seq = 0; Pacer pacer; double lastIn = 0;
  std::string enc; int errors = 0;
  NvSes* nv = nullptr; bool useNv = true; double lastIdr = 0, qpSum = 0; int qpN = 0, slow = 0; std::string nvWhy;   // NVENC direto (placas NVIDIA)
  int nIn = 0, nOut = 0, nBusy = 0, nKey = 0; double bytes = 0; std::vector<double> lat; std::vector<std::pair<LONGLONG, double>> inT;
};
static void sesFreeVP(EncSes& s){ for (auto*& v : s.views) rel(v); s.views.clear(); rel(s.vin); rel(s.vp); rel(s.vpe); s.vpInW = s.vpInH = 0; }
static void sesFree(EncSes& s){
  if (s.nv){ nvFree(s.nv); s.nv = nullptr; }
  if (s.mft){
    s.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); s.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    // codificador assíncrono: desligar antes de soltar (sem isso as linhas internas dele seguem com eventos pendentes)
    IMFShutdownT* sh = nullptr;
    if (s.async && SUCCEEDED(s.mft->QueryInterface(kIID_IMFShutdown, (void**)&sh)) && sh){ sh->Shutdown(); sh->Release(); }
  }
  sesFreeVP(s);
  for (auto*& t : s.ring) rel(t);
  s.ring.clear();
  rel(s.api); rel(s.ev); rel(s.mft);
  if (s.flags){ s.flags->drop(); s.flags = nullptr; }
}
static HRESULT sesVP(EncSes& s, VRes& r, int inW, int inH, int fps){
  sesFreeVP(s);
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputWidth = inW; cd.InputHeight = inH; cd.OutputWidth = s.ow; cd.OutputHeight = s.oh;
  cd.InputFrameRate = {(UINT)fps, 1}; cd.OutputFrameRate = {(UINT)fps, 1};
  cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
  HRESULT hr = r.vdev->CreateVideoProcessorEnumerator(&cd, &s.vpe);
  if (SUCCEEDED(hr)) hr = r.vdev->CreateVideoProcessor(s.vpe, 0, &s.vp);
  if (FAILED(hr)) return hr;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  hr = r.vdev->CreateVideoProcessorInputView(r.inCopy, s.vpe, &iv, &s.vin);
  if (FAILED(hr)) return hr;
  for (auto* t : s.ring){
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorOutputView* v = nullptr;
    hr = r.vdev->CreateVideoProcessorOutputView(t, s.vpe, &ovd, &v);
    if (FAILED(hr)) return hr;
    s.views.push_back(v);
  }
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {}; in.RGB_Range = 0;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {}; out.RGB_Range = 0; out.YCbCr_Matrix = 1; out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  r.vctx->VideoProcessorSetStreamColorSpace(s.vp, 0, &in);
  r.vctx->VideoProcessorSetOutputColorSpace(s.vp, &out);
  r.vctx->VideoProcessorSetStreamFrameFormat(s.vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  r.vctx->VideoProcessorSetStreamAutoProcessingMode(s.vp, 0, FALSE);
  D3D11_VIDEO_COLOR black = {}; black.YCbCr.Y = 16.0f / 255; black.YCbCr.Cb = 0.5f; black.YCbCr.Cr = 0.5f; black.YCbCr.A = 1.0f;
  r.vctx->VideoProcessorSetOutputBackgroundColor(s.vp, TRUE, &black);
  RECT all = {0, 0, s.ow, s.oh};
  r.vctx->VideoProcessorSetOutputTargetRect(s.vp, TRUE, &all);
  s.vpInW = inW; s.vpInH = inH;
  return S_OK;
}
// Abre o codificador da placa para uma sessão (um tamanho/ritmo/formato). Devolve "" se deu certo, ou o motivo.
static std::string sesOpen(EncSes& s, VRes& r, IMFDXGIDeviceManager* mgr, const LUID* luid, int inW, int inH, int contW, int contH){
  if (!s.flags) s.flags = new SlotFlags();
  fitSize(contW, contH, s.boxW, s.boxH, &s.ow, &s.oh);
  // largura múltipla de 16 (e altura de 8 no H.265): janela com tamanho quebrado (ex.: 1282x720, a cena de teste no PC do
  // escapriem) derrubava o Telão Audio ao abrir o codificador H.265 de uma GTX 1050 Ti. A diferença some na redução.
  s.ow = std::max(64, s.ow & ~15); s.oh = std::max(64, s.hevc ? s.oh & ~7 : s.oh & ~1);
  // NVIDIA: NVENC direto (modo plateia). Qualquer falha → segue pela Media Foundation, como antes.
  if (s.useNv && r.vendorId == 0x10DE){
    std::string why;
    if (!nvLoad(&why)) s.nvWhy = why;
    else {
      bool ringOk = true;
      for (int i = 0; i < 4 && ringOk; i++){
        ID3D11Texture2D* t = makeTex(r.dev, s.ow, s.oh, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | 0x400 /*VIDEO_ENCODER*/, 0, 0);
        if (!t) t = makeTex(r.dev, s.ow, s.oh, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0);
        if (!t) ringOk = false; else s.ring.push_back(t);
      }
      if (ringOk && SUCCEEDED(sesVP(s, r, inW, inH, s.fps))){
        std::string e = nvOpen(&s.nv, r.dev, s.ring, s.ow, s.oh, s.fps, s.kbps, s.hevc, s.profile);
        if (e.empty()){ s.enc = s.nv->desc; s.pacer.setFps(s.fps); return ""; }
        s.nvWhy = e;
      } else s.nvWhy = "imagem ou processador de vídeo para o NVENC";
      logf("Motor: NVENC direto não abriu (%s); usando a Media Foundation", s.nvWhy.c_str());
      sesFreeVP(s); for (auto*& t : s.ring) rel(t); s.ring.clear();
    }
  }
  std::string err;
  s.mft = mftPick(kMFT_CAT_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, FCC_NV12, s.hevc ? FCC_HEVC : FCC_H264, luid, &s.enc, &err);
  if (!s.mft) return std::string(s.hevc ? "codificador H.265 da placa: " : "codificador H.264 da placa: ") + err;
  IMFAttributes* at = nullptr;
  if (SUCCEEDED(s.mft->GetAttributes(&at)) && at){
    UINT32 a = 0;
    if (SUCCEEDED(at->GetUINT32(kMF_TRANSFORM_ASYNC, &a)) && a){ s.async = true; at->SetUINT32(kMF_TRANSFORM_ASYNC_UNLOCK, TRUE); }
    at->SetUINT32(kMF_LOW_LATENCY, TRUE);
    at->Release();
  }
  if (s.async && FAILED(s.mft->QueryInterface(__uuidof(IMFMediaEventGenerator), (void**)&s.ev))) return "o codificador não avisa quando está pronto";
  HRESULT hr = s.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr);
  if (FAILED(hr)) return "o codificador não aceitou trabalhar direto na placa (" + hrText(hr) + ")";
  s.mft->QueryInterface(__uuidof(ICodecAPI), (void**)&s.api);
  // baixo atraso: taxa constante, sem quadros B, quadro completo quando alguém pede (ou a cada 4 s)
  capiSet(s.api, kCAPI_RateControlMode, 0 /*CBR*/);
  capiSet(s.api, kCAPI_MeanBitRate, (UINT32)s.kbps * 1000);
  capiSet(s.api, kCAPI_LowLatency, 1);
  capiSet(s.api, kCAPI_BPictureCount, 0);
  capiSet(s.api, kCAPI_GOPSize, (UINT32)s.fps * 4);   // quadro completo a cada 4 s: se algo se perder, a imagem se corrige sozinha
  IMFMediaType* ot = nullptr;
  if (FAILED(gMF.CreateMediaType(&ot))) return "Media Foundation sem memória";
  ot->SetGUID(kMF_MT_MAJOR_TYPE, kMFMediaType_Video);
  ot->SetGUID(kMF_MT_SUBTYPE, mfFourcc(s.hevc ? FCC_HEVC : FCC_H264));
  ot->SetUINT32(kMF_MT_AVG_BITRATE, (UINT32)s.kbps * 1000);
  mtSize(ot, kMF_MT_FRAME_SIZE, s.ow, s.oh);
  mtSize(ot, kMF_MT_FRAME_RATE, s.fps, 1);
  mtSize(ot, kMF_MT_PIXEL_ASPECT, 1, 1);
  ot->SetUINT32(kMF_MT_INTERLACE_MODE, 2 /*progressivo*/);
  // H.264: 66 base (restrita), 77 main, 100 high; H.265: 1 = Main
  ot->SetUINT32(kMF_MT_MPEG2_PROFILE, s.hevc ? 1 : (s.profile == 0 ? 66 : s.profile == 2 ? 100 : 77));
  hr = s.mft->SetOutputType(0, ot, 0);
  ot->Release();
  if (FAILED(hr)) return "o codificador recusou " + std::to_string(s.ow) + "x" + std::to_string(s.oh) + " a " + std::to_string(s.fps) + " fps (" + hrText(hr) + ")";
  bool inOk = false;
  for (DWORD i = 0; i < 32 && !inOk; i++){
    IMFMediaType* it = nullptr;
    if (FAILED(s.mft->GetInputAvailableType(0, i, &it)) || !it) break;
    GUID sub{}; it->GetGUID(kMF_MT_SUBTYPE, &sub);
    if (IsEqualGUID(sub, mfFourcc(FCC_NV12))){
      mtSize(it, kMF_MT_FRAME_SIZE, s.ow, s.oh); mtSize(it, kMF_MT_FRAME_RATE, s.fps, 1); mtSize(it, kMF_MT_PIXEL_ASPECT, 1, 1);
      it->SetUINT32(kMF_MT_INTERLACE_MODE, 2);
      inOk = SUCCEEDED(s.mft->SetInputType(0, it, 0));
    }
    it->Release();
  }
  if (!inOk){
    IMFMediaType* it = nullptr; gMF.CreateMediaType(&it);
    it->SetGUID(kMF_MT_MAJOR_TYPE, kMFMediaType_Video); it->SetGUID(kMF_MT_SUBTYPE, mfFourcc(FCC_NV12));
    mtSize(it, kMF_MT_FRAME_SIZE, s.ow, s.oh); mtSize(it, kMF_MT_FRAME_RATE, s.fps, 1); mtSize(it, kMF_MT_PIXEL_ASPECT, 1, 1);
    it->SetUINT32(kMF_MT_INTERLACE_MODE, 2);
    hr = s.mft->SetInputType(0, it, 0); it->Release();
    if (FAILED(hr)) return "o codificador não aceitou a imagem da placa (NV12, " + hrText(hr) + ")";
  }
  MFT_OUTPUT_STREAM_INFO si{};
  if (SUCCEEDED(s.mft->GetOutputStreamInfo(0, &si))){ s.provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0; s.outCb = si.cbSize; }
  s.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  s.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  s.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  // imagens em rodízio onde o processador de vídeo escreve e o codificador lê
  const int K = 6;   // (até 8: tamanho de busy)
  for (int i = 0; i < K; i++){
    ID3D11Texture2D* t = makeTex(r.dev, s.ow, s.oh, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | 0x400 /*VIDEO_ENCODER*/, 0, 0);
    if (!t) t = makeTex(r.dev, s.ow, s.oh, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0);
    if (!t) return "a placa não deixou criar a imagem para o codificador";
    s.ring.push_back(t);
  }
  hr = sesVP(s, r, inW, inH, s.fps);
  if (FAILED(hr)) return "o processador de vídeo da placa recusou o tamanho (" + hrText(hr) + ")";
  s.pacer.setFps(s.fps);
  return "";
}
static void sesSend(EncSes& s, Conn* conn, IMFSample* smp){
  IMFMediaBuffer* buf = nullptr; BYTE* p = nullptr; DWORD len = 0;
  if (FAILED(smp->ConvertToContiguousBuffer(&buf)) || !buf) return;
  if (FAILED(buf->Lock(&p, nullptr, &len))){ buf->Release(); return; }
  UINT32 clean = 0; smp->GetUINT32(kMFSample_CleanPoint, &clean);
  LONGLONG t = 0; smp->GetSampleTime(&t);
  AuInfo a = auInfo(p, len, s.hevc);
  bool key = clean || a.key;
  if (key && s.seqHdr.empty()){
    IMFMediaType* ct = nullptr; UINT32 sz = 0;
    if (SUCCEEDED(s.mft->GetOutputCurrentType(0, &ct)) && ct){
      if (SUCCEEDED(ct->GetBlobSize(kMF_MT_MPEG_SEQ_HEADER, &sz)) && sz > 0 && sz < 4096){ s.seqHdr.resize(sz); ct->GetBlob(kMF_MT_MPEG_SEQ_HEADER, s.seqHdr.data(), sz, nullptr); }
      ct->Release();
    }
  }
  std::vector<uint8_t> pkt(24);
  engHeader(pkt.data(), "TEF1", s.id, key ? 1 : 0, ++s.seq, (uint64_t)(t / 10));
  if (key && !a.params && s.seqHdr.size() >= 4 && s.seqHdr[0] == 0 && s.seqHdr[1] == 0) pkt.insert(pkt.end(), s.seqHdr.begin(), s.seqHdr.end());
  pkt.insert(pkt.end(), p, p + len);
  buf->Unlock(); buf->Release();
  for (size_t i = 0; i < s.inT.size(); i++) if (s.inT[i].first == t){ s.lat.push_back(nowMs() - s.inT[i].second); s.inT.erase(s.inT.begin(), s.inT.begin() + i + 1); break; }
  s.nOut++; s.bytes += len; if (key) s.nKey++;
  wsSend(conn, 2, pkt.data(), pkt.size());
}
static void sesOutput(EncSes& s, Conn* conn){
  for (int guard = 0; guard < 8; guard++){
    MFT_OUTPUT_DATA_BUFFER ob{}; DWORD st = 0; IMFSample* own = nullptr;
    if (!s.provides){
      IMFMediaBuffer* b = nullptr;
      if (FAILED(gMF.CreateSample(&own)) || FAILED(gMF.CreateMemoryBuffer(std::max<DWORD>(s.outCb, 4 << 20), &b))){ rel(own); rel(b); return; }
      own->AddBuffer(b); b->Release(); ob.pSample = own;
    }
    HRESULT hr = s.mft->ProcessOutput(0, 1, &ob, &st);
    if (ob.pEvents) ob.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE){
      IMFMediaType* t = nullptr;
      if (SUCCEEDED(s.mft->GetOutputAvailableType(0, 0, &t)) && t){ s.mft->SetOutputType(0, t, 0); t->Release(); }
      s.seqHdr.clear(); rel(own);
      continue;
    }
    if (FAILED(hr)){ rel(own); return; }   // MF_E_TRANSFORM_NEED_MORE_INPUT: nada pronto ainda
    if (ob.pSample){ sesSend(s, conn, ob.pSample); if (ob.pSample != own) ob.pSample->Release(); }
    rel(own);
    if (s.async) return;   // assíncrono: um quadro por aviso
  }
}
// Entrega a imagem do rodízio (slot) ao codificador.
static bool sesInput(EncSes& s, Conn* conn, int slot, double t0){
  if (s.nv){
    double now = nowMs();
    bool idr = s.key && now - s.lastIdr >= 1000;   // quadro completo: no máximo 1 por segundo (pedidos em rajada viram um só)
    if (s.newKbps){ if (nvRate(s.nv, s.newKbps)) s.kbps = s.newKbps; s.newKbps = 0; }
    LONGLONG t100 = (LONGLONG)((now - t0) * 10000);
    std::vector<uint8_t> out; bool key = false; int qp = 0; std::string err;
    if (!nvEncode(s.nv, slot, (uint64_t)t100, idr, &out, &key, &qp, &err)){ if (s.errors++ < 3) logf("Motor: NVENC: %s", err.c_str()); return false; }
    if (idr){ s.key = false; s.lastIdr = now; }
    s.nIn++; s.lastIn = now; s.lastSlot = slot;
    if (!out.empty()){
      std::vector<uint8_t> pkt(24);
      engHeader(pkt.data(), "TEF1", s.id, key ? 1 : 0, ++s.seq, (uint64_t)(t100 / 10));
      pkt.insert(pkt.end(), out.begin(), out.end());
      s.lat.push_back(nowMs() - now); s.nOut++; s.bytes += out.size(); if (key) s.nKey++;
      if (qp > 0){ s.qpSum += qp; s.qpN++; }
      wsSend(conn, 2, pkt.data(), pkt.size());
    }
    return true;
  }
  if (s.async && s.needInput <= 0){ s.nBusy++; return false; }
  if (s.key){ capiSet(s.api, kCAPI_ForceKeyFrame, 1); s.key = false; }
  if (s.newKbps){ capiSet(s.api, kCAPI_MeanBitRate, (UINT32)s.newKbps * 1000); s.kbps = s.newKbps; s.newKbps = 0; }
  IMFMediaBuffer* buf = nullptr; IMFSample* smp = nullptr; IMFTrackedSampleT* tr = nullptr;
  if (FAILED(gMF.CreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), s.ring[slot], 0, FALSE, &buf)) || !buf) return false;
  DWORD mx = 0; buf->GetMaxLength(&mx); buf->SetCurrentLength(mx);
  if (gMF.CreateTrackedSample && SUCCEEDED(gMF.CreateTrackedSample(&tr)) && tr){
    tr->QueryInterface(__uuidof(IMFSample), (void**)&smp);
    InterlockedExchange(&s.flags->busy[slot], 1);
    SlotDone* cb = new SlotDone(s.flags, slot);
    tr->SetAllocator(cb, nullptr);
    cb->Release(); tr->Release();
  } else gMF.CreateSample(&smp);
  if (!smp){ buf->Release(); InterlockedExchange(&s.flags->busy[slot], 0); return false; }
  smp->AddBuffer(buf); buf->Release();
  LONGLONG t100 = (LONGLONG)((nowMs() - t0) * 10000);
  smp->SetSampleTime(t100); smp->SetSampleDuration(10000000LL / s.fps);
  HRESULT hr = s.mft->ProcessInput(0, smp, 0);
  smp->Release();   // o codificador segura a dele; quando soltar, o rodízio libera a imagem
  if (hr == MF_E_NOTACCEPTING){ s.nBusy++; return false; }
  if (FAILED(hr)){ if (s.errors++ < 3) logf("Motor: codificador recusou o quadro (%s)", hrText(hr).c_str()); return false; }
  if (s.async) s.needInput--;
  s.inT.push_back({t100, nowMs()}); if (s.inT.size() > 64) s.inT.erase(s.inT.begin());
  s.nIn++; s.lastIn = nowMs(); s.lastSlot = slot;
  if (!s.async) sesOutput(s, conn);
  return true;
}
static void sesPump(EncSes& s, Conn* conn){
  if (s.nv || !s.async || !s.ev) return;
  for (int guard = 0; guard < 64; guard++){
    IMFMediaEvent* e = nullptr;
    if (FAILED(s.ev->GetEvent(MF_EVENT_FLAG_NO_WAIT, &e)) || !e) break;
    MediaEventType t = MEUnknown; e->GetType(&t); e->Release();
    if (t == METransformNeedInput) s.needInput++;
    else if (t == METransformHaveOutput) sesOutput(s, conn);
  }
}
static int sesFreeSlot(EncSes& s){
  if (s.nv) return s.ring.empty() ? -1 : (s.slot + 1) % (int)s.ring.size();   // NVENC síncrono: a imagem já foi lida
  for (int k = 1; k <= (int)s.ring.size(); k++){ int i = (s.slot + k) % (int)s.ring.size(); if (!s.flags->busy[i]) return i; }
  return -1;
}

struct EngCmd { int kind = 0; EncSes* ses = nullptr; uint32_t id = 0; int kbps = 0; };   // 1 abrir, 2 fechar, 3 quadro completo, 4 taxa
struct Eng {
  Conn* conn = nullptr; HANDLE thread = nullptr; volatile LONG stop = 0;
  HWND hwnd = nullptr; HMONITOR mon = nullptr; bool monitor = false, ddAllowed = false; UINT vendor = 0, device = 0;
  CRITICAL_SECTION lock; std::vector<EngCmd> cmds;
};
static CRITICAL_SECTION gEngLock;
static Eng* gEng = nullptr;
static void engSend(Eng* e, const std::string& j){ if (e->conn && e->conn->alive) wsText(e->conn, j); }
#define EFAIL(msg, hr) do{ engSend(e, "{\"type\":\"ecaperr\",\"message\":\"" + jsonEsc(std::string(msg) + " (" + hrText(hr) + ")") + "\"}"); logf("Motor: %s (%s)", msg, hrText(hr).c_str()); goto done; }while(0)

/* Tela cheia em primeiro plano: a captura de janela (WGC) do Windows 10 para de receber quadros quando o jogo desenha
   direto na tela (escapriem: 9 fps, mín 4). Enquanto a janela do jogo é a janela da frente E cobre o monitor inteiro, o
   motor pega a imagem do monitor (Desktop Duplication, como Parsec e Sunshine); quando deixa de ser (alt+tab, janela
   menor), volta na hora para a captura da janela: o que estiver em volta nunca vai para a transmissão. */
static bool ddWanted(HWND target){
  if (!target || !IsWindow(target) || IsIconic(target)) return false;
  HWND fg = GetForegroundWindow();
  if (!fg || GetAncestor(fg, GA_ROOT) != GetAncestor(target, GA_ROOT)) return false;
  RECT wr; if (!GetWindowRect(target, &wr)) return false;
  MONITORINFO mi = {}; mi.cbSize = sizeof mi;
  if (!GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi)) return false;
  const RECT& m = mi.rcMonitor;
  return wr.left <= m.left + 2 && wr.top <= m.top + 2 && wr.right >= m.right - 2 && wr.bottom >= m.bottom - 2;
}
/* Só a área interna da janela: a captura de janela do Windows inclui barra de título e, no Windows 10, as bordas
   invisíveis de redimensionar (a cena de teste do escapriem chegava 1296x728 em vez de 1280x720, com faixas pretas e a
   imagem fora de escala). Devolve o retângulo da área interna dentro do quadro capturado (fw×fh), ou false. */
typedef HRESULT (WINAPI *PFN_DwmAttr)(HWND, DWORD, void*, DWORD);
static PFN_DwmAttr pDwmAttr = nullptr; static bool dwmTried = false;
static bool clientCrop(HWND h, int fw, int fh, RECT* out){
  if (!dwmTried){ dwmTried = true; HMODULE m = LoadLibraryW(L"dwmapi.dll"); if (m) pDwmAttr = (PFN_DwmAttr)GetProcAddress(m, "DwmGetWindowAttribute"); }
  RECT wr, cr; if (!h || IsIconic(h) || !GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return false;
  POINT p0 = {0, 0}; if (!ClientToScreen(h, &p0)) return false;
  RECT efb = wr; if (pDwmAttr) pDwmAttr(h, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &efb, sizeof efb);
  auto fits = [&](const RECT& b){ return std::abs((int)(b.right - b.left) - fw) <= 2 && std::abs((int)(b.bottom - b.top) - fh) <= 2; };
  const RECT* base = fits(efb) ? &efb : fits(wr) ? &wr : nullptr;
  if (!base) return false;   // não sei onde a área interna cai no quadro: captura inteira, como antes
  int x = std::max(0, (int)(p0.x - base->left)), y = std::max(0, (int)(p0.y - base->top));
  int w = std::min((int)cr.right, fw - x), hh = std::min((int)cr.bottom, fh - y);
  if (w < 64 || hh < 64) return false;
  if (x == 0 && y == 0 && w >= fw - 1 && hh >= fh - 1) return false;   // já é só a área interna
  *out = {x, y, x + w, y + hh};
  return true;
}
static IDXGIOutputDuplication* ddOpen(VRes& r, HMONITOR mon, std::string* err){
  IDXGIDevice* dx = nullptr; IDXGIAdapter* ad = nullptr; IDXGIOutputDuplication* dup = nullptr;
  if (FAILED(r.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) || FAILED(dx->GetAdapter(&ad))){ rel(dx); *err = "placa"; return nullptr; }
  bool found = false;
  for (UINT i = 0;; i++){
    IDXGIOutput* out = nullptr;
    if (ad->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
    DXGI_OUTPUT_DESC od; out->GetDesc(&od);
    if (od.Monitor == mon){
      found = true;
      IDXGIOutput1* o1 = nullptr;
      if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1))){
        HRESULT hr = o1->DuplicateOutput(r.dev, &dup);
        if (FAILED(hr)) *err = "o Windows recusou a captura do monitor (" + hrText(hr) + ")";
        o1->Release();
      }
    }
    out->Release();
    if (found) break;
  }
  if (!found) *err = "o monitor do jogo está ligado em outra placa de vídeo";
  rel(ad); rel(dx);
  return dup;
}
static void engRun(Eng* e){
  VRes r; IMFDXGIDeviceManager* mgr = nullptr; UINT tok = 0; LUID luid{}; bool haveLuid = false;
  std::vector<EncSes*> ses; SizeInt32 sz{}, poolSz{};
  double t0 = nowMs(), lastCheck = t0, statT = t0; std::string endReason;
  bool border = true;
  IDirect3D11CaptureFramePoolStatics2* st2 = nullptr;
  if (!videoApis()){ engSend(e, "{\"type\":\"ecaperr\",\"message\":\"Este Windows não tem a captura moderna (precisa do Windows 10 1903 ou mais novo)\"}"); return; }
  if (!mfReady()){ engSend(e, "{\"type\":\"ecaperr\",\"message\":\"Este Windows não tem Media Foundation\"}"); return; }
  pRoInitialize(1);
  {
    HRESULT hr = makeDevice(r, e->vendor, e->device);
    if (FAILED(hr)) EFAIL("não consegui usar a placa de vídeo", hr);
    haveLuid = luidOfDevice(r.dev, &luid);
    hr = gMF.CreateDXGIDeviceManager(&tok, &mgr);
    if (SUCCEEDED(hr)) hr = mgr->ResetDevice(r.dev, tok);
    if (FAILED(hr)) EFAIL("não consegui ligar a placa ao Media Foundation", hr);
    auto interop = (IGraphicsCaptureItemInterop*)activationFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", IID_IGraphicsCaptureItemInterop);
    if (!interop) EFAIL("a captura do Windows não está disponível", E_NOINTERFACE);
    hr = e->monitor ? interop->CreateForMonitor(e->mon, IID_IGraphicsCaptureItem, (void**)&r.item)
                    : interop->CreateForWindow(e->hwnd, IID_IGraphicsCaptureItem, (void**)&r.item);
    interop->Release();
    if (FAILED(hr) || !r.item) EFAIL(e->monitor ? "o Windows não deixou capturar esse monitor" : "o Windows não deixou capturar essa janela", hr);
    r.item->get_Size(&sz);
    if (sz.Width < 16 || sz.Height < 16) EFAIL("a janela está minimizada ou sem tamanho", E_FAIL);
    st2 = (IDirect3D11CaptureFramePoolStatics2*)activationFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", IID_IFramePoolStatics2);
    if (!st2) EFAIL("a captura do Windows não está disponível", E_NOINTERFACE);
    poolSz = sz;
    hr = st2->CreateFreeThreaded(r.rtDev, PIXEL_BGRA8, 3, poolSz, &r.pool);
    st2->Release(); st2 = nullptr;
    if (FAILED(hr)) EFAIL("não consegui preparar a captura", hr);
    hr = r.pool->CreateCaptureSession(r.item, &r.ses);
    if (FAILED(hr)) EFAIL("não consegui abrir a sessão de captura", hr);
    { IGraphicsCaptureSession2* s2 = nullptr; if (SUCCEEDED(r.ses->QueryInterface(IID_ISession2, (void**)&s2))){ s2->put_IsCursorCaptureEnabled(true); s2->Release(); } }
    { IGraphicsCaptureSession3* s3 = nullptr; if (SUCCEEDED(r.ses->QueryInterface(IID_ISession3, (void**)&s3))){ border = FAILED(s3->put_IsBorderRequired(false)); s3->Release(); } }
    { IGraphicsCaptureSession5* s5 = nullptr; if (SUCCEEDED(r.ses->QueryInterface(IID_ISession5, (void**)&s5))){ s5->put_MinUpdateInterval(10000000 / 240); s5->Release(); } }
    hr = r.ses->StartCapture();
    if (FAILED(hr)) EFAIL("o Windows recusou iniciar a captura", hr);
    r.inCopy = makeTex(r.dev, sz.Width, sz.Height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0);
    if (!r.inCopy) EFAIL("a placa não deixou criar a imagem de entrada", E_OUTOFMEMORY);
    r.inW = sz.Width; r.inH = sz.Height;
    r.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!r.timer) r.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  engSend(e, "{\"type\":\"ecapok\",\"sw\":" + std::to_string(sz.Width) + ",\"sh\":" + std::to_string(sz.Height) + ",\"adapter\":\"" + jsonEsc(r.adapter) +
             "\",\"border\":" + (border ? "true" : "false") + "}");
  logf("Motor: captura pronta %dx%d na placa %s", sz.Width, sz.Height, r.adapter.c_str());
  {
    int cw = sz.Width, ch = sz.Height;   // área útil da imagem capturada
    bool haveImg = false;
    IDXGIOutputDuplication* dup = nullptr; double ddRetry = 0, ddCheck = 0; bool ddWant = false, ddSaidErr = false; std::string ddMode = "wgc";
    RECT crop{}; bool haveCrop = false; double cropCheck = 0; int cropFw = 0, cropFh = 0;
    while (!e->stop && e->conn->alive){
      { // área interna da janela (sem barra de título e bordas): vale para o tamanho das codificações e para o recorte
        double tnow = nowMs();
        if (!dup && !e->monitor && (tnow - cropCheck > 500 || cropFw != cw || cropFh != ch)){
          cropCheck = tnow; cropFw = cw; cropFh = ch;
          RECT c2; bool hc = clientCrop(e->hwnd, cw, ch, &c2);
          if (hc != haveCrop || (hc && memcmp(&c2, &crop, sizeof crop))){ haveCrop = hc; crop = c2; if (hc) logf("Motor: só a área interna da janela (%ld,%ld %ldx%ld de %dx%d)", crop.left, crop.top, crop.right - crop.left, crop.bottom - crop.top, cw, ch); }
        }
      }
      // comandos da página (abrir/fechar sessão, quadro completo, taxa)
      std::vector<EngCmd> cmds;
      EnterCriticalSection(&e->lock); cmds.swap(e->cmds); LeaveCriticalSection(&e->lock);
      for (auto& c : cmds){
        if (c.kind == 1 && c.ses){
          for (size_t i = 0; i < ses.size(); i++) if (ses[i]->id == c.ses->id){ sesFree(*ses[i]); delete ses[i]; ses.erase(ses.begin() + i); break; }
          if (ses.size() >= 4){ engSend(e, "{\"type\":\"esesserr\",\"id\":" + std::to_string(c.ses->id) + ",\"message\":\"sessões demais\"}"); delete c.ses; continue; }
          // tamanho de partida: a área interna da janela (sem bordas), quando já se sabe; o processador de vídeo continua lendo a imagem inteira
          int effW = haveCrop && !dup ? crop.right - crop.left : r.inW, effH = haveCrop && !dup ? crop.bottom - crop.top : r.inH;
          std::string err = sesOpen(*c.ses, r, mgr, haveLuid ? &luid : nullptr, r.inW, r.inH, effW, effH);
          if (!err.empty()){
            engSend(e, "{\"type\":\"esesserr\",\"id\":" + std::to_string(c.ses->id) + ",\"message\":\"" + jsonEsc(err) + "\"}");
            logf("Motor: sessão %u não abriu: %s", c.ses->id, err.c_str());
            sesFree(*c.ses); delete c.ses;
          } else {
            engSend(e, "{\"type\":\"esessok\",\"id\":" + std::to_string(c.ses->id) + ",\"w\":" + std::to_string(c.ses->ow) + ",\"h\":" + std::to_string(c.ses->oh) +
                       ",\"fps\":" + std::to_string(c.ses->fps) + ",\"codec\":\"" + (c.ses->hevc ? "h265" : "h264") + "\",\"encoder\":\"" + jsonEsc(c.ses->enc) +
                       "\",\"async\":" + (c.ses->async || c.ses->nv ? "true" : "false") + ",\"api\":\"" + (c.ses->nv ? "nvenc" : "mf") + "\",\"nvWhy\":\"" + jsonEsc(c.ses->nvWhy) + "\"}");
            logf("Motor: sessão %u %s %dx%d a %d fps, %d kbps (%s)", c.ses->id, c.ses->hevc ? "H.265" : "H.264", c.ses->ow, c.ses->oh, c.ses->fps, c.ses->kbps, c.ses->enc.c_str());
            ses.push_back(c.ses);
          }
        } else {
          for (size_t i = 0; i < ses.size(); i++) if (ses[i]->id == c.id){
            if (c.kind == 2){ sesFree(*ses[i]); delete ses[i]; ses.erase(ses.begin() + i); }
            else if (c.kind == 3) ses[i]->key = true;
            else if (c.kind == 4 && c.kbps > 0) ses[i]->newKbps = c.kbps;
            break;
          }
        }
      }
      double now = nowMs();
      if (now - lastCheck > 500){
        lastCheck = now;
        if (!e->monitor && !IsWindow(e->hwnd)){ endReason = "a janela fechou"; break; }
        HRESULT dr = r.dev->GetDeviceRemovedReason();
        if (dr != S_OK){ endReason = "a placa de vídeo reiniciou (" + hrText(dr) + ")"; break; }
      }
      if (now - statT >= 2000){
        double secs = (now - statT) / 1000;
        for (auto* s : ses){
          double avg = 0; for (double x : s->lat) avg += x; if (!s->lat.empty()) avg /= s->lat.size();
          char b[380]; snprintf(b, sizeof b, "{\"type\":\"estat\",\"id\":%u,\"in\":%.1f,\"out\":%.1f,\"busy\":%.1f,\"kbps\":%.0f,\"lat\":%.2f,\"keys\":%d,\"target\":%d,\"qp\":%.1f,\"api\":\"%s\"}",
                                s->id, s->nIn / secs, s->nOut / secs, s->nBusy / secs, s->bytes * 8 / 1000 / secs, avg, s->nKey, s->kbps, s->qpN ? s->qpSum / s->qpN : 0.0, s->nv ? "nvenc" : "mf");
          s->qpSum = 0; s->qpN = 0;
          if (s->nv && !s->lat.empty() && avg > 1000.0 / s->fps * 0.7){
            if (++s->slow >= 2 && nvLighten(s->nv)){ s->enc = s->nv->desc; logf("Motor: NVENC sem folga (%.1f ms por quadro); 1 passada", avg); engSend(e, "{\"type\":\"esessok\",\"id\":" + std::to_string(s->id) + ",\"w\":" + std::to_string(s->ow) + ",\"h\":" + std::to_string(s->oh) + ",\"fps\":" + std::to_string(s->fps) + ",\"codec\":\"" + (s->hevc ? "h265" : "h264") + "\",\"encoder\":\"" + jsonEsc(s->enc) + "\",\"async\":true,\"api\":\"nvenc\"}"); }
          } else s->slow = 0;
          engSend(e, b);
          s->nIn = s->nOut = s->nBusy = s->nKey = 0; s->bytes = 0; s->lat.clear();
        }
        statT = now;
      }
      // tela cheia em primeiro plano → monitor (Desktop Duplication); senão, a janela (WGC)
      if (e->ddAllowed && !e->monitor && now - ddCheck >= 100){
        ddCheck = now;
        ddWant = ddWanted(e->hwnd);
        if (!ddWant && dup){ dup->Release(); dup = nullptr; }
        if (ddWant && !dup && now >= ddRetry){
          std::string err; dup = ddOpen(r, MonitorFromWindow(e->hwnd, MONITOR_DEFAULTTONEAREST), &err);
          if (!dup){ ddRetry = now + 5000; if (!ddSaidErr){ ddSaidErr = true; logf("Motor: captura do monitor indisponível: %s", err.c_str()); engSend(e, "{\"type\":\"emode\",\"mode\":\"wgc\",\"why\":\"" + jsonEsc(err) + "\"}"); } }
        }
        const char* mode = dup ? "dd" : "wgc";
        if (ddMode != mode){ ddMode = mode; logf("Motor: captura %s", dup ? "do monitor (jogo em tela cheia na frente)" : "da janela"); engSend(e, std::string("{\"type\":\"emode\",\"mode\":\"") + mode + "\"}"); }
      }
      // imagem nova da captura (fica só com a mais nova)
      IDirect3D11CaptureFrame* fr = nullptr;
      bool fresh = false;
      if (dup){
        DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource* res = nullptr;
        HRESULT hr = dup->AcquireNextFrame(0, &fi, &res);
        if (hr == DXGI_ERROR_ACCESS_LOST){ dup->Release(); dup = nullptr; ddRetry = now + 200; }
        else if (SUCCEEDED(hr)){
          ID3D11Texture2D* tex = nullptr;
          if (fi.LastPresentTime.QuadPart && res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex){
            D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
            if ((int)td.Width != r.inW || (int)td.Height != r.inH){
              rel(r.inCopy);
              r.inCopy = makeTex(r.dev, td.Width, td.Height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0);
              r.inW = td.Width; r.inH = td.Height;
              for (auto* s : ses) sesVP(*s, r, r.inW, r.inH, s->fps);
            }
            if (r.inCopy && td.Format == DXGI_FORMAT_B8G8R8A8_UNORM){ r.ctx->CopyResource(r.inCopy, tex); fresh = true; haveImg = true; cw = td.Width; ch = td.Height; }
            tex->Release();
          }
          rel(res);
          dup->ReleaseFrame();
        }
        // a captura da janela continua aberta (para voltar na hora): descarta o que chegou por ela
        for (IDirect3D11CaptureFrame* nx = nullptr; SUCCEEDED(r.pool->TryGetNextFrame(&nx)) && nx; nx = nullptr) closeFrame(nx);
      }
      else if (SUCCEEDED(r.pool->TryGetNextFrame(&fr)) && fr){
        for (IDirect3D11CaptureFrame* nx = nullptr; SUCCEEDED(r.pool->TryGetNextFrame(&nx)) && nx; nx = nullptr){ closeFrame(fr); fr = nx; }
        SizeInt32 cs{}; fr->get_ContentSize(&cs);
        if (cs.Width >= 16 && cs.Height >= 16 && (cs.Width != poolSz.Width || cs.Height != poolSz.Height)){
          poolSz = cs; r.pool->Recreate(r.rtDev, PIXEL_BGRA8, 3, poolSz); closeFrame(fr); fr = nullptr;
        }
      }
      if (fr){
        ID3D11Texture2D* tex = frameTexture(fr);
        if (tex){
          D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
          if ((int)td.Width != r.inW || (int)td.Height != r.inH){
            rel(r.inCopy);
            r.inCopy = makeTex(r.dev, td.Width, td.Height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0);
            r.inW = td.Width; r.inH = td.Height;
            for (auto* s : ses) sesVP(*s, r, r.inW, r.inH, s->fps);
          }
          if (r.inCopy){ r.ctx->CopyResource(r.inCopy, tex); fresh = true; haveImg = true; }
          SizeInt32 cs{}; fr->get_ContentSize(&cs);
          cw = std::min<int>(cs.Width > 0 ? cs.Width : td.Width, td.Width); ch = std::min<int>(cs.Height > 0 ? cs.Height : td.Height, td.Height);
          tex->Release();
        }
        closeFrame(fr);
      }
      bool did = false;
      for (auto* sp : ses){
        EncSes& s = *sp;
        sesPump(s, e->conn);
        bool due = fresh ? s.pacer.accept(now) : false;
        // NVENC: o quadro completo pedido pode estar esperando o intervalo de 1 s; enquanto espera, o pedido não pode virar
        // "repete agora" (codificava a cada volta do laço: 198 quadros/s, o vídeo de mentira entupia e quem assistia saía do motor)
        bool keyNow = s.key && (!s.nv || now - s.lastIdr >= 1000);
        bool repeat = !fresh && haveImg && (keyNow || now - s.lastIn > 250);   // tela parada: repete para o vídeo não parar (e o quadro completo sair)
        if (!(due || repeat) || !s.vp) continue;
        int slot = sesFreeSlot(s);
        if (slot < 0){ s.nBusy++; continue; }
        RECT src = {0, 0, cw, ch};
        if (haveCrop && !dup) src = crop;
        IRect d = aspectRect(src.right - src.left, src.bottom - src.top, s.ow, s.oh);
        RECT dst = {d.l, d.t, d.r, d.b};
        r.vctx->VideoProcessorSetStreamSourceRect(s.vp, 0, TRUE, &src);
        r.vctx->VideoProcessorSetStreamDestRect(s.vp, 0, TRUE, &dst);
        D3D11_VIDEO_PROCESSOR_STREAM vs = {}; vs.Enable = TRUE; vs.pInputSurface = s.vin;
        if (FAILED(r.vctx->VideoProcessorBlt(s.vp, s.views[slot], 0, 1, &vs))) continue;
        s.slot = slot;
        if (sesInput(s, e->conn, slot, t0)) did = true;
      }
      if (!fresh && !did) hrSleep(r.timer, 1);
    }
    if (dup) dup->Release();
  }
  if (!endReason.empty()){ engSend(e, "{\"type\":\"eended\",\"reason\":\"" + jsonEsc(endReason) + "\"}"); logf("Motor: captura encerrada: %s", endReason.c_str()); }
done:
  for (auto* s : ses){ sesFree(*s); delete s; }
  ses.clear();
  rel(mgr);
  freeRes(r);
}
static DWORD WINAPI engThread(void* arg){
  Eng* e = (Eng*)arg;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  typedef HANDLE (WINAPI *PFN_AvSet)(LPCWSTR, LPDWORD);
  typedef BOOL (WINAPI *PFN_AvRevert)(HANDLE);
  HMODULE avrt = LoadLibraryW(L"avrt.dll");
  PFN_AvSet avSet = avrt ? (PFN_AvSet)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : nullptr;
  PFN_AvRevert avRevert = avrt ? (PFN_AvRevert)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics") : nullptr;
  DWORD idx = 0; HANDLE mm = avSet ? avSet(L"Capture", &idx) : nullptr;
  engRun(e);
  if (mm && avRevert) avRevert(mm);
  CoUninitialize();
  return 0;
}
static void engStop(Conn* onlyFor){
  EnterCriticalSection(&gEngLock);
  Eng* e = gEng;
  if (e && (!onlyFor || e->conn == onlyFor)){
    gEng = nullptr;
    InterlockedExchange(&e->stop, 1);
    if (WaitForSingleObject(e->thread, 5000) == WAIT_OBJECT_0){
      CloseHandle(e->thread);
      for (auto& c : e->cmds) delete c.ses;
      DeleteCriticalSection(&e->lock);
      delete e;
    }
  }
  LeaveCriticalSection(&gEngLock);
}
static void engPost(Conn* c, const EngCmd& cmd){
  EnterCriticalSection(&gEngLock);
  if (gEng && gEng->conn == c){ EnterCriticalSection(&gEng->lock); gEng->cmds.push_back(cmd); LeaveCriticalSection(&gEng->lock); }
  else if (cmd.ses){ wsText(c, "{\"type\":\"esesserr\",\"id\":" + std::to_string(cmd.ses->id) + ",\"message\":\"captura não iniciada\"}"); delete cmd.ses; }
  LeaveCriticalSection(&gEngLock);
}

/* ---------- decodificação e janela de vídeo (quem assiste) ---------- */
struct DecFrame { std::vector<uint8_t> data; bool key = false; uint32_t seq = 0; };
struct DecStream {
  uint32_t id = 0; bool hevc = false; Conn* conn = nullptr;
  HWND owner = nullptr, wnd = nullptr;
  RECT rc{0, 0, 0, 0}; bool visible = false, full = false;   // rc e buracos: na área interna da janela do Telão (pixels físicos)
  std::vector<RECT> holes;
  POINT org{-99999, -99999}; bool firstSent = false;   // onde a janela do Telão estava na última vez; já avisou o 1º quadro
  IMFTransform* mft = nullptr; bool waitKey = true; std::string dec; bool outOk = false; std::string outErr; int noOut = 0; HRESULT outHr = S_OK;
  IDXGISwapChain1* sc = nullptr; int scW = 0, scH = 0;
  ID3D11VideoProcessorEnumerator* vpe = nullptr; ID3D11VideoProcessor* vp = nullptr; int vpInW = 0, vpInH = 0, vpOutW = 0, vpOutH = 0;
  std::vector<std::pair<std::pair<ID3D11Texture2D*, UINT>, ID3D11VideoProcessorInputView*>> inViews;
  IMFSample* last = nullptr; int vw = 0, vh = 0, cropX = 0, cropY = 0;   // último quadro decodificado (repintar, imagem parada)
  std::deque<DecFrame> q;   // quadros esperando (protegido por gPresLock)
  Ov ov;                    // reações por cima do vídeo
  std::wstring name, sub;   // tela cheia: nome e jogo
  std::vector<std::wstring> fsReacts; double fsUntil = 0; int fsHover = -1;
  int nIn = 0, nDec = 0, nPres = 0, nDrop = 0, freezes = 0, errors = 0; double lastPres = 0, lastDec = 0; std::vector<double> decMs;
};
struct PresCmd { int kind = 0; uint32_t id = 0; Conn* conn = nullptr; std::string a, b, c, d; int x = 0, y = 0, w = 0, h = 0; bool on = false; unsigned long long owner = 0; std::vector<RECT> holes; HANDLE done = nullptr; };
static CRITICAL_SECTION gPresLock;
static std::vector<PresCmd*> gPresCmds;
static std::vector<DecStream*> gStreams;   // só a linha da janela mexe (fora a fila de quadros, que é protegida)
static HANDLE gPresThread = nullptr, gPresWake = nullptr, gPresReady = nullptr;
static DWORD gPresTid = 0;
static volatile LONG gPresState = 0;   // 0 nada, 1 pronta, -1 não deu
struct Pres {
  ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; ID3D11VideoDevice* vdev = nullptr; ID3D11VideoContext* vctx = nullptr;
  IDXGIFactory2* fac = nullptr; IMFDXGIDeviceManager* mgr = nullptr; UINT tok = 0;
  ID2D1Factory1* d2 = nullptr; ID2D1Device* d2dev = nullptr; IDWriteFactory* dw = nullptr; IWICImagingFactory* wic = nullptr;
  bool hevcDec = false; std::string adapter;
} gPr;

static DecStream* findStream(uint32_t id){ for (auto* s : gStreams) if (s->id == id) return s; return nullptr; }
static void presSend(DecStream* s, const std::string& j){ if (s->conn && s->conn->alive) wsText(s->conn, j); }

static IMFTransform* decCreate(bool hevc, std::string* name, std::string* err){
  IMFTransform* t = nullptr;
  if (!hevc){
    HRESULT hr = CoCreateInstance(kCLSID_MSH264Decoder, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void**)&t);
    if (SUCCEEDED(hr) && t){ *name = "Microsoft H.264"; return t; }
    *err = "decodificador H.264 do Windows (" + hrText(hr) + ")";
    return nullptr;
  }
  // H.265: o da "Extensão de Vídeo HEVC" (só os síncronos: os assíncronos da placa ficam para depois)
  t = mftPick(kMFT_CAT_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, FCC_HEVC, FCC_NV12, nullptr, name, err);
  if (!t) *err = "decodificador H.265 do Windows (instale a \"Extensão de Vídeo HEVC\"): " + *err;
  return t;
}
static bool decPickOutput(DecStream* s){
  int n = 0; HRESULT last = S_OK, setHr = S_OK; std::string subs;
  for (DWORD i = 0; i < 64; i++){
    IMFMediaType* t = nullptr;
    HRESULT ghr = s->mft->GetOutputAvailableType(0, i, &t);
    if (FAILED(ghr) || !t){ last = ghr; break; }
    n++;
    GUID sub{}; t->GetGUID(kMF_MT_SUBTYPE, &sub);
    char fc[5] = {(char)(sub.Data1 & 0xff), (char)((sub.Data1 >> 8) & 0xff), (char)((sub.Data1 >> 16) & 0xff), (char)((sub.Data1 >> 24) & 0xff), 0};
    for (char& ch : fc) if (ch && (ch < 32 || ch > 126)) ch = '?';
    if (subs.size() < 60){ if (!subs.empty()) subs += ","; subs += fc; }
    bool isNv12 = IsEqualGUID(sub, mfFourcc(FCC_NV12));
    if (isNv12){ setHr = s->mft->SetOutputType(0, t, 0); }
    bool ok = isNv12 && SUCCEEDED(setHr);
    if (ok){
      UINT32 w = 0, h = 0; mtGetSize(t, kMF_MT_FRAME_SIZE, &w, &h);
      MFVideoArea ar{}; UINT32 sz = 0;
      s->cropX = s->cropY = 0; s->vw = (int)w; s->vh = (int)h;
      if (SUCCEEDED(t->GetBlob(kMF_MT_MIN_APERTURE, (UINT8*)&ar, sizeof ar, &sz)) && sz == sizeof ar && ar.Area.cx > 0 && ar.Area.cy > 0){
        s->cropX = ar.OffsetX.value; s->cropY = ar.OffsetY.value; s->vw = ar.Area.cx; s->vh = ar.Area.cy;
      }
    }
    t->Release();
    if (ok){ s->outOk = true; s->outErr.clear(); return true; }
  }
  s->outOk = false;
  s->outErr = std::to_string(n) + " formatos (" + subs + ")" + (FAILED(last) && last != MF_E_NO_MORE_TYPES ? ", lista: " + hrText(last) : "") + (FAILED(setHr) ? ", NV12 recusado: " + hrText(setHr) : "");
  return false;
}
static std::string decOpen(DecStream* s){
  std::string err;
  s->mft = decCreate(s->hevc, &s->dec, &err);
  if (!s->mft) return err;
  IMFAttributes* at = nullptr; UINT32 aware = 0;
  if (SUCCEEDED(s->mft->GetAttributes(&at)) && at){ at->GetUINT32(kMF_SA_D3D11_AWARE, &aware); at->SetUINT32(kMF_LOW_LATENCY, TRUE); at->Release(); }
  if (!aware) return "o decodificador não trabalha direto na placa";
  HRESULT hr = s->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)gPr.mgr);
  if (FAILED(hr)) return "o decodificador não aceitou a placa (" + hrText(hr) + ")";
  ICodecAPI* api = nullptr;
  if (SUCCEEDED(s->mft->QueryInterface(__uuidof(ICodecAPI), (void**)&api))){ capiSet(api, kCAPI_LowLatency, 1); api->Release(); }
  // tamanho de partida (o de verdade vem no 1º quadro completo e o decodificador avisa a troca); alguns só oferecem
  // os formatos de saída depois de saber um tamanho
  for (int pass = 0; pass < 2; pass++){
    IMFMediaType* it = nullptr; gMF.CreateMediaType(&it);
    it->SetGUID(kMF_MT_MAJOR_TYPE, kMFMediaType_Video); it->SetGUID(kMF_MT_SUBTYPE, mfFourcc(s->hevc ? FCC_HEVC : FCC_H264));
    it->SetUINT32(kMF_MT_INTERLACE_MODE, 2);
    if (pass == 0){ mtSize(it, kMF_MT_FRAME_SIZE, 1920, 1080); mtSize(it, kMF_MT_FRAME_RATE, 60, 1); mtSize(it, kMF_MT_PIXEL_ASPECT, 1, 1); }
    hr = s->mft->SetInputType(0, it, 0); it->Release();
    if (SUCCEEDED(hr)) break;
  }
  if (FAILED(hr)) return "o decodificador recusou o formato (" + hrText(hr) + ")";
  // sem formato de saída ainda: escolhe quando o 1º quadro completo chegar (o decodificador pede com "tipo não definido")
  if (!decPickOutput(s)) logf("Motor: tela %u: decodificador ainda sem NV12 (%s); escolho no 1º quadro", s->id, s->outErr.c_str());
  s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return "";
}
static void streamFreeVP(DecStream* s){ for (auto& v : s->inViews) rel(v.second); s->inViews.clear(); rel(s->vp); rel(s->vpe); s->vpInW = s->vpInH = s->vpOutW = s->vpOutH = 0; }
static void streamFree(DecStream* s){
  streamFreeVP(s);
  if (s->last){ s->last->Release(); s->last = nullptr; }
  if (s->mft){ s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); rel(s->mft); }
  safeRelease(s->ov.white); safeRelease(s->ov.black); safeRelease(s->ov.green); safeRelease(s->ov.bubble); safeRelease(s->ov.label);
  safeRelease(s->ov.fEmoji); safeRelease(s->ov.fBig); safeRelease(s->ov.fLabel); safeRelease(s->ov.fChat);
  s->ov.items.clear(); s->ov.cache.clear();
  s->ov.rt = nullptr; safeRelease(s->ov.dc); s->ov.d2 = nullptr; s->ov.dw = nullptr; s->ov.wic = nullptr;
  rel(s->sc);
  if (s->wnd){ DestroyWindow(s->wnd); s->wnd = nullptr; }
}
// Desenha o último quadro (com reações e controles da tela cheia) na janela.
static void streamDrawOverlay(DecStream* s, ID3D11Texture2D* bb);
static bool streamSnap(DecStream* s, int w, int h, std::string* out, std::string* err);
// Contexto de desenho (Direct2D) da tela: um por janela, criado na primeira vez que precisa.
static bool streamEnsureDC(DecStream* s){
  Ov& o = s->ov;
  if (o.dc) return true;
  if (!gPr.d2dev || FAILED(gPr.d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &o.dc)) || !o.dc) return false;
  o.rt = o.dc; o.d2 = gPr.d2; o.dw = gPr.dw; o.wic = gPr.wic;
  o.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  return true;
}
static void streamPresent(DecStream* s){
  if (!s->wnd || !s->visible) return;
  RECT cr; GetClientRect(s->wnd, &cr);
  int W = std::max(1L, cr.right), H = std::max(1L, cr.bottom);
  if (!s->sc){
    DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = W; d.Height = H; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr = gPr.fac->CreateSwapChainForHwnd(gPr.dev, s->wnd, &d, nullptr, nullptr, &s->sc);
    if (FAILED(hr)){ if (s->errors++ < 3) presSend(s, "{\"type\":\"derror\",\"id\":" + std::to_string(s->id) + ",\"message\":\"" + jsonEsc("não consegui desenhar na janela (" + hrText(hr) + ")") + "\"}"); return; }
    s->scW = W; s->scH = H;
  } else if (W != s->scW || H != s->scH){
    streamFreeVP(s);
    s->sc->ResizeBuffers(0, W, H, DXGI_FORMAT_UNKNOWN, 0);
    s->scW = W; s->scH = H;
  }
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(s->sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
  ID3D11Texture2D* tex = nullptr; UINT sub = 0;
  if (s->last){
    IMFMediaBuffer* mb = nullptr; IMFDXGIBuffer* db = nullptr;
    if (SUCCEEDED(s->last->GetBufferByIndex(0, &mb)) && SUCCEEDED(mb->QueryInterface(__uuidof(IMFDXGIBuffer), (void**)&db))){
      db->GetResource(__uuidof(ID3D11Texture2D), (void**)&tex); db->GetSubresourceIndex(&sub);
    }
    rel(db); rel(mb);
  }
  bool drawn = false;
  if (tex){
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    if (!s->vp || s->vpInW != (int)td.Width || s->vpInH != (int)td.Height || s->vpOutW != W || s->vpOutH != H){
      streamFreeVP(s);
      D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
      cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
      cd.InputWidth = td.Width; cd.InputHeight = td.Height; cd.OutputWidth = W; cd.OutputHeight = H;
      cd.InputFrameRate = {60, 1}; cd.OutputFrameRate = {60, 1}; cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
      if (SUCCEEDED(gPr.vdev->CreateVideoProcessorEnumerator(&cd, &s->vpe)) && SUCCEEDED(gPr.vdev->CreateVideoProcessor(s->vpe, 0, &s->vp))){
        s->vpInW = td.Width; s->vpInH = td.Height; s->vpOutW = W; s->vpOutH = H;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {}; in.YCbCr_Matrix = 1; in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {}; out.RGB_Range = 0;
        gPr.vctx->VideoProcessorSetStreamColorSpace(s->vp, 0, &in);
        gPr.vctx->VideoProcessorSetOutputColorSpace(s->vp, &out);
        gPr.vctx->VideoProcessorSetStreamFrameFormat(s->vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        gPr.vctx->VideoProcessorSetStreamAutoProcessingMode(s->vp, 0, FALSE);
        D3D11_VIDEO_COLOR black = {}; black.RGBA.A = 1.0f;
        gPr.vctx->VideoProcessorSetOutputBackgroundColor(s->vp, FALSE, &black);
      }
    }
    if (s->vp){
      ID3D11VideoProcessorInputView* iv = nullptr;
      for (auto& v : s->inViews) if (v.first.first == tex && v.first.second == sub){ iv = v.second; break; }
      if (!iv){
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {}; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; ivd.Texture2D.ArraySlice = sub;
        if (SUCCEEDED(gPr.vdev->CreateVideoProcessorInputView(tex, s->vpe, &ivd, &iv))){
          tex->AddRef(); s->inViews.push_back({{tex, sub}, iv});
          if (s->inViews.size() > 40){ s->inViews.front().first.first->Release(); rel(s->inViews.front().second); s->inViews.erase(s->inViews.begin()); }
        }
      }
      ID3D11VideoProcessorOutputView* ov = nullptr;
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      if (iv && SUCCEEDED(gPr.vdev->CreateVideoProcessorOutputView(bb, s->vpe, &ovd, &ov))){
        RECT src = {s->cropX, s->cropY, s->cropX + s->vw, s->cropY + s->vh};
        FRect fr = letterbox(s->vw, s->vh, W, H);
        RECT dst = {(LONG)fr.l, (LONG)fr.t, (LONG)fr.r, (LONG)fr.b};
        RECT all = {0, 0, W, H};
        gPr.vctx->VideoProcessorSetStreamSourceRect(s->vp, 0, TRUE, &src);
        gPr.vctx->VideoProcessorSetStreamDestRect(s->vp, 0, TRUE, &dst);
        gPr.vctx->VideoProcessorSetOutputTargetRect(s->vp, TRUE, &all);
        D3D11_VIDEO_PROCESSOR_STREAM vs = {}; vs.Enable = TRUE; vs.pInputSurface = iv;
        drawn = SUCCEEDED(gPr.vctx->VideoProcessorBlt(s->vp, ov, 0, 1, &vs));
        ov->Release();
      }
    }
    tex->Release();
  }
  if (!drawn){   // sem quadro ainda: fundo preto
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(gPr.dev->CreateRenderTargetView(bb, nullptr, &rtv))){ float c[4] = {0, 0, 0, 1}; gPr.ctx->ClearRenderTargetView(rtv, c); rtv->Release(); }
  }
  streamDrawOverlay(s, bb);
  bb->Release();
  s->sc->Present(0, 0);
  s->nPres++; s->lastPres = nowMs();
}
// Reações (e, na tela cheia, nome e barra de reações) por cima do vídeo, com o mesmo desenho da camada sobre o jogo.
static bool ovPrepare(Ov& o, ID2D1DeviceContext* dc, int W, int H, double scale){
  if (o.W == W && o.H == H && std::fabs(o.scale - scale) < 0.01 && o.white) return true;
  safeRelease(o.white); safeRelease(o.black); safeRelease(o.green); safeRelease(o.bubble); safeRelease(o.label);
  safeRelease(o.fEmoji); safeRelease(o.fBig); safeRelease(o.fLabel); safeRelease(o.fChat);
  o.W = W; o.H = H; o.scale = scale;
  dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &o.white);
  dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f), &o.black);
  dc->CreateSolidColorBrush(D2D1::ColorF(0x7cf29a, 1), &o.green);
  dc->CreateSolidColorBrush(D2D1::ColorF(10 / 255.f, 12 / 255.f, 16 / 255.f, 0.82f), &o.bubble);
  dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &o.label);
  auto font = [&](float px, DWRITE_FONT_WEIGHT w){ IDWriteTextFormat* f = nullptr; gPr.dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"pt-br", &f); return f; };
  o.fEmoji = font((float)(44 * scale), DWRITE_FONT_WEIGHT_NORMAL);
  o.fBig = font((float)(26 * scale), DWRITE_FONT_WEIGHT_EXTRA_BOLD);
  o.fLabel = font((float)(12 * scale), DWRITE_FONT_WEIGHT_NORMAL);
  o.fChat = font((float)(14 * scale), DWRITE_FONT_WEIGHT_NORMAL);
  return o.white && o.fEmoji;
}
static std::vector<D2D1_RECT_F> fsButtons(DecStream* s, int W, int H, double k){
  std::vector<D2D1_RECT_F> v;
  int n = (int)s->fsReacts.size() + 1;   // reações + sair
  float bw = (float)(48 * k), gap = (float)(4 * k), total = n * bw + (n - 1) * gap + (float)(12 * k);
  float x = (W - total) / 2 + (float)(6 * k), y = H - (float)(22 * k) - bw;
  for (int i = 0; i < n; i++){ v.push_back(D2D1::RectF(x, y, x + bw, y + bw)); x += bw + gap; }
  return v;
}
static void streamDrawOverlay(DecStream* s, ID3D11Texture2D* bb){
  double now = nowMs();
  bool fsUi = s->full && now < s->fsUntil;
  Ov& o = s->ov;
  if (o.items.empty() && !fsUi) return;
  if (!streamEnsureDC(s)) return;
  ID2D1DeviceContext* dc = s->ov.dc;
  IDXGISurface* surf = nullptr; ID2D1Bitmap1* target = nullptr;
  D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
  if (SUCCEEDED(bb->QueryInterface(__uuidof(IDXGISurface), (void**)&surf)) && SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surf, &bp, &target))){
    dc->SetTarget(target);
    UINT dpi = 96; static PFN_GetDpiForWindow gdw = (PFN_GetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (gdw) dpi = gdw(s->wnd);
    double k = std::max(1.0, dpi / 96.0) * (s->full ? 1.25 : std::min(1.0, std::max(0.6, s->scH / 540.0)));
    ovPrepare(o, dc, s->scW, s->scH, k);
    gOvCur = &o;
    dc->BeginDraw();
    o.items.erase(std::remove_if(o.items.begin(), o.items.end(), [&](const OvItem& it){ return (now - o.t0) - it.born > (it.chat ? 8900 : 3000); }), o.items.end());
    for (auto& it : o.items) if (!it.chat) ovDrawReact(it, (now - o.t0) - it.born);
    ovDrawChat(now - o.t0);
    if (fsUi){
      ID2D1SolidColorBrush *bg = nullptr, *hov = nullptr;
      dc->CreateSolidColorBrush(D2D1::ColorF(13 / 255.f, 15 / 255.f, 18 / 255.f, 0.86f), &bg);
      dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), &hov);
      auto bts = fsButtons(s, s->scW, s->scH, k);
      if (!bts.empty() && bg){
        D2D1_RECT_F bar = D2D1::RectF(bts.front().left - (float)(6 * k), bts.front().top - (float)(6 * k), bts.back().right + (float)(6 * k), bts.back().bottom + (float)(6 * k));
        dc->FillRoundedRectangle(D2D1::RoundedRect(bar, (bar.bottom - bar.top) / 2, (bar.bottom - bar.top) / 2), bg);
      }
      for (size_t i = 0; i < bts.size(); i++){
        if ((int)i == s->fsHover && hov) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((bts[i].left + bts[i].right) / 2, (bts[i].top + bts[i].bottom) / 2), (bts[i].right - bts[i].left) / 2, (bts[i].bottom - bts[i].top) / 2), hov);
        std::wstring t = i < s->fsReacts.size() ? s->fsReacts[i] : std::wstring(L"✕");
        IDWriteTextLayout* l = ovLayout(t, t.size() > 2 && i < s->fsReacts.size() ? o.fLabel : o.fChat, 400);
        if (!l) continue;
        if (i < s->fsReacts.size() && t.size() <= 2){ l->Release(); l = nullptr; IDWriteTextFormat* f = nullptr; gPr.dw->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (float)(24 * k), L"pt-br", &f); if (f){ gPr.dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, 400, 400, &l); f->Release(); } if (!l) continue; }
        DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
        ovText(l, (bts[i].left + bts[i].right - m.width) / 2, (bts[i].top + bts[i].bottom - m.height) / 2, o.white);
        l->Release();
      }
      // nome e jogo em cima, à esquerda
      if (!s->name.empty()){
        std::wstring top = s->name + (s->sub.empty() ? L"" : L"\n" + s->sub);
        IDWriteTextLayout* l = ovLayout(top, o.fChat, (float)(s->scW / 2));
        if (l){
          DWRITE_TEXT_RANGE nr{0, (UINT32)s->name.size()}; l->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, nr);
          DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
          float x = (float)(24 * k), y = (float)(18 * k), pad = (float)(10 * k);
          if (bg) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - pad, y - pad, x + m.width + pad, y + m.height + pad), (float)(10 * k), (float)(10 * k)), bg);
          ovText(l, x, y, o.white);
          l->Release();
        }
      }
      safeRelease(bg); safeRelease(hov);
    }
    dc->EndDraw();
    dc->SetTarget(nullptr);
    gOvCur = nullptr;
  }
  safeRelease(target); safeRelease(surf);
}
// Decodifica o que chegou; mostra só o quadro mais novo.
static void streamDecode(DecStream* s){
  std::deque<DecFrame> q;
  EnterCriticalSection(&gPresLock); q.swap(s->q); LeaveCriticalSection(&gPresLock);
  if (!s->mft){ s->nDrop += (int)q.size(); return; }
  bool got = false;
  auto drain = [&](){
    for (int guard = 0; guard < 16; guard++){
      MFT_OUTPUT_DATA_BUFFER ob{}; DWORD st = 0;
      HRESULT hr = s->mft->ProcessOutput(0, 1, &ob, &st);
      if (ob.pEvents) ob.pEvents->Release();
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == (HRESULT)0xC00D6D60 /*MF_E_TRANSFORM_TYPE_NOT_SET*/){ if (!decPickOutput(s) && hr != MF_E_TRANSFORM_STREAM_CHANGE) break; streamFreeVP(s); continue; }
      if (FAILED(hr)){ if (hr != (HRESULT)0xC00D6D72 /*MF_E_TRANSFORM_NEED_MORE_INPUT*/) s->outHr = hr; break; }
      if (ob.pSample){ if (s->last) s->last->Release(); s->last = ob.pSample; got = true; s->nDec++; }
    }
  };
  for (auto& f : q){
    s->nIn++;
    if (s->waitKey){
      if (!f.key){ s->nDrop++; continue; }
      s->waitKey = false;
    }
    IMFSample* smp = nullptr; IMFMediaBuffer* buf = nullptr; BYTE* p = nullptr;
    if (FAILED(gMF.CreateSample(&smp)) || FAILED(gMF.CreateMemoryBuffer((DWORD)f.data.size(), &buf))){ rel(smp); rel(buf); continue; }
    if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr))){ memcpy(p, f.data.data(), f.data.size()); buf->Unlock(); buf->SetCurrentLength((DWORD)f.data.size()); }
    smp->AddBuffer(buf); buf->Release();
    smp->SetSampleTime((LONGLONG)f.seq * 166667); smp->SetSampleDuration(166667);
    double t0 = nowMs();
    HRESULT hr = s->mft->ProcessInput(0, smp, 0);
    if (hr == MF_E_NOTACCEPTING){ drain(); hr = s->mft->ProcessInput(0, smp, 0); }
    smp->Release();
    if (FAILED(hr)){
      if (s->errors++ < 5) logf("Motor: decodificador recusou o quadro da tela %u (%s)", s->id, hrText(hr).c_str());
      s->waitKey = true;
      presSend(s, "{\"type\":\"dneedkey\",\"id\":" + std::to_string(s->id) + "}");
      continue;
    }
    drain();
    s->decMs.push_back(nowMs() - t0);
    // quadros entrando e nenhuma imagem saindo: avisa o Telão (ele volta para o Chromium) com o motivo
    if (!s->firstSent && !got && ++s->noOut == 90){
      std::string why = "o decodificador não entregou imagem em 90 quadros";
      if (!s->outOk) why += " · sem NV12: " + s->outErr;
      if (FAILED(s->outHr)) why += " · saída: " + hrText(s->outHr);
      presSend(s, "{\"type\":\"derror\",\"id\":" + std::to_string(s->id) + ",\"message\":\"" + jsonEsc(why) + "\"}");
      logf("Motor: tela %u: %s", s->id, why.c_str());
    }
  }
  if (got){
    double now = nowMs();
    if (!s->firstSent){ s->firstSent = true; presSend(s, "{\"type\":\"dfirst\",\"id\":" + std::to_string(s->id) + ",\"w\":" + std::to_string(s->vw) + ",\"h\":" + std::to_string(s->vh) + "}"); }
    if (s->lastDec && now - s->lastDec > 250) s->freezes++;
    s->lastDec = now;
    streamPresent(s);
  }
}
static LRESULT CALLBACK vidProc(HWND h, UINT m, WPARAM w, LPARAM l){
  DecStream* s = (DecStream*)GetWindowLongPtrW(h, GWLP_USERDATA);
  auto send = [&](const char* ev, int extra){ if (s){ char b[160]; snprintf(b, sizeof b, "{\"type\":\"dev\",\"id\":%u,\"ev\":\"%s\",\"n\":%d}", s->id, ev, extra); presSend(s, b); } };
  switch (m){
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); if (s) streamPresent(s); return 0; }
    case WM_LBUTTONDBLCLK: send("dbl", 0); return 0;
    case WM_RBUTTONUP: send("ctx", 0); return 0;
    case WM_MOUSEWHEEL: send("wheel", GET_WHEEL_DELTA_WPARAM(w)); return 0;
    case WM_MOUSEMOVE:
      if (s && s->full){
        s->fsUntil = nowMs() + 3000;
        UINT dpi = 96; static PFN_GetDpiForWindow gdw = (PFN_GetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (gdw) dpi = gdw(h);
        auto bts = fsButtons(s, s->scW, s->scH, std::max(1.0, dpi / 96.0) * 1.25);
        int x = (short)LOWORD(l), y = (short)HIWORD(l), hov = -1;
        for (size_t i = 0; i < bts.size(); i++) if (x >= bts[i].left && x < bts[i].right && y >= bts[i].top && y < bts[i].bottom) hov = (int)i;
        s->fsHover = hov;
        streamPresent(s);
      }
      return 0;
    case WM_LBUTTONUP:
      if (s && s->full && nowMs() < s->fsUntil && s->fsHover >= 0){
        if (s->fsHover < (int)s->fsReacts.size()) send("react", s->fsHover); else send("exit", 0);
        return 0;
      }
      send("click", 0); return 0;
  }
  return DefWindowProcW(h, m, w, l);
}
// A posição vem relativa à área interna da janela do Telão: aqui vira posição na tela (e acompanha a janela quando ela anda).
static POINT ownerOrigin(DecStream* s){
  POINT p = {0, 0};
  if (s->owner && IsWindow(s->owner)) ClientToScreen(s->owner, &p);
  return p;
}
static void streamPlace(DecStream* s){
  if (!s->wnd) return;
  bool ownerOk = !s->owner || (IsWindow(s->owner) && IsWindowVisible(s->owner) && !IsIconic(s->owner));
  if (!ownerOk || !s->visible || s->rc.right - s->rc.left < 8 || s->rc.bottom - s->rc.top < 8){ ShowWindow(s->wnd, SW_HIDE); s->org = {-99999, -99999}; return; }
  POINT o = ownerOrigin(s); s->org = o;
  int w = s->rc.right - s->rc.left, h = s->rc.bottom - s->rc.top;
  // buracos: onde a página tem algo por cima (aviso, menu, janela), a janela de vídeo some e deixa ver a página
  HRGN rgn = CreateRectRgn(0, 0, w, h);
  for (auto& b : s->holes){ HRGN hr = CreateRectRgn(b.left - s->rc.left, b.top - s->rc.top, b.right - s->rc.left, b.bottom - s->rc.top); CombineRgn(rgn, rgn, hr, RGN_DIFF); DeleteObject(hr); }
  SetWindowRgn(s->wnd, rgn, FALSE);   // o Windows fica dono da região
  SetWindowPos(s->wnd, nullptr, o.x + s->rc.left, o.y + s->rc.top, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}
// A janela do Telão mudou de lugar (arrastada, maximizada): as janelas de vídeo vão junto na hora.
static HWINEVENTHOOK gPresHook = nullptr; static DWORD gPresHookPid = 0;
static void CALLBACK presFollow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD){
  if (obj != OBJID_WINDOW) return;
  for (auto* s : gStreams) if (s->owner == h && s->visible){ POINT o = ownerOrigin(s); if (o.x != s->org.x || o.y != s->org.y) streamPlace(s); }
}
static void presHookOwner(HWND owner){
  DWORD pid = 0; if (!owner || !GetWindowThreadProcessId(owner, &pid) || !pid) return;
  if (gPresHook && gPresHookPid == pid) return;
  if (gPresHook){ UnhookWinEvent(gPresHook); gPresHook = nullptr; }
  gPresHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, presFollow, pid, 0, WINEVENT_OUTOFCONTEXT);
  gPresHookPid = pid;
}
static void presApply(PresCmd* c){
  DecStream* s = findStream(c->id);
  if (c->kind == 1){   // abrir
    if (s){ streamFree(s); gStreams.erase(std::find(gStreams.begin(), gStreams.end(), s)); delete s; s = nullptr; }
    s = new DecStream(); s->id = c->id; s->conn = c->conn; s->hevc = c->a == "h265";
    s->owner = (HWND)(uintptr_t)c->owner; s->ov.t0 = nowMs();
    std::string err = decOpen(s);
    if (err.empty()){
      s->wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"TelaoVideo", L"Telão — vídeo", WS_POPUP | WS_CLIPSIBLINGS,
                               0, 0, 16, 16, IsWindow(s->owner) ? s->owner : nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!s->wnd) err = "não consegui criar a janela de vídeo";
      else SetWindowLongPtrW(s->wnd, GWLP_USERDATA, (LONG_PTR)s);
    }
    if (!err.empty()){
      presSend(s, "{\"type\":\"dopenerr\",\"id\":" + std::to_string(s->id) + ",\"message\":\"" + jsonEsc(err) + "\"}");
      logf("Motor: tela %u não abriu: %s", s->id, err.c_str());
      streamFree(s); delete s;
      return;
    }
    gStreams.push_back(s);
    presHookOwner(s->owner);
    presSend(s, "{\"type\":\"dopenok\",\"id\":" + std::to_string(s->id) + ",\"decoder\":\"" + jsonEsc(s->dec) + "\"}");
    logf("Motor: tela %u aberta (%s, %s)", s->id, s->hevc ? "H.265" : "H.264", s->dec.c_str());
  } else if (!s){
    return;
  } else if (c->kind == 2){   // fechar
    streamFree(s); gStreams.erase(std::find(gStreams.begin(), gStreams.end(), s)); delete s;
  } else if (c->kind == 3){   // posição e buracos
    s->rc = {c->x, c->y, c->x + c->w, c->y + c->h}; s->visible = c->on; s->holes = c->holes;
    streamPlace(s);
    streamPresent(s);
  } else if (c->kind == 4){   // reação ou mensagem por cima
    OvItem it; it.chat = c->d == "chat"; it.born = nowMs() - s->ov.t0;
    it.text = wide(c->a).substr(0, it.chat ? 400 : 32); it.who = wide(c->b).substr(0, 48);
    if (!c->c.empty() && streamEnsureDC(s)) it.img = ovDecode(s->ov, c->c, (int)std::lround(120 * std::max(1.0, s->ov.scale)));
    if (it.text.empty() && !it.img) return;
    it.xFrac = (float)(0.08 + (rand() % 1000) / 1000.0 * 0.52);
    if (s->ov.items.size() < 30) s->ov.items.push_back(std::move(it));
  } else if (c->kind == 5){   // tela cheia: liga/desliga os controles desenhados aqui
    s->full = c->on; s->fsUntil = c->on ? nowMs() + 3000 : 0;
    s->name = wide(c->a); s->sub = wide(c->b);
    s->fsReacts.clear();
    size_t p = 0; std::string all = c->c;
    while (p < all.size() && s->fsReacts.size() < 8){ size_t q = all.find('\n', p); if (q == std::string::npos) q = all.size(); std::string one = all.substr(p, q - p); if (!one.empty()) s->fsReacts.push_back(wide(one)); p = q + 1; }
    streamPresent(s);
  } else if (c->kind == 6){   // teste padrão: miniatura em tons de cinza do último quadro decodificado
    std::string err, y; bool raw = c->w <= 0; int w = raw ? 0 : std::max(64, std::min(1280, c->w)), h = raw ? 0 : std::max(36, std::min(720, c->h));
    if (streamSnap(s, w, h, &y, &err)){
      if (raw){ size_t a = y.find(','), b = y.find(',', a + 1); w = atoi(y.substr(0, a).c_str()); h = atoi(y.substr(a + 1, b - a - 1).c_str()); y = y.substr(b + 1); }
      presSend(s, "{\"type\":\"dsnapok\",\"id\":" + std::to_string(s->id) + ",\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) + ",\"raw\":" + (raw ? "true" : "false") + ",\"y\":\"" + y + "\"}");
    }
    else presSend(s, "{\"type\":\"dsnaperr\",\"id\":" + std::to_string(s->id) + ",\"message\":\"" + jsonEsc(err) + "\"}");
  } else if (c->kind == 7){   // pedir de novo um quadro completo (o Telão vai pedir para quem transmite)
    s->waitKey = true;
  }
}
/* Teste padrão: reduz o último quadro decodificado para w×h na placa (mesmo processador de vídeo da tela), traz para a
   memória e devolve só a luminância em base64. A página compara com a cena de referência (nota SSIM). Uma por segundo. */
static bool streamSnap(DecStream* s, int w, int h, std::string* out, std::string* err){
  if (!s->last){ *err = "sem quadro ainda"; return false; }
  ID3D11Texture2D* tex = nullptr; UINT sub = 0;
  { IMFMediaBuffer* mb = nullptr; IMFDXGIBuffer* db = nullptr;
    if (SUCCEEDED(s->last->GetBufferByIndex(0, &mb)) && SUCCEEDED(mb->QueryInterface(__uuidof(IMFDXGIBuffer), (void**)&db))){ db->GetResource(__uuidof(ID3D11Texture2D), (void**)&tex); db->GetSubresourceIndex(&sub); }
    rel(db); rel(mb); }
  if (!tex){ *err = "quadro fora da placa"; return false; }
  D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
  if (w <= 0){
    // modo exato: a luminância do quadro decodificado (plano Y do NV12, sem conversão de cor nem redução da placa), em
    // média 2×2. A página reduz a referência do mesmo jeito: a nota mede só o que a compressão perdeu.
    if (td.Format != DXGI_FORMAT_NV12){ tex->Release(); *err = "quadro não está em NV12"; return false; }
    D3D11_TEXTURE2D_DESC sd = td; sd.ArraySize = 1; sd.MipLevels = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; bool ok = false;
    if (SUCCEEDED(gPr.dev->CreateTexture2D(&sd, nullptr, &st))){
      gPr.ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, sub, nullptr);
      D3D11_MAPPED_SUBRESOURCE m;
      if (SUCCEEDED(gPr.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))){
        int vw = std::min(s->vw, (int)td.Width - s->cropX), vh = std::min(s->vh, (int)td.Height - s->cropY), ow = vw / 2, oh = vh / 2;
        std::vector<uint8_t> yv((size_t)ow * oh);
        for (int r = 0; r < oh; r++){
          const uint8_t* a = (const uint8_t*)m.pData + (size_t)(s->cropY + 2 * r) * m.RowPitch + s->cropX, *b = a + m.RowPitch;
          for (int x = 0; x < ow; x++) yv[(size_t)r * ow + x] = (uint8_t)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
        }
        gPr.ctx->Unmap(st, 0);
        *out = std::to_string(ow) + "," + std::to_string(oh) + "," + base64(yv.data(), yv.size());
        ok = ow > 0 && oh > 0;
      } else *err = "leitura";
      st->Release();
    } else *err = "textura de leitura";
    tex->Release();
    return ok;
  }
  ID3D11VideoProcessorEnumerator* vpe = nullptr; ID3D11VideoProcessor* vp = nullptr; ID3D11VideoProcessorInputView* iv = nullptr; ID3D11VideoProcessorOutputView* ov = nullptr;
  ID3D11Texture2D* rt = nullptr; ID3D11Texture2D* st = nullptr; bool ok = false;
  do {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE; cd.InputWidth = td.Width; cd.InputHeight = td.Height; cd.OutputWidth = w; cd.OutputHeight = h;
    cd.InputFrameRate = {60, 1}; cd.OutputFrameRate = {60, 1}; cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(gPr.vdev->CreateVideoProcessorEnumerator(&cd, &vpe)) || FAILED(gPr.vdev->CreateVideoProcessor(vpe, 0, &vp))){ *err = "processador de vídeo"; break; }
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {}; in.YCbCr_Matrix = 1; in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE o = {}; o.RGB_Range = 0;
    gPr.vctx->VideoProcessorSetStreamColorSpace(vp, 0, &in); gPr.vctx->VideoProcessorSetOutputColorSpace(vp, &o);
    gPr.vctx->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    gPr.vctx->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    D3D11_TEXTURE2D_DESC rd = {}; rd.Width = w; rd.Height = h; rd.MipLevels = 1; rd.ArraySize = 1; rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(gPr.dev->CreateTexture2D(&rd, nullptr, &rt))){ *err = "textura"; break; }
    rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gPr.dev->CreateTexture2D(&rd, nullptr, &st))){ *err = "textura de leitura"; break; }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {}; ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; ivd.Texture2D.ArraySlice = sub;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(gPr.vdev->CreateVideoProcessorInputView(tex, vpe, &ivd, &iv)) || FAILED(gPr.vdev->CreateVideoProcessorOutputView(rt, vpe, &ovd, &ov))){ *err = "vistas do processador"; break; }
    RECT src = {s->cropX, s->cropY, s->cropX + s->vw, s->cropY + s->vh}, dst = {0, 0, w, h};
    gPr.vctx->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &src);
    gPr.vctx->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &dst);
    gPr.vctx->VideoProcessorSetOutputTargetRect(vp, TRUE, &dst);
    D3D11_VIDEO_PROCESSOR_STREAM vs = {}; vs.Enable = TRUE; vs.pInputSurface = iv;
    if (FAILED(gPr.vctx->VideoProcessorBlt(vp, ov, 0, 1, &vs))){ *err = "redução"; break; }
    gPr.ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gPr.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))){ *err = "leitura"; break; }
    std::vector<uint8_t> yv((size_t)w * h);
    for (int r = 0; r < h; r++){
      const uint8_t* px = (const uint8_t*)m.pData + (size_t)r * m.RowPitch;
      for (int x = 0; x < w; x++, px += 4) yv[(size_t)r * w + x] = (uint8_t)((19 * px[0] + 183 * px[1] + 54 * px[2] + 128) >> 8);   // BGRA → luminância (BT.709)
    }
    gPr.ctx->Unmap(st, 0);
    *out = base64(yv.data(), yv.size());
    ok = true;
  } while (false);
  rel(ov); rel(iv); rel(st); rel(rt); rel(vp); rel(vpe); tex->Release();
  return ok;
}
static DWORD WINAPI presThread(void*){
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = mfReady() && videoApis();
  if (ok){
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &gPr.dev, nullptr, &gPr.ctx);
    if (FAILED(hr)) hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl + 1, 1, D3D11_SDK_VERSION, &gPr.dev, nullptr, &gPr.ctx);
    ok = SUCCEEDED(hr);
    if (ok){
      gPr.dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&gPr.vdev);
      gPr.ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&gPr.vctx);
      ID3D10Multithread* mt = nullptr;
      if (SUCCEEDED(gPr.ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))){ mt->SetMultithreadProtected(TRUE); mt->Release(); }
      IDXGIDevice* dx = nullptr; IDXGIAdapter* ad = nullptr;
      if (SUCCEEDED(gPr.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx)) && SUCCEEDED(dx->GetAdapter(&ad))){
        DXGI_ADAPTER_DESC d; if (SUCCEEDED(ad->GetDesc(&d))) gPr.adapter = utf8(d.Description);
        ad->GetParent(__uuidof(IDXGIFactory2), (void**)&gPr.fac);
      }
      ok = gPr.vdev && gPr.vctx && gPr.fac && SUCCEEDED(gMF.CreateDXGIDeviceManager(&gPr.tok, &gPr.mgr)) && SUCCEEDED(gPr.mgr->ResetDevice(gPr.dev, gPr.tok));
      HMODULE d2m = LoadLibraryW(L"d2d1.dll"), dwm = LoadLibraryW(L"dwrite.dll");
      auto mkD2 = d2m ? (PFN_D2D1CreateFactory)GetProcAddress(d2m, "D2D1CreateFactory") : nullptr;
      auto mkDw = dwm ? (PFN_DWriteCreateFactory)GetProcAddress(dwm, "DWriteCreateFactory") : nullptr;
      if (ok && mkD2 && mkDw && dx && SUCCEEDED(mkD2(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&gPr.d2)) &&
          SUCCEEDED(mkDw(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gPr.dw)))
        gPr.d2->CreateDevice(dx, &gPr.d2dev);   // sem isso, só o vídeo (sem reações por cima)
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&gPr.wic);
      rel(ad); rel(dx);
    }
    if (ok){
      WNDCLASSEXW wc{}; wc.cbSize = sizeof wc; wc.style = CS_DBLCLKS; wc.lpfnWndProc = vidProc; wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"TelaoVideo"; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
      RegisterClassExW(&wc);
    }
  }
  gPresTid = GetCurrentThreadId();
  InterlockedExchange(&gPresState, ok ? 1 : -1);
  logf(ok ? "Motor: janela de vídeo pronta (placa %s)" : "Motor: não consegui preparar a decodificação na placa", gPr.adapter.c_str());
  SetEvent(gPresReady);
  if (!ok){ CoUninitialize(); return 0; }
  double statT = nowMs();
  for (;;){
    bool anim = false;
    for (auto* s : gStreams) if (s->visible && (!s->ov.items.empty() || (s->full && nowMs() < s->fsUntil + 100))) anim = true;
    MsgWaitForMultipleObjects(1, &gPresWake, FALSE, anim ? 16 : 100, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    std::vector<PresCmd*> cmds;
    EnterCriticalSection(&gPresLock); cmds.swap(gPresCmds); LeaveCriticalSection(&gPresLock);
    for (auto* c : cmds){
      if (c->kind == 9){   // a conexão fechou: fecha as telas dela
        for (size_t i = 0; i < gStreams.size(); ){ if (gStreams[i]->conn == c->conn){ streamFree(gStreams[i]); delete gStreams[i]; gStreams.erase(gStreams.begin() + i); } else i++; }
      } else presApply(c);
      if (c->done) SetEvent(c->done);
      else delete c;
    }
    double now = nowMs();
    for (auto* s : gStreams) if (s->visible && s->wnd){   // rede de segurança do acompanhar (minimizar/restaurar, eventos perdidos)
      bool shown = IsWindowVisible(s->wnd) != FALSE, ownerOk = !s->owner || (IsWindow(s->owner) && IsWindowVisible(s->owner) && !IsIconic(s->owner));
      POINT o = ownerOrigin(s);
      if (shown != ownerOk || (ownerOk && (o.x != s->org.x || o.y != s->org.y))) streamPlace(s);
    }
    for (auto* s : gStreams){
      size_t n; EnterCriticalSection(&gPresLock); n = s->q.size(); LeaveCriticalSection(&gPresLock);
      if (n) streamDecode(s);
      else if (s->visible && (!s->ov.items.empty() || (s->full && now < s->fsUntil + 100))) streamPresent(s);   // reações animando sobre o último quadro
    }
    if (now - statT >= 2000){
      double secs = (now - statT) / 1000;
      for (auto* s : gStreams){
        double avg = 0; for (double x : s->decMs) avg += x; if (!s->decMs.empty()) avg /= s->decMs.size();
        char b[300]; snprintf(b, sizeof b, "{\"type\":\"dstat\",\"id\":%u,\"in\":%.1f,\"dec\":%.1f,\"pres\":%.1f,\"drop\":%d,\"freezes\":%d,\"w\":%d,\"h\":%d,\"ms\":%.2f}",
                              s->id, s->nIn / secs, s->nDec / secs, s->nPres / secs, s->nDrop, s->freezes, s->vw, s->vh, avg);
        presSend(s, b);
        s->nIn = s->nDec = s->nPres = s->nDrop = s->freezes = 0; s->decMs.clear();
      }
      statT = now;
    }
  }
  return 0;
}
static bool presEnsure(){
  EnterCriticalSection(&gPresLock);
  if (!gPresThread){ gPresWake = CreateEventW(nullptr, FALSE, FALSE, nullptr); gPresReady = CreateEventW(nullptr, TRUE, FALSE, nullptr); gPresThread = CreateThread(nullptr, 0, presThread, nullptr, 0, nullptr); }
  LeaveCriticalSection(&gPresLock);
  if (gPresReady) WaitForSingleObject(gPresReady, 8000);
  return gPresState == 1;
}
static void presPost(PresCmd* c, bool wait){
  if (wait) c->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  EnterCriticalSection(&gPresLock); gPresCmds.push_back(c); LeaveCriticalSection(&gPresLock);
  SetEvent(gPresWake);
  if (wait){ WaitForSingleObject(c->done, 5000); CloseHandle(c->done); delete c; }
}
// Quadro comprimido que chegou do Telão (binário "TDF1")
static void presFrame(Conn* c, const std::string& p){
  uint32_t id, flags, seq; uint64_t ts;
  if (gPresState != 1 || !engParse((const uint8_t*)p.data(), p.size(), "TDF1", &id, &flags, &seq, &ts)) return;
  EnterCriticalSection(&gPresLock);
  for (auto* s : gStreams) if (s->id == id && s->conn == c){
    if (s->q.size() > 30){ s->q.clear(); s->waitKey = true; wsText(c, "{\"type\":\"dneedkey\",\"id\":" + std::to_string(id) + "}"); }   // atrasou demais: recomeça no próximo quadro completo
    DecFrame f; f.data.assign(p.begin() + 24, p.end()); f.key = flags & 1; f.seq = seq;
    s->q.push_back(std::move(f));
    break;
  }
  LeaveCriticalSection(&gPresLock);
  SetEvent(gPresWake);
}
static std::vector<RECT> parseHoles(const std::string& s){
  std::vector<RECT> v; const char* p = s.c_str();
  while (*p && v.size() < 32){
    long a[4]; int k = 0;
    for (; k < 4 && *p; k++){ char* e = nullptr; a[k] = strtol(p, &e, 10); if (e == p) break; p = e; if (*p == ',') p++; }
    if (k < 4) break;
    v.push_back({a[0], a[1], a[0] + a[2], a[1] + a[3]});
    if (*p == ';') p++;
  }
  return v;
}

/* ---------- comandos do motor ---------- */
static std::string engCaps(){
  bool mf = mfReady();
  std::string nvWhy; bool nvOk = nvLoad(&nvWhy);
  std::string encNames, encNames5, decNames5;
  int e4 = mf ? mftCount(kMFT_CAT_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, FCC_NV12, FCC_H264, &encNames) : 0;
  int e5 = mf ? mftCount(kMFT_CAT_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, FCC_NV12, FCC_HEVC, &encNames5) : 0;
  int d5 = mf ? mftCount(kMFT_CAT_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, FCC_HEVC, FCC_NV12, &decNames5) : 0;
  return std::string("{\"type\":\"vcaps\",\"v\":1,\"mf\":") + (mf ? "true" : "false") + ",\"enc\":{\"h264\":" + (e4 ? "true" : "false") + ",\"h265\":" + (e5 ? "true" : "false") +
         "},\"dec\":{\"h264\":" + (mf ? "true" : "false") + ",\"h265\":" + (d5 ? "true" : "false") + "},\"encNames\":\"" + jsonEsc(encNames + (encNames5.empty() ? "" : " | " + encNames5)) +
         "\",\"decNames\":\"" + jsonEsc(decNames5) + "\",\"snap\":true,\"crash\":\"" + jsonEsc(gLastCrash.substr(0, 1500)) + "\",\"nvenc\":" + (nvOk ? "true" : "false") + ",\"nvencWhy\":\"" + jsonEsc(nvWhy) + "\"}";
}
static bool engineCommand(Conn* c, const std::string& cmd, const std::string& msg){
  if (cmd.size() < 2 || (cmd[0] != 'e' && cmd[0] != 'd' && cmd != "vcaps")) return false;
  static const char* known[] = {"vcaps", "ecap", "estop", "esess", "eclose", "ekey", "erate", "dopen", "dclose", "drect", "dreact", "dfull", "dkey", "dsnap", nullptr};
  bool isEng = false; for (int i = 0; known[i]; i++) if (cmd == known[i]) isEng = true;
  if (!isEng) return false;
  if (!c->app){ wsText(c, "{\"type\":\"eerror\",\"message\":\"o motor de vídeo só funciona no app do Telão\"}"); return true; }
  if (cmd == "vcaps"){ wsText(c, engCaps()); return true; }
  if (cmd == "ecap"){
    engStop(nullptr);
    Eng* e = new Eng(); e->conn = c; InitializeCriticalSection(&e->lock);
    e->vendor = (UINT)strtoul(jsonStr(msg, "vendor").c_str(), nullptr, 10);
    e->device = (UINT)strtoul(jsonStr(msg, "device").c_str(), nullptr, 10);
    e->ddAllowed = jsonStr(msg, "dd") == "1";
    if (jsonStr(msg, "target") == "monitor"){
      e->monitor = true; POINT pt = {atol(jsonStr(msg, "x").c_str()), atol(jsonStr(msg, "y").c_str())};
      e->mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    } else {
      unsigned long long hv = strtoull(jsonStr(msg, "hwnd").c_str(), nullptr, 10);
      e->hwnd = (HWND)(uintptr_t)hv;
      if (!hv || !IsWindow(e->hwnd)){ DeleteCriticalSection(&e->lock); delete e; wsText(c, "{\"type\":\"ecaperr\",\"message\":\"a janela escolhida não existe mais\"}"); return true; }
    }
    EnterCriticalSection(&gEngLock);
    gEng = e;
    e->thread = CreateThread(nullptr, 0, engThread, e, 0, nullptr);
    if (!e->thread){ gEng = nullptr; DeleteCriticalSection(&e->lock); delete e; wsText(c, "{\"type\":\"ecaperr\",\"message\":\"não consegui iniciar a captura\"}"); }
    LeaveCriticalSection(&gEngLock);
    return true;
  }
  if (cmd == "estop"){ engStop(c); return true; }
  if (cmd == "esess"){
    EncSes* s = new EncSes();
    s->id = (uint32_t)strtoul(jsonStr(msg, "id").c_str(), nullptr, 10);
    s->boxW = std::max(160, std::min(3840, atoi(jsonStr(msg, "w").c_str())));
    s->boxH = std::max(90, std::min(2160, atoi(jsonStr(msg, "h").c_str())));
    s->fps = std::max(5, std::min(240, atoi(jsonStr(msg, "fps").c_str())));
    s->kbps = std::max(200, std::min(80000, atoi(jsonStr(msg, "kbps").c_str())));
    s->hevc = jsonStr(msg, "codec") == "h265";
    std::string pr = jsonStr(msg, "profile"); s->profile = pr == "base" ? 0 : pr == "high" ? 2 : 1;
    s->useNv = jsonStr(msg, "nv") != "0";
    EngCmd ec; ec.kind = 1; ec.ses = s; engPost(c, ec);
    return true;
  }
  if (cmd == "eclose" || cmd == "ekey" || cmd == "erate"){
    EngCmd ec; ec.kind = cmd == "eclose" ? 2 : cmd == "ekey" ? 3 : 4;
    ec.id = (uint32_t)strtoul(jsonStr(msg, "id").c_str(), nullptr, 10);
    ec.kbps = atoi(jsonStr(msg, "kbps").c_str());
    engPost(c, ec);
    return true;
  }
  // quem assiste
  if (cmd == "dopen"){
    if (!presEnsure()){ wsText(c, "{\"type\":\"dopenerr\",\"id\":" + jsonStr(msg, "id") + ",\"message\":\"não consegui preparar a decodificação na placa\"}"); return true; }
    PresCmd* p = new PresCmd(); p->kind = 1; p->conn = c;
    p->id = (uint32_t)strtoul(jsonStr(msg, "id").c_str(), nullptr, 10);
    p->a = jsonStr(msg, "codec"); p->owner = strtoull(jsonStr(msg, "owner").c_str(), nullptr, 10);
    presPost(p, false);
    return true;
  }
  if (gPresState != 1) return true;
  PresCmd* p = new PresCmd(); p->conn = c;
  p->id = (uint32_t)strtoul(jsonStr(msg, "id").c_str(), nullptr, 10);
  if (cmd == "dclose") p->kind = 2;
  else if (cmd == "drect"){
    p->kind = 3; p->on = jsonStr(msg, "show") == "1";
    p->x = atoi(jsonStr(msg, "x").c_str()); p->y = atoi(jsonStr(msg, "y").c_str()); p->w = atoi(jsonStr(msg, "w").c_str()); p->h = atoi(jsonStr(msg, "h").c_str());
    p->holes = parseHoles(jsonStr(msg, "holes"));
  } else if (cmd == "dreact"){
    p->kind = 4; p->a = jsonStr(msg, "text"); p->b = jsonStr(msg, "who"); p->c = jsonStr(msg, "img"); p->d = jsonStr(msg, "kind");
    if (p->c.size() > 1200000) p->c.clear();
  } else if (cmd == "dfull"){
    p->kind = 5; p->on = jsonStr(msg, "on") == "1"; p->a = jsonStr(msg, "name"); p->b = jsonStr(msg, "sub"); p->c = jsonStr(msg, "reacts");
  } else if (cmd == "dkey") p->kind = 7;
  else if (cmd == "dsnap"){ p->kind = 6; p->w = atoi(jsonStr(msg, "w").c_str()); p->h = atoi(jsonStr(msg, "h").c_str()); }
  presPost(p, false);
  return true;
}
static void engineClosed(Conn* c){
  engStop(c);
  if (gPresState == 1){ PresCmd* p = new PresCmd(); p->kind = 9; p->conn = c; presPost(p, true); }
}
