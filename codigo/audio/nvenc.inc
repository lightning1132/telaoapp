/* ======================= NVENC direto (placas NVIDIA) =======================
   O motor fala direto com o codificador da NVIDIA (Video Codec SDK, API 11.1), sem a Media Foundation no meio.
   O que isso dá e a Media Foundation não deixava escolher:
     - modo "plateia": preset P4 com ajuste de alta qualidade, primeira passada em 1/4 da resolução, AQ espacial (e temporal,
       quando a placa tem), sem quadros B;
     - taxa constante com buffer de ~0,4 s (a cota de bits é por janela, não por quadro: cena de ação pega bits das paradas);
     - sem quadro completo periódico: renovação gradual (intra refresh) a cada 3 s corrige perdas sem o pico de um quadro
       completo; quadro completo só quando alguém pede (no máximo 1 por segundo);
     - número de QP de cada quadro (qualidade de verdade) para o relatório.
   Se qualquer passo falhar, a sessão abre pela Media Foundation, como antes. Se o Telão Audio cair dentro do NVENC, na
   próxima vez ele não usa o NVENC (marca [nvenc] no relatório de erro). */
#include "nvEncodeAPI.h"

struct NvApi { HMODULE dll = nullptr; NV_ENCODE_API_FUNCTION_LIST f{}; bool ok = false, tried = false; std::string err; uint32_t maxVer = 0; };
static NvApi gNv;
static CRITICAL_SECTION gNvLock;
static bool nvLoad(std::string* why){
  EnterCriticalSection(&gNvLock);
  if (!gNv.tried){
    gNv.tried = true;
    gNv.dll = LoadLibraryW(L"nvEncodeAPI64.dll");
    typedef NVENCSTATUS (NVENCAPI *PFN_Max)(uint32_t*);
    typedef NVENCSTATUS (NVENCAPI *PFN_Create)(NV_ENCODE_API_FUNCTION_LIST*);
    PFN_Max getMax = gNv.dll ? (PFN_Max)GetProcAddress(gNv.dll, "NvEncodeAPIGetMaxSupportedVersion") : nullptr;
    PFN_Create create = gNv.dll ? (PFN_Create)GetProcAddress(gNv.dll, "NvEncodeAPICreateInstance") : nullptr;
    const uint32_t need = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (!gNv.dll) gNv.err = "sem o NVENC (placa ou driver sem nvEncodeAPI64.dll)";
    else if (!getMax || !create) gNv.err = "NVENC incompleto neste driver";
    else if (getMax(&gNv.maxVer) != NV_ENC_SUCCESS || gNv.maxVer < need)
      gNv.err = "driver da NVIDIA antigo para o NVENC direto (API " + std::to_string(gNv.maxVer >> 4) + "." + std::to_string(gNv.maxVer & 15) + "; precisa 11.1)";
    else {
      gNv.f.version = NV_ENCODE_API_FUNCTION_LIST_VER;
      if (create(&gNv.f) != NV_ENC_SUCCESS || !gNv.f.nvEncOpenEncodeSessionEx) gNv.err = "o driver recusou abrir o NVENC";
      else gNv.ok = true;
    }
  }
  bool ok = gNv.ok && gNvOff.empty();
  if (why) *why = !gNvOff.empty() ? gNvOff : gNv.err;
  LeaveCriticalSection(&gNvLock);
  return ok;
}

struct NvSes {
  void* enc = nullptr; int w = 0, h = 0, fps = 60; bool hevc = false;
  NV_ENC_INITIALIZE_PARAMS init{}; NV_ENC_CONFIG cfg{};
  std::vector<NV_ENC_REGISTERED_PTR> reg; std::vector<NV_ENC_OUTPUT_PTR> bs; int bsi = 0;
  std::string desc;
};
struct NvGuard { NvGuard(){ InterlockedExchange(&gInNvenc, 1); } ~NvGuard(){ InterlockedExchange(&gInNvenc, 0); } };
static std::string nvErr(const char* what, NVENCSTATUS st, void* enc){
  const char* m = enc && gNv.f.nvEncGetLastErrorString ? gNv.f.nvEncGetLastErrorString(enc) : nullptr;
  return std::string(what) + " (NVENC " + std::to_string((int)st) + (m && *m ? std::string(": ") + m : "") + ")";
}
static void nvFree(NvSes* n){
  if (!n) return;
  NvGuard g;
  if (n->enc){
    for (auto r : n->reg) gNv.f.nvEncUnregisterResource(n->enc, r);
    for (auto b : n->bs) gNv.f.nvEncDestroyBitstreamBuffer(n->enc, b);
    gNv.f.nvEncDestroyEncoder(n->enc);
  }
  delete n;
}
static void nvRateParams(NV_ENC_CONFIG& c, int kbps){
  uint32_t bps = (uint32_t)std::max(200, kbps) * 1000;
  c.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  c.rcParams.averageBitRate = bps; c.rcParams.maxBitRate = bps;
  c.rcParams.vbvBufferSize = (uint32_t)(bps * 0.4); c.rcParams.vbvInitialDelay = c.rcParams.vbvBufferSize;   // cota por ~0,4 s
}
// Abre uma sessão NVENC no device da captura, com as imagens do rodízio já criadas (NV12). "" = deu certo.
static std::string nvOpen(NvSes** out, ID3D11Device* dev, const std::vector<ID3D11Texture2D*>& ring, int w, int h, int fps, int kbps, bool hevc, int profile){
  *out = nullptr;
  NvGuard g;
  NvSes* n = new NvSes(); n->w = w; n->h = h; n->fps = fps; n->hevc = hevc;
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {}; op.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; op.device = dev; op.apiVersion = NVENCAPI_VERSION;
  NVENCSTATUS st = gNv.f.nvEncOpenEncodeSessionEx(&op, &n->enc);
  if (st != NV_ENC_SUCCESS){ std::string e = nvErr("o NVENC não abriu", st, nullptr); n->enc = nullptr; nvFree(n); return e; }
  const GUID codec = hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
  const GUID preset = fps > 90 ? NV_ENC_PRESET_P2_GUID : NV_ENC_PRESET_P4_GUID;   // 120 fps: preset mais rápido
  // temporal AQ só onde a placa tem
  int tAQ = 0;
  { NV_ENC_CAPS_PARAM cp = {}; cp.version = NV_ENC_CAPS_PARAM_VER; cp.capsToQuery = NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ; gNv.f.nvEncGetEncodeCaps(n->enc, codec, &cp, &tAQ); }
  NV_ENC_PRESET_CONFIG pc = {}; pc.version = NV_ENC_PRESET_CONFIG_VER; pc.presetCfg.version = NV_ENC_CONFIG_VER;
  st = gNv.f.nvEncGetEncodePresetConfigEx(n->enc, codec, preset, NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc);
  if (st != NV_ENC_SUCCESS){ std::string e = nvErr("o NVENC não deu a configuração do preset", st, n->enc); nvFree(n); return e; }
  NV_ENC_CONFIG& c = n->cfg; c = pc.presetCfg; c.version = NV_ENC_CONFIG_VER;
  c.gopLength = NVENC_INFINITE_GOPLENGTH; c.frameIntervalP = 1;   // sem quadros B (o WebRTC não lida bem com quadros fora de ordem)
  nvRateParams(c, kbps);
  c.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
  c.rcParams.enableLookahead = 0; c.rcParams.zeroReorderDelay = 1;
  c.rcParams.enableAQ = 1; c.rcParams.aqStrength = 0;   // 0 = o driver escolhe a força
  c.rcParams.enableTemporalAQ = tAQ ? 1 : 0;
  const uint32_t irPeriod = (uint32_t)fps * 3, irCnt = (uint32_t)std::max(8, fps / 2);   // renovação gradual: a cada 3 s, em ~0,5 s
  if (hevc){
    NV_ENC_CONFIG_HEVC& x = c.encodeCodecConfig.hevcConfig;
    c.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
    x.idrPeriod = NVENC_INFINITE_GOPLENGTH; x.repeatSPSPPS = 1; x.chromaFormatIDC = 1; x.pixelBitDepthMinus8 = 0;
    x.enableIntraRefresh = 1; x.intraRefreshPeriod = irPeriod; x.intraRefreshCnt = irCnt;
    x.hevcVUIParameters.videoSignalTypePresentFlag = 1; x.hevcVUIParameters.videoFullRangeFlag = 0;
    x.hevcVUIParameters.colourDescriptionPresentFlag = 1;
    x.hevcVUIParameters.videoFormat = 5; x.hevcVUIParameters.colourPrimaries = 1; x.hevcVUIParameters.transferCharacteristics = 1;   // BT.709, faixa limitada (o que o processador de vídeo entrega)
    x.hevcVUIParameters.colourMatrix = 1;
  } else {
    NV_ENC_CONFIG_H264& x = c.encodeCodecConfig.h264Config;
    c.profileGUID = profile == 0 ? NV_ENC_H264_PROFILE_BASELINE_GUID : profile == 2 ? NV_ENC_H264_PROFILE_HIGH_GUID : NV_ENC_H264_PROFILE_MAIN_GUID;
    if (profile == 0) x.entropyCodingMode = NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC;   // baseline restrita: sem CABAC
    x.idrPeriod = NVENC_INFINITE_GOPLENGTH; x.repeatSPSPPS = 1; x.chromaFormatIDC = 1;
    x.enableIntraRefresh = 1; x.intraRefreshPeriod = irPeriod; x.intraRefreshCnt = irCnt;
    x.h264VUIParameters.videoSignalTypePresentFlag = 1; x.h264VUIParameters.videoFullRangeFlag = 0;
    x.h264VUIParameters.colourDescriptionPresentFlag = 1;
    x.h264VUIParameters.videoFormat = 5; x.h264VUIParameters.colourPrimaries = 1; x.h264VUIParameters.transferCharacteristics = 1;   // BT.709, faixa limitada (o que o processador de vídeo entrega)
    x.h264VUIParameters.colourMatrix = 1;
  }
  NV_ENC_INITIALIZE_PARAMS& ip = n->init; ip = {}; ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
  ip.encodeGUID = codec; ip.presetGUID = preset; ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
  ip.encodeWidth = w; ip.encodeHeight = h; ip.darWidth = w; ip.darHeight = h; ip.maxEncodeWidth = w; ip.maxEncodeHeight = h;
  ip.frameRateNum = fps; ip.frameRateDen = 1; ip.enablePTD = 1; ip.enableEncodeAsync = 0; ip.encodeConfig = &n->cfg;
  st = gNv.f.nvEncInitializeEncoder(n->enc, &ip);
  if (st != NV_ENC_SUCCESS && c.rcParams.enableTemporalAQ){ c.rcParams.enableTemporalAQ = 0; tAQ = 0; st = gNv.f.nvEncInitializeEncoder(n->enc, &ip); }
  if (st != NV_ENC_SUCCESS && c.rcParams.multiPass){ c.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED; st = gNv.f.nvEncInitializeEncoder(n->enc, &ip); }
  if (st != NV_ENC_SUCCESS){ std::string e = nvErr("o NVENC recusou a configuração", st, n->enc); nvFree(n); return e; }
  for (auto* t : ring){
    NV_ENC_REGISTER_RESOURCE rr = {}; rr.version = NV_ENC_REGISTER_RESOURCE_VER;
    rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; rr.width = w; rr.height = h; rr.pitch = 0;
    rr.resourceToRegister = t; rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12; rr.bufferUsage = NV_ENC_INPUT_IMAGE;
    st = gNv.f.nvEncRegisterResource(n->enc, &rr);
    if (st != NV_ENC_SUCCESS){ std::string e = nvErr("o NVENC não aceitou a imagem da placa", st, n->enc); nvFree(n); return e; }
    n->reg.push_back(rr.registeredResource);
  }
  for (int i = 0; i < 3; i++){
    NV_ENC_CREATE_BITSTREAM_BUFFER cb = {}; cb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = gNv.f.nvEncCreateBitstreamBuffer(n->enc, &cb);
    if (st != NV_ENC_SUCCESS){ std::string e = nvErr("o NVENC não criou o buffer de saída", st, n->enc); nvFree(n); return e; }
    n->bs.push_back(cb.bitstreamBuffer);
  }
  n->desc = std::string("NVENC direto ") + (preset == NV_ENC_PRESET_P2_GUID ? "P2" : "P4") + " qualidade, 2 passadas, AQ" + (tAQ ? "+temporal" : "") + ", renovação gradual";
  *out = n;
  return "";
}
static bool nvRate(NvSes* n, int kbps){
  NvGuard g;
  nvRateParams(n->cfg, kbps);
  NV_ENC_RECONFIGURE_PARAMS rp = {}; rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  rp.reInitEncodeParams = n->init; rp.reInitEncodeParams.encodeConfig = &n->cfg; rp.resetEncoder = 0; rp.forceIDR = 0;
  return gNv.f.nvEncReconfigureEncoder(n->enc, &rp) == NV_ENC_SUCCESS;
}
// Placa sem folga (codificação demorando mais que ~70% do tempo de um quadro): tira as 2 passadas e o AQ temporal.
static bool nvLighten(NvSes* n){
  NvGuard g;
  if (n->cfg.rcParams.multiPass == NV_ENC_MULTI_PASS_DISABLED && !n->cfg.rcParams.enableTemporalAQ) return false;
  n->cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED; n->cfg.rcParams.enableTemporalAQ = 0;
  NV_ENC_RECONFIGURE_PARAMS rp = {}; rp.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  rp.reInitEncodeParams = n->init; rp.reInitEncodeParams.encodeConfig = &n->cfg; rp.resetEncoder = 1; rp.forceIDR = 1;
  bool ok = gNv.f.nvEncReconfigureEncoder(n->enc, &rp) == NV_ENC_SUCCESS;
  if (ok) n->desc += " (leve: 1 passada)";
  return ok;
}
// Codifica a imagem do rodízio (síncrono: volta com o quadro pronto). Devolve false se falhou.
static bool nvEncode(NvSes* n, int slot, uint64_t ts, bool idr, std::vector<uint8_t>* outBuf, bool* key, int* qp, std::string* err){
  NvGuard g;
  NV_ENC_MAP_INPUT_RESOURCE mp = {}; mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER; mp.registeredResource = n->reg[slot];
  NVENCSTATUS st = gNv.f.nvEncMapInputResource(n->enc, &mp);
  if (st != NV_ENC_SUCCESS){ *err = nvErr("mapear a imagem", st, n->enc); return false; }
  NV_ENC_PIC_PARAMS pp = {}; pp.version = NV_ENC_PIC_PARAMS_VER;
  pp.inputWidth = n->w; pp.inputHeight = n->h; pp.inputBuffer = mp.mappedResource; pp.bufferFmt = mp.mappedBufferFmt;
  pp.outputBitstream = n->bs[n->bsi]; n->bsi = (n->bsi + 1) % (int)n->bs.size();
  pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME; pp.inputTimeStamp = ts;
  pp.encodePicFlags = idr ? (NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) : 0;
  st = gNv.f.nvEncEncodePicture(n->enc, &pp);
  bool ok = false;
  if (st == NV_ENC_SUCCESS){
    NV_ENC_LOCK_BITSTREAM lb = {}; lb.version = NV_ENC_LOCK_BITSTREAM_VER; lb.outputBitstream = pp.outputBitstream;
    st = gNv.f.nvEncLockBitstream(n->enc, &lb);
    if (st == NV_ENC_SUCCESS){
      const uint8_t* p = (const uint8_t*)lb.bitstreamBufferPtr;
      outBuf->assign(p, p + lb.bitstreamSizeInBytes);
      *key = lb.pictureType == NV_ENC_PIC_TYPE_IDR || lb.pictureType == NV_ENC_PIC_TYPE_I;
      *qp = (int)lb.frameAvgQP;
      gNv.f.nvEncUnlockBitstream(n->enc, lb.outputBitstream);
      ok = true;
    } else *err = nvErr("ler o quadro codificado", st, n->enc);
  } else *err = nvErr("codificar", st, n->enc);
  gNv.f.nvEncUnmapInputResource(n->enc, mp.mappedResource);
  return ok;
}
