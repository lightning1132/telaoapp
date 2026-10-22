// Telão Audio — captura o áudio de um app específico no Windows e entrega para o Telão.
//
// Como funciona:
//   - Usa a captura de áudio por processo do Windows (process loopback, Windows 10 2004+).
//     O Windows entrega só o som do processo escolhido (e dos filhos dele), sem tocar no jogo.
//   - Abre um WebSocket só em 127.0.0.1:47800. O Telão (no navegador) pede a lista de apps
//     com áudio, escolhe um e recebe o PCM (48 kHz, estéreo, 16 bits).
//   - Só a página aberta como arquivo local é aceita automaticamente. Qualquer site precisa
//     ser aprovado aqui no console (fica salvo em telao-audio-origens.txt).
//
// Build (de qualquer SO, com Zig):
//   zig c++ -target x86_64-windows-gnu -O2 -o telao-audio.exe telao-audio.cpp -lole32 -lws2_32 -luser32

#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

/* ======================= Partes portáveis (testáveis fora do Windows) ======================= */

static void sha1(const uint8_t* data, size_t len, uint8_t out[20]){
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  std::vector<uint8_t> m(data, data + len);
  uint64_t bits = (uint64_t)len * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (i * 8)));
  auto rol = [](uint32_t x, int n){ return (x << n) | (x >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64){
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)m[off + i*4] << 24 | (uint32_t)m[off + i*4 + 1] << 16 | (uint32_t)m[off + i*4 + 2] << 8 | m[off + i*4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++){
      uint32_t f, k;
      if (i < 20){ f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40){ f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; i++){
    out[i*4] = hs[i] >> 24; out[i*4+1] = hs[i] >> 16; out[i*4+2] = hs[i] >> 8; out[i*4+3] = hs[i];
  }
}

static std::string base64(const uint8_t* d, size_t n){
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  for (size_t i = 0; i < n; i += 3){
    uint32_t v = d[i] << 16 | (i + 1 < n ? d[i+1] << 8 : 0) | (i + 2 < n ? d[i+2] : 0);
    o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
    o += i + 1 < n ? t[(v >> 6) & 63] : '=';
    o += i + 2 < n ? t[v & 63] : '=';
  }
  return o;
}

static std::string wsAcceptKey(const std::string& key){
  std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t h[20]; sha1((const uint8_t*)s.data(), s.size(), h);
  return base64(h, 20);
}

static std::string lower(std::string s){ for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

// Valor de um header HTTP (nome sem diferenciar maiúsculas).
static std::string httpHeader(const std::string& req, const std::string& name){
  std::string want = lower(name);
  size_t pos = req.find("\r\n");
  while (pos != std::string::npos){
    size_t start = pos + 2, end = req.find("\r\n", start);
    if (end == std::string::npos || end == start) break;
    std::string line = req.substr(start, end - start);
    size_t colon = line.find(':');
    if (colon != std::string::npos && lower(line.substr(0, colon)) == want){
      std::string v = line.substr(colon + 1);
      size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
      return a == std::string::npos ? "" : v.substr(a, b - a + 1);
    }
    pos = end;
  }
  return "";
}

// Lê um campo string de um JSON simples ({"cmd":"start","name":"x.exe"}).
static std::string jsonStr(const std::string& j, const std::string& key){
  std::string pat = "\"" + key + "\"";
  size_t p = j.find(pat);
  if (p == std::string::npos) return "";
  p = j.find(':', p + pat.size());
  if (p == std::string::npos) return "";
  p = j.find('"', p);
  if (p == std::string::npos) return "";
  std::string out;
  for (size_t i = p + 1; i < j.size(); i++){
    char c = j[i];
    if (c == '\\' && i + 1 < j.size()){
      char n = j[++i];
      if (n == 'n') out += '\n'; else if (n == 't') out += '\t'; else out += n;
    } else if (c == '"') return out;
    else out += c;
  }
  return "";
}

static std::string jsonEsc(const std::string& s){
  std::string o;
  for (unsigned char c : s){
    if (c == '"' || c == '\\'){ o += '\\'; o += (char)c; }
    else if (c < 0x20){ char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o;
}

// Cabeçalho de frame WebSocket do servidor (sem máscara).
static std::string wsFrameHeader(int opcode, uint64_t len){
  std::string h;
  h += (char)(0x80 | opcode);
  if (len < 126) h += (char)len;
  else if (len < 65536){ h += (char)126; h += (char)(len >> 8); h += (char)(len & 0xFF); }
  else { h += (char)127; for (int i = 7; i >= 0; i--) h += (char)((len >> (i * 8)) & 0xFF); }
  return h;
}

// Processos descendentes de 'root' (inclui o próprio), só com o mesmo executável do root.
// O filtro por nome protege contra PID reutilizado por outro programa.
struct ProcInfo { uint32_t pid, parent; std::string exe; };
static std::vector<uint32_t> processTree(uint32_t root, const std::vector<ProcInfo>& ps){
  std::string exe;
  for (auto& p : ps) if (p.pid == root) exe = lower(p.exe);
  std::vector<uint32_t> tree;
  if (exe.empty()) return tree;
  tree.push_back(root);
  for (size_t i = 0; i < tree.size() && tree.size() < 512; i++)
    for (auto& p : ps)
      if (p.parent == tree[i] && p.pid != root && lower(p.exe) == exe){
        bool seen = false;
        for (auto q : tree) if (q == p.pid) seen = true;
        if (!seen) tree.push_back(p.pid);
      }
  return tree;
}

/* ---------- captura turbo de vídeo: partes portáveis ---------- */
// Tamanho de saída: cabe na "caixa" da qualidade escolhida (ex.: 1920x1080), mantém a proporção,
// nunca aumenta a imagem e sempre tem dimensões pares (exigência do NV12).
static void fitSize(int sw, int sh, int bw, int bh, int* ow, int* oh){
  if (sw < 2 || sh < 2){ *ow = 16; *oh = 16; return; }
  double k = std::min(1.0, std::min((double)bw / sw, (double)bh / sh));
  int w = ((int)(sw * k + 0.5)) & ~1, h = ((int)(sh * k + 0.5)) & ~1;
  *ow = std::max(16, w); *oh = std::max(16, h);
}
// Onde a imagem entra dentro da saída quando a proporção da janela muda no meio (faixas pretas, sem esticar).
struct IRect { int l, t, r, b; };
static IRect aspectRect(int sw, int sh, int ow, int oh){
  if (sw < 1 || sh < 1) return {0, 0, ow, oh};
  double k = std::min((double)ow / sw, (double)oh / sh);
  int w = std::min(ow, (int)(sw * k + 0.5)), h = std::min(oh, (int)(sh * k + 0.5));
  int l = ((ow - w) / 2) & ~1, t = ((oh - h) / 2) & ~1;
  return {l, t, l + w, t + h};
}
// Ritmo de saída: entrega no máximo 'fps' quadros por segundo e espaçados por igual,
// mesmo quando o Windows entrega mais (ex.: jogo a 120 com transmissão a 60).
struct Pacer {
  double period = 1000.0 / 60, next = -1;
  void setFps(int fps){ period = 1000.0 / std::max(1, fps); next = -1; }
  bool accept(double now){
    if (next < 0 || now - next > period){ next = now + period; return true; }   // começo ou atrasou demais: recomeça
    if (now >= next - period * 0.25){ next += period; return true; }
    return false;
  }
};
// Cabeçalho do quadro enviado pela memória: "TVF1", largura, altura, número do quadro (little-endian).
static void frameHeader(uint8_t* p, uint32_t w, uint32_t h, uint32_t seq){
  p[0] = 'T'; p[1] = 'V'; p[2] = 'F'; p[3] = '1';
  for (int i = 0; i < 4; i++){ p[4 + i] = (uint8_t)(w >> (8 * i)); p[8 + i] = (uint8_t)(h >> (8 * i)); p[12 + i] = (uint8_t)(seq >> (8 * i)); }
}

#ifdef TELAO_PORTABLE_TEST
int main(){
  int fails = 0;
  auto check = [&](bool ok, const char* what){ printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; };
  check(wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "handshake RFC 6455");
  uint8_t h[20]; sha1((const uint8_t*)"abc", 3, h);
  check(base64(h, 20) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=", "sha1(abc)");
  std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nsec-websocket-key: abc==\r\nOrigin: null\r\n\r\n";
  check(httpHeader(req, "Sec-WebSocket-Key") == "abc==", "header case-insensitive");
  check(httpHeader(req, "origin") == "null", "origin");
  check(jsonStr("{\"cmd\":\"start\",\"target\":\"app\",\"name\":\"Marvel-Win64-Shipping.exe\"}", "name") == "Marvel-Win64-Shipping.exe", "json name");
  check(jsonStr("{\"cmd\": \"list\"}", "cmd") == "list", "json with space");
  check(jsonEsc("a\"b\\c\n") == "a\\\"b\\\\c\\u000a", "json escape");
  std::string f = wsFrameHeader(2, 300);
  check(f.size() == 4 && (uint8_t)f[0] == 0x82 && (uint8_t)f[1] == 126 && (uint8_t)f[2] == 1 && (uint8_t)f[3] == 44, "frame 16-bit length");
  check(wsFrameHeader(1, 5) == std::string("\x81\x05"), "frame short length");
  {
    std::vector<ProcInfo> ps = {
      {100, 1, "Telao.exe"}, {101, 100, "Telao.exe"}, {102, 100, "telao.exe"}, {103, 101, "Telao.exe"},
      {104, 100, "telao-audio.exe"}, {105, 999, "Telao.exe"}, {106, 103, "notepad.exe"}, {107, 106, "Telao.exe"},
    };
    auto t = processTree(100, ps);
    auto has = [&](uint32_t x){ for (auto y : t) if (y == x) return true; return false; };
    check(t.size() == 4 && has(100) && has(101) && has(102) && has(103), "árvore: main + renderers/GPU (filhos e netos)");
    check(!has(104) && !has(105) && !has(107), "árvore: ignora outros programas e Telão de fora");
    check(processTree(555, ps).empty(), "árvore: root inexistente = vazio");
    std::vector<ProcInfo> loop = {{200, 201, "a.exe"}, {201, 200, "a.exe"}};
    check(processTree(200, loop).size() == 2, "árvore: ciclo de PIDs não trava");
  }
  {
    int w, h;
    fitSize(2560, 1440, 1920, 1080, &w, &h); check(w == 1920 && h == 1080, "turbo: 1440p na caixa 1080p");
    fitSize(3440, 1440, 1920, 1080, &w, &h); check(w == 1920 && h == 804, "turbo: ultrawide mantém proporção");
    fitSize(1280, 720, 1920, 1080, &w, &h); check(w == 1280 && h == 720, "turbo: não aumenta janela pequena");
    fitSize(1001, 563, 1920, 1080, &w, &h); check(w == 1000 && h == 562, "turbo: dimensões pares");
    fitSize(0, 0, 1920, 1080, &w, &h); check(w == 16 && h == 16, "turbo: tamanho inválido não quebra");
    IRect r = aspectRect(1920, 1080, 1920, 1080); check(r.l == 0 && r.t == 0 && r.r == 1920 && r.b == 1080, "turbo: mesma proporção ocupa tudo");
    r = aspectRect(1080, 1080, 1920, 1080); check(r.l == 420 && r.r == 1500 && r.t == 0 && r.b == 1080, "turbo: quadrada fica centralizada");
    auto run = [](double arrivalMs, int fps, double secs){ Pacer p; p.setFps(fps); int n = 0; for (double t = 0; t < secs * 1000; t += arrivalMs) n += p.accept(t); return n / secs; };
    double a = run(1000.0 / 120, 60, 10), b = run(15, 60, 10), c = run(1000.0 / 59, 60, 10), d = run(1000.0 / 240, 120, 10);
    check(a > 59 && a < 61, "ritmo: jogo a 120 → 60");
    check(b > 57 && b < 61.5, "ritmo: Windows a 66 → ~60");
    check(c > 58.5, "ritmo: Windows a 59 → passa tudo");
    check(d > 119 && d < 121, "ritmo: 240 → 120");
    uint8_t hd[16]; frameHeader(hd, 1920, 1080, 258);
    check(hd[0] == 'T' && hd[3] == '1' && hd[4] == 0x80 && hd[5] == 7 && hd[8] == 0x38 && hd[9] == 4 && hd[12] == 2 && hd[13] == 1, "cabeçalho do quadro");
  }
  return fails;
}
#else

/* ======================================== Windows ======================================== */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <winstring.h>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static const unsigned short PORT = 47800;
static const int RATE = 48000, CHANNELS = 2;

// GUIDs definidos aqui para não depender de libuuid.
static const GUID kIID_IUnknown        = {0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID kIID_IAgileObject    = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID kIID_ActivateHandler = {0x41D949AB,0x9862,0x444A,{0x80,0xF6,0xC2,0x61,0x33,0x4D,0xA5,0xEB}};
static const GUID kIID_IAudioClient    = {0x1CB9AD4C,0xDBFA,0x4c32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID kIID_IAudioCapture   = {0xC8ADBD64,0xE71E,0x48a0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const GUID kCLSID_MMDeviceEnum  = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID kIID_IMMDeviceEnum   = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID kIID_ISessionMgr2    = {0x77AA99A0,0x1BD6,0x484F,{0x8B,0xC7,0x2C,0x65,0x4C,0x9A,0x9B,0x6F}};
static const GUID kIID_ISessionCtl2    = {0xbfb7ff88,0x7239,0x4fc9,{0x8f,0xa2,0x07,0xc9,0x50,0xbe,0x9c,0x6d}};

// Estruturas de AUDIOCLIENT_ACTIVATION_PARAMS (audioclientactivationparams.h).
enum TL_ActivationType { TL_ACTIVATION_DEFAULT = 0, TL_ACTIVATION_PROCESS_LOOPBACK = 1 };
enum TL_LoopbackMode { TL_INCLUDE_TREE = 0, TL_EXCLUDE_TREE = 1 };
struct TL_LoopbackParams { DWORD TargetProcessId; TL_LoopbackMode Mode; };
struct TL_ActivationParams { TL_ActivationType ActivationType; TL_LoopbackParams Loopback; };

typedef HRESULT (WINAPI *PFN_ActivateAsync)(LPCWSTR, REFIID, PROPVARIANT*,
  IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);

/* ---------- utilidades ---------- */
static CRITICAL_SECTION gLogLock;
static void logf(const char* fmt, ...){
  EnterCriticalSection(&gLogLock);
  SYSTEMTIME t; GetLocalTime(&t);
  printf("[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
  printf("\n"); fflush(stdout);
  LeaveCriticalSection(&gLogLock);
}
static std::string utf8(const std::wstring& w){
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
static std::wstring wide(const std::string& s){
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
static std::string hrText(HRESULT hr){ char b[32]; snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr); return b; }

/* ---------- processos ---------- */
struct Proc { DWORD pid, parent; std::wstring exe; };
static std::vector<Proc> listProcesses(){
  std::vector<Proc> v;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return v;
  PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
  if (Process32FirstW(snap, &pe)) do { v.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile}); } while (Process32NextW(snap, &pe));
  CloseHandle(snap);
  return v;
}
// Processo "raiz" de um executável: o que não tem pai com o mesmo nome.
// Capturar a árvore dele pega todos os processos filhos (útil para apps multiprocesso).
static DWORD rootPidOf(const std::vector<Proc>& ps, const std::vector<std::wstring>& names){
  auto match = [&](const std::wstring& e){ for (auto& n : names) if (_wcsicmp(e.c_str(), n.c_str()) == 0) return true; return false; };
  std::vector<DWORD> pids;
  for (auto& p : ps) if (match(p.exe)) pids.push_back(p.pid);
  for (auto& p : ps){
    if (!match(p.exe)) continue;
    bool parentSame = false;
    for (DWORD q : pids) if (q == p.parent) parentSame = true;
    if (!parentSame) return p.pid;
  }
  return pids.empty() ? 0 : pids[0];
}
static const std::vector<std::wstring> DISCORD_NAMES = {L"Discord.exe", L"DiscordPTB.exe", L"DiscordCanary.exe"};

struct WinTitleCtx { std::vector<std::pair<DWORD, std::wstring>> out; };
static BOOL CALLBACK enumWin(HWND h, LPARAM lp){
  if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
  wchar_t t[256]; int n = GetWindowTextW(h, t, 256);
  if (n <= 0) return TRUE;
  DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
  ((WinTitleCtx*)lp)->out.push_back({pid, std::wstring(t, n)});
  return TRUE;
}

/* Modo desempenho (ver boostThread): PID do Telão Desktop e quantos processos já foram liberados. */
static DWORD gBoostRoot = 0;
static volatile LONG gBoostCount = -1;   // -1 = modo desempenho não pedido
static long gBoostCountValue(){ return InterlockedCompareExchange(&gBoostCount, 0, 0x7fffffff); }

/* Lista os apps que têm sessão de áudio em algum dispositivo de saída ativo. */
static std::string buildAppList(){
  struct App { std::wstring exe, title; bool active; };
  std::vector<App> apps;
  std::vector<Proc> ps = listProcesses();
  auto exeOf = [&](DWORD pid) -> std::wstring { for (auto& p : ps) if (p.pid == pid) return p.exe; return L""; };

  IMMDeviceEnumerator* en = nullptr;
  if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnum, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnum, (void**)&en))){
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))){
      UINT nd = 0; coll->GetCount(&nd);
      for (UINT d = 0; d < nd; d++){
        IMMDevice* dev = nullptr; if (FAILED(coll->Item(d, &dev))) continue;
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(kIID_ISessionMgr2, CLSCTX_ALL, nullptr, (void**)&mgr))){
          IAudioSessionEnumerator* se = nullptr;
          if (SUCCEEDED(mgr->GetSessionEnumerator(&se))){
            int ns = 0; se->GetCount(&ns);
            for (int i = 0; i < ns; i++){
              IAudioSessionControl* ctl = nullptr; if (FAILED(se->GetSession(i, &ctl))) continue;
              IAudioSessionControl2* c2 = nullptr;
              if (SUCCEEDED(ctl->QueryInterface(kIID_ISessionCtl2, (void**)&c2))){
                DWORD pid = 0; c2->GetProcessId(&pid);
                AudioSessionState st = AudioSessionStateInactive; c2->GetState(&st);
                if (pid && c2->IsSystemSoundsSession() != S_OK && st != AudioSessionStateExpired){
                  std::wstring exe = exeOf(pid);
                  if (!exe.empty() && _wcsicmp(exe.c_str(), L"telao-audio.exe") != 0){
                    bool found = false;
                    for (auto& a : apps) if (_wcsicmp(a.exe.c_str(), exe.c_str()) == 0){ found = true; a.active = a.active || st == AudioSessionStateActive; }
                    if (!found) apps.push_back({exe, L"", st == AudioSessionStateActive});
                  }
                }
                c2->Release();
              }
              ctl->Release();
            }
            se->Release();
          }
          mgr->Release();
        }
        dev->Release();
      }
      coll->Release();
    }
    en->Release();
  }

  WinTitleCtx wt; EnumWindows(enumWin, (LPARAM)&wt);
  for (auto& a : apps){
    for (auto& w : wt.out){
      if (_wcsicmp(exeOf(w.first).c_str(), a.exe.c_str()) == 0){ a.title = w.second; break; }
    }
  }

  bool discord = rootPidOf(ps, DISCORD_NAMES) != 0;
  std::string j = "{\"type\":\"list\",\"boost\":" + std::to_string(gBoostCountValue()) +
                  ",\"discord\":" + std::string(discord ? "true" : "false") + ",\"apps\":[";
  for (size_t i = 0; i < apps.size(); i++){
    if (i) j += ",";
    j += "{\"name\":\"" + jsonEsc(utf8(apps[i].exe)) + "\",\"title\":\"" + jsonEsc(utf8(apps[i].title)) +
         "\",\"active\":" + (apps[i].active ? "true" : "false") + "}";
  }
  return j + "]}";
}

/* ---------- conexões WebSocket ---------- */
struct Conn {
  SOCKET s;
  CRITICAL_SECTION sendLock;
  volatile LONG alive;
  std::string origin;
};

static bool sendAll(SOCKET s, const char* d, int n){
  while (n > 0){ int k = send(s, d, n, 0); if (k <= 0) return false; d += k; n -= k; }
  return true;
}
static bool recvExact(SOCKET s, char* d, int n){
  while (n > 0){ int k = recv(s, d, n, 0); if (k <= 0) return false; d += k; n -= k; }
  return true;
}
static bool wsSend(Conn* c, int opcode, const void* data, size_t len){
  if (!c->alive) return false;
  std::string h = wsFrameHeader(opcode, len);
  EnterCriticalSection(&c->sendLock);
  bool ok = sendAll(c->s, h.data(), (int)h.size()) && (len == 0 || sendAll(c->s, (const char*)data, (int)len));
  LeaveCriticalSection(&c->sendLock);
  if (!ok) InterlockedExchange(&c->alive, 0);
  return ok;
}
static void wsText(Conn* c, const std::string& s){ wsSend(c, 1, s.data(), s.size()); }
static void wsError(Conn* c, const std::string& msg){ wsText(c, "{\"type\":\"error\",\"message\":\"" + jsonEsc(msg) + "\"}"); }

/* ---------- origens permitidas ---------- */
static CRITICAL_SECTION gOriginLock;
static std::vector<std::string> gOrigins;
static std::wstring originsFile(){
  wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s(p); size_t k = s.find_last_of(L"\\/");
  return (k == std::wstring::npos ? L"" : s.substr(0, k + 1)) + L"telao-audio-origens.txt";
}
static void loadOrigins(){
  FILE* f = _wfopen(originsFile().c_str(), L"rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof line, f)){
    std::string s(line); while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (!s.empty()) gOrigins.push_back(s);
  }
  fclose(f);
}
static bool originAllowed(const std::string& o){
  if (o.empty()) return false;                                 // navegadores sempre mandam Origin
  if (o == "null" || o.rfind("file://", 0) == 0) return true;  // Telão aberto como arquivo local
  EnterCriticalSection(&gOriginLock);
  bool ok = false;
  for (auto& x : gOrigins) if (x == o) ok = true;
  if (!ok){
    EnterCriticalSection(&gLogLock);
    printf("\n  Um site quer usar o Telão Audio: %s\n  Só permita se for o seu Telão hospedado. Permitir? [s/n] ", o.c_str());
    fflush(stdout);
    char ans[16] = {0};
    if (fgets(ans, sizeof ans, stdin) && (ans[0] == 's' || ans[0] == 'S' || ans[0] == 'y' || ans[0] == 'Y')) ok = true;
    LeaveCriticalSection(&gLogLock);
    if (ok){
      gOrigins.push_back(o);
      FILE* f = _wfopen(originsFile().c_str(), L"ab");
      if (f){ fprintf(f, "%s\n", o.c_str()); fclose(f); }
      logf("Site permitido: %s", o.c_str());
    } else logf("Site recusado: %s", o.c_str());
  }
  LeaveCriticalSection(&gOriginLock);
  return ok;
}

/* ---------- captura ---------- */
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler {
  LONG ref = 1;
public:
  HANDLE done;
  HRESULT result = E_FAIL;
  IAudioClient* client = nullptr;
  ActivateHandler(){ done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
  virtual ~ActivateHandler(){ CloseHandle(done); }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_ActivateHandler) || IsEqualGUID(riid, kIID_IAgileObject)){
      *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
  STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
  STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
    HRESULT hr = E_FAIL; IUnknown* unk = nullptr;
    HRESULT q = op->GetActivateResult(&hr, &unk);
    if (SUCCEEDED(q) && SUCCEEDED(hr) && unk){
      result = unk->QueryInterface(kIID_IAudioClient, (void**)&client);
    } else result = FAILED(q) ? q : hr;
    if (unk) unk->Release();
    SetEvent(done);
    return S_OK;
  }
};

struct Capture {
  Conn* conn;
  IAudioClient* client;
  IAudioCaptureClient* cap;
  HANDLE event, proc, thread;
  volatile LONG stop;
  std::string label;
};
static CRITICAL_SECTION gCapLock;
static Capture* gCap = nullptr;

static DWORD WINAPI captureThread(void* arg){
  Capture* c = (Capture*)arg;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::vector<char> out; out.reserve(RATE * 4 / 10);
  bool ended = false;
  while (!c->stop && c->conn->alive){
    WaitForSingleObject(c->event, 200);
    UINT32 frames = 0;
    while (SUCCEEDED(c->cap->GetNextPacketSize(&frames)) && frames > 0){
      BYTE* data = nullptr; DWORD flags = 0;
      if (FAILED(c->cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
      size_t bytes = (size_t)frames * CHANNELS * 2, at = out.size();
      out.resize(at + bytes);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(&out[at], 0, bytes);
      else memcpy(&out[at], data, bytes);
      c->cap->ReleaseBuffer(frames);
    }
    if (!out.empty()){ wsSend(c->conn, 2, out.data(), out.size()); out.clear(); }
    if (c->proc && WaitForSingleObject(c->proc, 0) == WAIT_OBJECT_0){ ended = true; break; }
  }
  c->client->Stop();
  if (ended){
    logf("O app capturado fechou: %s", c->label.c_str());
    wsText(c->conn, "{\"type\":\"ended\"}");
  }
  CoUninitialize();
  return 0;
}

static void stopCapture(Conn* onlyFor){
  EnterCriticalSection(&gCapLock);
  Capture* c = gCap;
  if (c && (!onlyFor || c->conn == onlyFor)){
    gCap = nullptr;
    InterlockedExchange(&c->stop, 1);
    SetEvent(c->event);
    if (WaitForSingleObject(c->thread, 3000) == WAIT_OBJECT_0){
      CloseHandle(c->thread);
      c->cap->Release(); c->client->Release();
      CloseHandle(c->event);
      if (c->proc) CloseHandle(c->proc);
      logf("Captura parada: %s", c->label.c_str());
      delete c;
    }  // se a thread travou, não libera nada por baixo dela (vaza, mas não derruba o programa)
  }
  LeaveCriticalSection(&gCapLock);
}

static void startCapture(Conn* conn, DWORD pid, bool include, const std::string& label){
  stopCapture(nullptr);  // só uma captura por vez; a mais recente vence

  static PFN_ActivateAsync activate = nullptr;
  if (!activate){
    HMODULE mm = LoadLibraryW(L"Mmdevapi.dll");
    if (mm) activate = (PFN_ActivateAsync)GetProcAddress(mm, "ActivateAudioInterfaceAsync");
  }
  if (!activate){ wsError(conn, "Este Windows não tem captura de áudio por app (precisa Windows 10 2004 ou mais novo)."); return; }

  TL_ActivationParams params{};
  params.ActivationType = TL_ACTIVATION_PROCESS_LOOPBACK;
  params.Loopback.TargetProcessId = pid;
  params.Loopback.Mode = include ? TL_INCLUDE_TREE : TL_EXCLUDE_TREE;
  PROPVARIANT pv; memset(&pv, 0, sizeof pv);
  pv.vt = VT_BLOB; pv.blob.cbSize = sizeof params; pv.blob.pBlobData = (BYTE*)&params;

  ActivateHandler* h = new ActivateHandler();
  IActivateAudioInterfaceAsyncOperation* op = nullptr;
  HRESULT hr = activate(L"VAD\\Process_Loopback", kIID_IAudioClient, &pv, h, &op);
  if (SUCCEEDED(hr) && WaitForSingleObject(h->done, 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  else if (SUCCEEDED(hr)) hr = h->result;
  IAudioClient* client = SUCCEEDED(hr) ? h->client : nullptr;
  if (op) op->Release();
  h->Release();
  if (FAILED(hr) || !client){ wsError(conn, "O Windows recusou a captura (" + hrText(hr) + ")."); return; }

  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = CHANNELS; wf.nSamplesPerSec = RATE;
  wf.wBitsPerSample = 16; wf.nBlockAlign = CHANNELS * 2; wf.nAvgBytesPerSec = RATE * wf.nBlockAlign;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
    200000, 0, &wf, nullptr);
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  IAudioCaptureClient* cap = nullptr;
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev);
  if (SUCCEEDED(hr)) hr = client->GetService(kIID_IAudioCapture, (void**)&cap);
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)){
    if (cap) cap->Release();
    client->Release(); CloseHandle(ev);
    wsError(conn, "Não consegui iniciar a captura (" + hrText(hr) + ").");
    return;
  }

  Capture* c = new Capture{};
  c->conn = conn; c->client = client; c->cap = cap; c->event = ev; c->label = label;
  c->proc = include ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
  wsText(conn, "{\"type\":\"started\",\"sampleRate\":" + std::to_string(RATE) + ",\"channels\":" + std::to_string(CHANNELS) +
               ",\"label\":\"" + jsonEsc(label) + "\"}");
  EnterCriticalSection(&gCapLock);
  gCap = c;
  c->thread = CreateThread(nullptr, 0, captureThread, c, 0, nullptr);
  LeaveCriticalSection(&gCapLock);
  logf("Capturando: %s", label.c_str());
}

/* ---------- captura turbo de vídeo ----------
   O Windows entrega cada quadro da janela/monitor na placa de vídeo (Windows Graphics Capture).
   Aqui ele é reduzido para o tamanho da transmissão e convertido para NV12 na própria placa.
   Dois jeitos de entregar para o Telão:
     - "gpu": a imagem fica numa textura compartilhada; o Telão recebe só "o quadro N está pronto"
       (zero cópia: o codificador da placa lê direto dali);
     - "mem": a imagem pronta vem para a memória e vai pelo WebSocket (plano B, funciona em qualquer placa).
   Só o app do Telão (página local) pode usar: sites aprovados para o áudio não recebem vídeo. */

struct SizeInt32 { INT32 Width, Height; };
static const GUID IID_IGraphicsCaptureItemInterop = {0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
static const GUID IID_IGraphicsCaptureItem        = {0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
static const GUID IID_IFramePoolStatics2          = {0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
static const GUID IID_ISession2                   = {0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
static const GUID IID_ISession3                   = {0xf2cdd966, 0x22ae, 0x5ea1, {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
static const GUID IID_ISession5                   = {0x67c0ea62, 0x1f85, 0x5061, {0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb}};
static const GUID IID_IDxgiInterfaceAccess        = {0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
static const GUID IID_IClosable                   = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};

struct IGraphicsCaptureItemInterop : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND, REFIID, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR, REFIID, void**) = 0;
};
struct IGraphicsCaptureItem : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_Closed(void*, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_Closed(INT64) = 0;
};
struct IDirect3D11CaptureFrame : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32*) = 0;
};
struct IGraphicsCaptureSession : IInspectable { virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0; };
struct IGraphicsCaptureSession2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean) = 0;
};
struct IGraphicsCaptureSession3 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean) = 0;
};
struct IGraphicsCaptureSession5 : IInspectable {     // Windows 11 24H2+: intervalo mínimo entre quadros (unidades de 100 ns)
  virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(INT64) = 0;
};
struct IDirect3D11CaptureFramePool : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable*, INT32, INT32, SizeInt32) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame**) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(void*, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(INT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem*, IGraphicsCaptureSession**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(void**) = 0;
};
struct IDirect3D11CaptureFramePoolStatics2 : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable*, INT32, INT32, SizeInt32, IDirect3D11CaptureFramePool**) = 0;
};
struct IDirect3DDxgiInterfaceAccess : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID, void**) = 0; };
struct IClosable : IInspectable { virtual HRESULT STDMETHODCALLTYPE Close() = 0; };
static const INT32 PIXEL_BGRA8 = 87;   // DirectXPixelFormat.B8G8R8A8UIntNormalized

typedef HRESULT (WINAPI *PFN_RoInitialize)(int);
typedef HRESULT (WINAPI *PFN_RoGetActivationFactory)(HSTRING, REFIID, void**);
typedef HRESULT (WINAPI *PFN_WindowsCreateString)(LPCWSTR, UINT32, HSTRING*);
typedef HRESULT (WINAPI *PFN_WindowsDeleteString)(HSTRING);
typedef HRESULT (WINAPI *PFN_CreateD3DDeviceFromDXGI)(IDXGIDevice*, IInspectable**);
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
static PFN_RoInitialize pRoInitialize;
static PFN_RoGetActivationFactory pRoGetActivationFactory;
static PFN_WindowsCreateString pWindowsCreateString;
static PFN_WindowsDeleteString pWindowsDeleteString;
static PFN_D3D11CreateDevice pD3D11CreateDevice;
static PFN_CreateDXGIFactory1 pCreateDXGIFactory1;
static PFN_CreateD3DDeviceFromDXGI pCreateD3DDeviceFromDXGI;

// Tudo carregado na hora: se faltar algo (Windows antigo), só a captura turbo fica indisponível.
static bool videoApis(){
  static int st = 0;
  if (st) return st > 0;
  HMODULE cb = LoadLibraryW(L"combase.dll"), d3 = LoadLibraryW(L"d3d11.dll"), dx = LoadLibraryW(L"dxgi.dll");
  if (cb){
    pRoInitialize = (PFN_RoInitialize)GetProcAddress(cb, "RoInitialize");
    pRoGetActivationFactory = (PFN_RoGetActivationFactory)GetProcAddress(cb, "RoGetActivationFactory");
    pWindowsCreateString = (PFN_WindowsCreateString)GetProcAddress(cb, "WindowsCreateString");
    pWindowsDeleteString = (PFN_WindowsDeleteString)GetProcAddress(cb, "WindowsDeleteString");
  }
  if (d3){
    pD3D11CreateDevice = (PFN_D3D11CreateDevice)GetProcAddress(d3, "D3D11CreateDevice");
    pCreateD3DDeviceFromDXGI = (PFN_CreateD3DDeviceFromDXGI)GetProcAddress(d3, "CreateDirect3D11DeviceFromDXGIDevice");
  }
  if (dx) pCreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(dx, "CreateDXGIFactory1");
  st = pRoInitialize && pRoGetActivationFactory && pWindowsCreateString && pWindowsDeleteString && pD3D11CreateDevice && pCreateD3DDeviceFromDXGI && pCreateDXGIFactory1 ? 1 : -1;
  return st > 0;
}
static IInspectable* activationFactory(const wchar_t* cls, REFIID iid){
  HSTRING s = nullptr; void* f = nullptr;
  if (FAILED(pWindowsCreateString(cls, (UINT32)wcslen(cls), &s))) return nullptr;
  HRESULT hr = pRoGetActivationFactory(s, iid, &f);
  pWindowsDeleteString(s);
  return SUCCEEDED(hr) ? (IInspectable*)f : nullptr;
}
static double nowMs(){ static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
// Espera curta e precisa sem depender da resolução do relógio do Windows (que ele limita em processos escondidos).
static void hrSleep(HANDLE timer, double ms){
  if (timer){
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(ms * 10000);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)){ WaitForSingleObject(timer, 100); return; }
  }
  Sleep(ms < 1 ? 0 : (DWORD)ms);
}
template<class T> static void rel(T*& p){ if (p){ p->Release(); p = nullptr; } }
static void closeObj(IUnknown* o){ if (!o) return; IClosable* c = nullptr; if (SUCCEEDED(o->QueryInterface(IID_IClosable, (void**)&c))){ c->Close(); c->Release(); } }

struct VCap {
  Conn* conn = nullptr;
  HANDLE thread = nullptr;
  volatile LONG stop = 0;
  HWND hwnd = nullptr; HMONITOR mon = nullptr; bool monitor = false;
  int boxW = 1920, boxH = 1080, fps = 60;
  bool gpu = true;
  DWORD pid = 0;
  UINT vendor = 0, device = 0;
};
static CRITICAL_SECTION gVidLock;
static VCap* gVid = nullptr;

struct VRes {
  ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
  ID3D11VideoDevice* vdev = nullptr; ID3D11VideoContext* vctx = nullptr;
  IInspectable* rtDev = nullptr;
  IGraphicsCaptureItem* item = nullptr; IDirect3D11CaptureFramePool* pool = nullptr; IGraphicsCaptureSession* ses = nullptr;
  ID3D11Texture2D* inCopy = nullptr; int inW = 0, inH = 0;
  ID3D11VideoProcessorEnumerator* vpe = nullptr; ID3D11VideoProcessor* vp = nullptr; ID3D11VideoProcessorInputView* vin = nullptr;
  std::vector<ID3D11Texture2D*> out; std::vector<ID3D11VideoProcessorOutputView*> outView; std::vector<HANDLE> shared;
  ID3D11Texture2D* staging = nullptr; ID3D11Query* query = nullptr;
  HANDLE timer = nullptr, target = nullptr;
  int ow = 0, oh = 0; bool nv12 = true;
  std::string adapter;
};
static void freeVP(VRes& r){
  for (auto*& v : r.outView) rel(v);
  r.outView.clear();
  rel(r.vin); rel(r.vp); rel(r.vpe);
}
static void freeRes(VRes& r){
  freeVP(r);
  rel(r.inCopy); rel(r.staging); rel(r.query);
  for (auto*& t : r.out) rel(t);
  r.out.clear();
  for (HANDLE h : r.shared) CloseHandle(h);
  r.shared.clear();
  if (r.ses){ closeObj(r.ses); rel(r.ses); }
  if (r.pool){ closeObj(r.pool); rel(r.pool); }
  rel(r.item); rel(r.rtDev); rel(r.vctx); rel(r.vdev);
  if (r.ctx){ r.ctx->ClearState(); r.ctx->Flush(); }
  rel(r.ctx); rel(r.dev);
  if (r.timer){ CloseHandle(r.timer); r.timer = nullptr; }
  if (r.target){ CloseHandle(r.target); r.target = nullptr; }
}
static ID3D11Texture2D* makeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, D3D11_USAGE u, UINT bind, UINT cpu, UINT misc){
  D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
  d.Usage = u; d.BindFlags = bind; d.CPUAccessFlags = cpu; d.MiscFlags = misc;
  ID3D11Texture2D* t = nullptr;
  return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t)) ? t : nullptr;
}
// Processador de vídeo da placa: entrada BGRA do tamanho da janela, saída do tamanho da transmissão.
static HRESULT setupVP(VRes& r, int inW, int inH, int fps){
  freeVP(r);
  if (r.inW != inW || r.inH != inH || !r.inCopy){
    rel(r.inCopy);
    r.inCopy = makeTex(r.dev, inW, inH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0);
    if (!r.inCopy) return E_OUTOFMEMORY;
    r.inW = inW; r.inH = inH;
  }
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
  cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  cd.InputWidth = inW; cd.InputHeight = inH; cd.OutputWidth = r.ow; cd.OutputHeight = r.oh;
  cd.InputFrameRate = {(UINT)fps, 1}; cd.OutputFrameRate = {(UINT)fps, 1};
  cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
  HRESULT hr = r.vdev->CreateVideoProcessorEnumerator(&cd, &r.vpe);
  if (SUCCEEDED(hr)) hr = r.vdev->CreateVideoProcessor(r.vpe, 0, &r.vp);
  if (FAILED(hr)) return hr;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  hr = r.vdev->CreateVideoProcessorInputView(r.inCopy, r.vpe, &iv, &r.vin);
  if (FAILED(hr)) return hr;
  for (auto* t : r.out){
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorOutputView* v = nullptr;
    hr = r.vdev->CreateVideoProcessorOutputView(t, r.vpe, &ovd, &v);
    if (FAILED(hr)) return hr;
    r.outView.push_back(v);
  }
  // cores: entrada RGB completa → saída BT.709 em faixa de TV (o padrão de vídeo/WebRTC)
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {}; in.RGB_Range = 0;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {}; out.RGB_Range = 0; out.YCbCr_Matrix = 1; out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  r.vctx->VideoProcessorSetStreamColorSpace(r.vp, 0, &in);
  r.vctx->VideoProcessorSetOutputColorSpace(r.vp, &out);
  r.vctx->VideoProcessorSetStreamFrameFormat(r.vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  r.vctx->VideoProcessorSetStreamAutoProcessingMode(r.vp, 0, FALSE);
  D3D11_VIDEO_COLOR black = {}; black.RGBA.A = 1.0f;
  r.vctx->VideoProcessorSetOutputBackgroundColor(r.vp, FALSE, &black);
  RECT all = {0, 0, r.ow, r.oh};
  r.vctx->VideoProcessorSetOutputTargetRect(r.vp, TRUE, &all);
  return S_OK;
}
static ID3D11Texture2D* frameTexture(IDirect3D11CaptureFrame* fr){
  IInspectable* surf = nullptr; if (FAILED(fr->get_Surface(&surf)) || !surf) return nullptr;
  IDirect3DDxgiInterfaceAccess* acc = nullptr; ID3D11Texture2D* tex = nullptr;
  if (SUCCEEDED(surf->QueryInterface(IID_IDxgiInterfaceAccess, (void**)&acc))){ acc->GetInterface(__uuidof(ID3D11Texture2D), (void**)&tex); acc->Release(); }
  surf->Release();
  return tex;
}
static void closeFrame(IDirect3D11CaptureFrame* fr){ closeObj(fr); fr->Release(); }

// Mesma placa que o Telão usa (fabricante/modelo informados pela página); sem isso, a placa principal.
static HRESULT makeDevice(VRes& r, UINT vendor, UINT device){
  IDXGIFactory1* fac = nullptr; IDXGIAdapter1* pick = nullptr;
  if (SUCCEEDED(pCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))){
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++){
      DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
      bool sw = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      if (!sw && !pick && (!vendor || (d.VendorId == vendor && (!device || d.DeviceId == device)))){ pick = a; continue; }
      a->Release();
    }
    fac->Release();
  }
  D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  HRESULT hr = pD3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, 2, D3D11_SDK_VERSION, &r.dev, nullptr, &r.ctx);
  if (FAILED(hr)) hr = pD3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl + 1, 1, D3D11_SDK_VERSION, &r.dev, nullptr, &r.ctx);
  if (pick) pick->Release();
  if (FAILED(hr)) return hr;
  IDXGIDevice* dx = nullptr; r.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx);
  if (!dx) return E_NOINTERFACE;
  IDXGIAdapter* ad = nullptr;
  if (SUCCEEDED(dx->GetAdapter(&ad))){ DXGI_ADAPTER_DESC d; ad->GetDesc(&d); r.adapter = utf8(d.Description); ad->Release(); }
  hr = pCreateD3DDeviceFromDXGI(dx, &r.rtDev);
  dx->Release();
  if (FAILED(hr)) return hr;
  r.dev->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&r.vdev);
  r.ctx->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&r.vctx);
  if (!r.vdev || !r.vctx) return E_NOINTERFACE;
  // Várias threads podem usar o dispositivo (a captura do Windows usa por baixo): protege o contexto.
  ID3D10Multithread* mt = nullptr;
  if (SUCCEEDED(r.ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))){ mt->SetMultithreadProtected(TRUE); mt->Release(); }
  return S_OK;
}

static void vSendErr(VCap* v, const std::string& type, const std::string& msg){
  wsText(v->conn, "{\"type\":\"" + type + "\",\"message\":\"" + jsonEsc(msg) + "\"}");
}
#define VFAIL(msg, hr) do{ vSendErr(v, "verror", std::string(msg) + " (" + hrText(hr) + ")"); logf("Captura turbo: %s (%s)", msg, hrText(hr).c_str()); freeRes(r); return; }while(0)

static void videoRun(VCap* v){
  VRes r;
  if (!videoApis()){ vSendErr(v, "verror", "Este Windows não tem a captura moderna (precisa do Windows 10 1903 ou mais novo)."); return; }
  pRoInitialize(1);
  HRESULT hr = makeDevice(r, v->vendor, v->device);
  if (FAILED(hr)) VFAIL("não consegui usar a placa de vídeo", hr);
  auto interop = (IGraphicsCaptureItemInterop*)activationFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem", IID_IGraphicsCaptureItemInterop);
  if (!interop) VFAIL("a captura do Windows não está disponível", E_NOINTERFACE);
  hr = v->monitor ? interop->CreateForMonitor(v->mon, IID_IGraphicsCaptureItem, (void**)&r.item)
                  : interop->CreateForWindow(v->hwnd, IID_IGraphicsCaptureItem, (void**)&r.item);
  interop->Release();
  if (FAILED(hr) || !r.item) VFAIL(v->monitor ? "o Windows não deixou capturar esse monitor" : "o Windows não deixou capturar essa janela", hr);
  SizeInt32 sz{}; r.item->get_Size(&sz);
  if (sz.Width < 16 || sz.Height < 16) VFAIL("a janela está minimizada ou sem tamanho", E_FAIL);
  fitSize(sz.Width, sz.Height, v->boxW, v->boxH, &r.ow, &r.oh);

  // saídas
  std::vector<std::string> remote;   // handles das imagens compartilhadas, já valendo no processo do Telão
  if (v->gpu){
    r.target = OpenProcess(PROCESS_DUP_HANDLE, FALSE, v->pid);
    if (!r.target) VFAIL("não consegui falar com o Telão para entregar os quadros", HRESULT_FROM_WIN32(GetLastError()));
    const int K = 5;   // quadros em rodízio: o codificador lê um enquanto os próximos são escritos
    UINT misc = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    for (int pass = 0; pass < 2 && (int)r.out.size() < K; pass++){
      r.nv12 = pass == 0;   // NV12 (o que o codificador quer); se a placa não deixar compartilhar, BGRA
      for (auto*& t : r.out) rel(t);
      r.out.clear();
      for (int i = 0; i < K; i++){
        ID3D11Texture2D* t = makeTex(r.dev, r.ow, r.oh, r.nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, misc);
        if (!t) break;
        r.out.push_back(t);
      }
    }
    if ((int)r.out.size() < K) VFAIL("a placa não deixou criar a imagem compartilhada", E_FAIL);
    for (auto* t : r.out){
      IDXGIResource1* r1 = nullptr; HANDLE h = nullptr, dup = nullptr;
      hr = t->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
      if (SUCCEEDED(hr)){ hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h); r1->Release(); }
      if (FAILED(hr) || !h) VFAIL("a placa não deixou compartilhar a imagem", hr);
      r.shared.push_back(h);
      if (!DuplicateHandle(GetCurrentProcess(), h, r.target, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        VFAIL("não consegui entregar a imagem ao Telão", HRESULT_FROM_WIN32(GetLastError()));
      remote.push_back(std::to_string((unsigned long long)(uintptr_t)dup));   // vale dentro do processo do Telão
    }
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    if (FAILED(r.dev->CreateQuery(&qd, &r.query))) VFAIL("a placa não deixou sincronizar", E_FAIL);
  } else {
    ID3D11Texture2D* t = makeTex(r.dev, r.ow, r.oh, DXGI_FORMAT_NV12, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0);
    if (!t) VFAIL("a placa não deixou criar a imagem de saída", E_FAIL);
    r.out.push_back(t);
    r.staging = makeTex(r.dev, r.ow, r.oh, DXGI_FORMAT_NV12, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0);
    if (!r.staging) VFAIL("a placa não deixou criar a imagem de leitura", E_FAIL);
  }
  hr = setupVP(r, sz.Width, sz.Height, v->fps);
  if (FAILED(hr)) VFAIL("o processador de vídeo da placa não aceitou esse tamanho", hr);

  auto st2 = (IDirect3D11CaptureFramePoolStatics2*)activationFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", IID_IFramePoolStatics2);
  if (!st2) VFAIL("a captura do Windows não está disponível (precisa do Windows 10 1903+)", E_NOINTERFACE);
  SizeInt32 poolSz = sz;
  hr = st2->CreateFreeThreaded(r.rtDev, PIXEL_BGRA8, 3, poolSz, &r.pool);
  st2->Release();
  if (FAILED(hr)) VFAIL("não consegui preparar a captura", hr);
  hr = r.pool->CreateCaptureSession(r.item, &r.ses);
  if (FAILED(hr)) VFAIL("não consegui abrir a sessão de captura", hr);
  bool border = true, minUpd = false;
  { IGraphicsCaptureSession2* s2 = nullptr; if (SUCCEEDED(r.ses->QueryInterface(IID_ISession2, (void**)&s2))){ s2->put_IsCursorCaptureEnabled(true); s2->Release(); } }
  { IGraphicsCaptureSession3* s3 = nullptr; if (SUCCEEDED(r.ses->QueryInterface(IID_ISession3, (void**)&s3))){ border = FAILED(s3->put_IsBorderRequired(false)); s3->Release(); } }
  { IGraphicsCaptureSession5* s5 = nullptr;   // sem isso o Windows entrega no máximo ~60 por segundo
    if (SUCCEEDED(r.ses->QueryInterface(IID_ISession5, (void**)&s5))){ minUpd = SUCCEEDED(s5->put_MinUpdateInterval(std::max<INT64>(10000000 / 240, 10000000 / (v->fps * 2)))); s5->Release(); } }
  hr = r.ses->StartCapture();
  if (FAILED(hr)) VFAIL("o Windows recusou iniciar a captura", hr);

  r.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!r.timer) r.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  { std::string j = "{\"type\":\"vstarted\",\"mode\":\"" + std::string(v->gpu ? "gpu" : "mem") + "\",\"w\":" + std::to_string(r.ow) + ",\"h\":" + std::to_string(r.oh) +
                    ",\"sw\":" + std::to_string(sz.Width) + ",\"sh\":" + std::to_string(sz.Height) + ",\"fmt\":\"" + (r.nv12 ? "nv12" : "bgra") + "\",\"slots\":[";
    for (size_t i = 0; i < remote.size(); i++) j += (i ? ",\"" : "\"") + remote[i] + "\"";
    j += "],\"adapter\":\"" + jsonEsc(r.adapter) + "\",\"border\":" + (border ? "true" : "false") + ",\"minUpdate\":" + (minUpd ? "true" : "false") + "}";
    wsText(v->conn, j); }
  logf("Captura turbo (%s): %dx%d → %dx%d a até %d fps, placa %s", v->gpu ? "placa" : "memória", sz.Width, sz.Height, r.ow, r.oh, v->fps, r.adapter.c_str());

  Pacer pacer; pacer.setFps(v->fps);
  std::vector<uint8_t> pkt;
  int slot = -1, lastSlot = -1; uint32_t seq = 0;
  double lastSend = 0, lastCheck = nowMs(), statT0 = nowMs();
  int nWin = 0, nOut = 0, nRep = 0; std::vector<double> ms;
  auto procCpu = []{ FILETIME c, e, k, u; if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    return ((double)(((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (double)(((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10000.0; };   // ms de processador
  double cpu0 = procCpu();
  std::string endReason;
  while (!v->stop && v->conn->alive){
    double now = nowMs();
    if (now - lastCheck > 500){
      lastCheck = now;
      if (!v->monitor && !IsWindow(v->hwnd)){ endReason = "a janela fechou"; break; }
      HRESULT dr = r.dev->GetDeviceRemovedReason();
      if (dr != S_OK){ endReason = "a placa de vídeo reiniciou (" + hrText(dr) + ")"; break; }
    }
    if (now - statT0 >= 2000){
      double secs = (now - statT0) / 1000, avg = 0, p95 = 0;
      if (!ms.empty()){ for (double x : ms) avg += x; avg /= ms.size(); std::sort(ms.begin(), ms.end()); p95 = ms[std::min(ms.size() - 1, (size_t)(ms.size() * 0.95))]; }
      double cpu1 = procCpu(), cpu = (cpu1 - cpu0) / (now - statT0) * 100; cpu0 = cpu1;   // % de um núcleo (o Telão Audio inteiro)
      char b[240]; snprintf(b, sizeof b, "{\"type\":\"vstat\",\"win\":%.1f,\"out\":%.1f,\"rep\":%.1f,\"ms\":%.2f,\"p95\":%.2f,\"cpu\":%.1f}", nWin / secs, nOut / secs, nRep / secs, avg, p95, cpu);
      wsText(v->conn, b);
      statT0 = now; nWin = nOut = nRep = 0; ms.clear();
    }
    IDirect3D11CaptureFrame* fr = nullptr;
    if (FAILED(r.pool->TryGetNextFrame(&fr)) || !fr){
      // tela parada (o Windows só manda quadro quando algo muda): repete o último para quem entrar agora ver a imagem
      if (lastSlot >= 0 && now - lastSend > 250){
        if (v->gpu){ char b[80]; snprintf(b, sizeof b, "{\"type\":\"vf\",\"s\":%d,\"n\":%u}", lastSlot, seq); wsText(v->conn, b); }
        else wsSend(v->conn, 2, pkt.data(), pkt.size());
        lastSend = now; nRep++;
      }
      hrSleep(r.timer, 1);
      continue;
    }
    nWin++;
    // se juntou mais de um na fila (processamento atrasou), fica só com o mais novo: menos atraso pra quem assiste
    for (IDirect3D11CaptureFrame* nx = nullptr; SUCCEEDED(r.pool->TryGetNextFrame(&nx)) && nx; nx = nullptr){ closeFrame(fr); fr = nx; nWin++; }
    SizeInt32 cs{}; fr->get_ContentSize(&cs);
    if (cs.Width >= 16 && cs.Height >= 16 && (cs.Width != poolSz.Width || cs.Height != poolSz.Height)){
      // a janela mudou de tamanho: o Windows passa a entregar no tamanho novo (a saída continua igual, com faixas se precisar)
      poolSz = cs;
      r.pool->Recreate(r.rtDev, PIXEL_BGRA8, 3, poolSz);
      closeFrame(fr);
      continue;
    }
    if (!pacer.accept(now)){ closeFrame(fr); continue; }
    ID3D11Texture2D* tex = frameTexture(fr);
    if (!tex){ closeFrame(fr); continue; }
    D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
    if ((int)td.Width != r.inW || (int)td.Height != r.inH){
      hr = setupVP(r, td.Width, td.Height, v->fps);
      if (FAILED(hr)){ tex->Release(); closeFrame(fr); endReason = "o processador de vídeo não aceitou o tamanho novo da janela (" + hrText(hr) + ")"; break; }
    }
    double t0 = nowMs();
    r.ctx->CopyResource(r.inCopy, tex);
    tex->Release();
    int cw = std::min<int>(cs.Width > 0 ? cs.Width : td.Width, td.Width), ch = std::min<int>(cs.Height > 0 ? cs.Height : td.Height, td.Height);
    RECT src = {0, 0, cw, ch};
    IRect d = aspectRect(cw, ch, r.ow, r.oh);
    RECT dst = {d.l, d.t, d.r, d.b};
    r.vctx->VideoProcessorSetStreamSourceRect(r.vp, 0, TRUE, &src);
    r.vctx->VideoProcessorSetStreamDestRect(r.vp, 0, TRUE, &dst);
    D3D11_VIDEO_PROCESSOR_STREAM s = {}; s.Enable = TRUE; s.pInputSurface = r.vin;
    bool sent = false;
    if (v->gpu){
      slot = (slot + 1) % (int)r.outView.size();
      hr = r.vctx->VideoProcessorBlt(r.vp, r.outView[slot], 0, 1, &s);
      r.ctx->End(r.query);
      r.ctx->Flush();
      // espera a placa terminar antes de avisar o Telão (os dois lados leem a mesma memória)
      BOOL done = FALSE; double w0 = nowMs();
      while (r.ctx->GetData(r.query, &done, sizeof done, 0) == S_FALSE && nowMs() - w0 < 100) hrSleep(r.timer, 0.25);
      closeFrame(fr);
      if (SUCCEEDED(hr)){
        seq++;
        char b[80]; snprintf(b, sizeof b, "{\"type\":\"vf\",\"s\":%d,\"n\":%u}", slot, seq);
        wsText(v->conn, b);
        sent = true;
        lastSlot = slot;
      }
    } else {
      hr = r.vctx->VideoProcessorBlt(r.vp, r.outView[0], 0, 1, &s);
      r.ctx->CopyResource(r.staging, r.out[0]);
      D3D11_MAPPED_SUBRESOURCE m;
      HRESULT mh = SUCCEEDED(hr) ? r.ctx->Map(r.staging, 0, D3D11_MAP_READ, 0, &m) : hr;   // o Map espera a placa terminar
      closeFrame(fr);   // só agora: antes disso o Windows poderia reaproveitar a imagem que ainda estava sendo copiada
      if (SUCCEEDED(mh)){
        size_t plane = (size_t)r.ow * r.oh;
        pkt.resize(16 + plane * 3 / 2);
        frameHeader(pkt.data(), r.ow, r.oh, ++seq);
        const uint8_t* p = (const uint8_t*)m.pData; uint8_t* o = pkt.data() + 16;
        for (int y = 0; y < r.oh; y++) memcpy(o + (size_t)y * r.ow, p + (size_t)y * m.RowPitch, r.ow);
        const uint8_t* uv = p + (size_t)m.RowPitch * r.oh;
        for (int y = 0; y < r.oh / 2; y++) memcpy(o + plane + (size_t)y * r.ow, uv + (size_t)y * m.RowPitch, r.ow);
        r.ctx->Unmap(r.staging, 0);
        ms.push_back(nowMs() - t0);   // tempo de placa + cópia (sem contar o envio)
        wsSend(v->conn, 2, pkt.data(), pkt.size());
        sent = true; lastSlot = 0;
      }
    }
    if (v->gpu && sent) ms.push_back(nowMs() - t0);
    if (sent){ nOut++; lastSend = nowMs(); }
  }
  if (!endReason.empty() && v->conn->alive){
    wsText(v->conn, "{\"type\":\"vended\",\"reason\":\"" + jsonEsc(endReason) + "\"}");
    logf("Captura turbo encerrada: %s", endReason.c_str());
  }
  freeRes(r);
}
static DWORD WINAPI videoThread(void* arg){
  VCap* v = (VCap*)arg;
  // Este programa roda escondido: no Windows 11 isso o coloca em modo economia (EcoQoS) e o jogo
  // ganharia a disputa pelo processador. A captura pede prioridade de "captura multimídia".
  { PROCESS_POWER_THROTTLING_STATE st{}; st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION; st.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st)){
      st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED; SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st); } }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  typedef HANDLE (WINAPI *PFN_AvSet)(LPCWSTR, LPDWORD);
  typedef BOOL (WINAPI *PFN_AvRevert)(HANDLE);
  HMODULE avrt = LoadLibraryW(L"avrt.dll");
  PFN_AvSet avSet = avrt ? (PFN_AvSet)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : nullptr;
  PFN_AvRevert avRevert = avrt ? (PFN_AvRevert)GetProcAddress(avrt, "AvRevertMmThreadCharacteristics") : nullptr;
  DWORD idx = 0; HANDLE mm = avSet ? avSet(L"Capture", &idx) : nullptr;
  videoRun(v);
  if (mm && avRevert) avRevert(mm);
  return 0;
}
static void stopVideo(Conn* onlyFor){
  EnterCriticalSection(&gVidLock);
  VCap* v = gVid;
  if (v && (!onlyFor || v->conn == onlyFor)){
    gVid = nullptr;
    InterlockedExchange(&v->stop, 1);
    if (WaitForSingleObject(v->thread, 4000) == WAIT_OBJECT_0){ CloseHandle(v->thread); delete v; }
    // se travou dentro do driver, deixa vazar em vez de derrubar o programa
  }
  LeaveCriticalSection(&gVidLock);
}
static void startVideo(Conn* c, const std::string& msg){
  if (!(c->origin == "null" || c->origin.rfind("file://", 0) == 0)){ wsText(c, "{\"type\":\"verror\",\"message\":\"a captura turbo só funciona no app do Telão\"}"); return; }
  stopVideo(nullptr);   // uma captura de vídeo por vez
  VCap* v = new VCap();
  v->conn = c;
  v->gpu = jsonStr(msg, "mode") != "mem";
  v->pid = (DWORD)strtoul(jsonStr(msg, "pid").c_str(), nullptr, 10);
  v->boxW = std::max(160, std::min(3840, atoi(jsonStr(msg, "w").c_str())));
  v->boxH = std::max(90, std::min(2160, atoi(jsonStr(msg, "h").c_str())));
  v->fps = std::max(5, std::min(240, atoi(jsonStr(msg, "fps").c_str())));
  v->vendor = (UINT)strtoul(jsonStr(msg, "vendor").c_str(), nullptr, 10);
  v->device = (UINT)strtoul(jsonStr(msg, "device").c_str(), nullptr, 10);
  if (jsonStr(msg, "target") == "monitor"){
    v->monitor = true;
    POINT pt = {atol(jsonStr(msg, "x").c_str()), atol(jsonStr(msg, "y").c_str())};
    v->mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  } else {
    unsigned long long hv = strtoull(jsonStr(msg, "hwnd").c_str(), nullptr, 10);
    v->hwnd = (HWND)(uintptr_t)hv;
    if (!hv || !IsWindow(v->hwnd)){ delete v; wsText(c, "{\"type\":\"verror\",\"message\":\"a janela escolhida não existe mais\"}"); return; }
  }
  if (v->gpu && !v->pid){ delete v; wsText(c, "{\"type\":\"verror\",\"message\":\"faltou o processo do Telão\"}"); return; }
  EnterCriticalSection(&gVidLock);
  gVid = v;
  v->thread = CreateThread(nullptr, 0, videoThread, v, 0, nullptr);
  if (!v->thread){ gVid = nullptr; delete v; wsText(c, "{\"type\":\"verror\",\"message\":\"não consegui iniciar a captura\"}"); }
  LeaveCriticalSection(&gVidLock);
}

static void handleCommand(Conn* c, const std::string& msg){
  std::string cmd = jsonStr(msg, "cmd");
  if (cmd == "list"){ wsText(c, buildAppList()); return; }
  if (cmd == "stop"){ stopCapture(c); return; }
  if (cmd == "vstart"){ startVideo(c, msg); return; }
  if (cmd == "vstop"){ stopVideo(c); return; }
  if (cmd == "winrect"){
    // Retângulo da janela (pixels físicos) e do monitor onde ela está: usado pela camada de reações do app.
    unsigned long long hv = strtoull(jsonStr(msg, "hwnd").c_str(), nullptr, 10);
    HWND hwnd = (HWND)(uintptr_t)hv;
    RECT r{};
    if (!hv || !IsWindow(hwnd) || !GetWindowRect(hwnd, &r)){ wsText(c, "{\"type\":\"winrect\",\"ok\":false}"); return; }
    MONITORINFO mi{}; mi.cbSize = sizeof mi;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    char b[256];
    snprintf(b, sizeof b, "{\"type\":\"winrect\",\"ok\":true,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,\"mx\":%ld,\"my\":%ld,\"mw\":%ld,\"mh\":%ld}",
             r.left, r.top, r.right - r.left, r.bottom - r.top,
             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    wsText(c, b);
    return;
  }
  if (cmd == "start"){
    std::vector<Proc> ps = listProcesses();
    std::string target = jsonStr(msg, "target");
    if (target == "hwnd"){
      // Janela escolhida no Telão Desktop ("window:<HWND>:0") → processo dono dela.
      unsigned long long hv = strtoull(jsonStr(msg, "hwnd").c_str(), nullptr, 10);
      HWND hwnd = (HWND)(uintptr_t)hv;
      DWORD pid = 0;
      if (hv && IsWindow(hwnd)) GetWindowThreadProcessId(hwnd, &pid);
      if (!pid){ wsError(c, "A janela escolhida não existe mais."); return; }
      std::wstring exe;
      for (auto& p : ps) if (p.pid == pid) exe = p.exe;
      if (_wcsicmp(exe.c_str(), L"ApplicationFrameHost.exe") == 0){
        wsError(c, "Esse app (da Microsoft Store/Xbox) não permite pegar o áudio pela janela. Escolha o jogo em \"Só o app\".");
        return;
      }
      startCapture(c, pid, true, exe.empty() ? "janela escolhida" : utf8(exe));
    } else if (target == "except-discord"){
      DWORD pid = rootPidOf(ps, DISCORD_NAMES);
      if (!pid){ wsError(c, "O Discord não está aberto."); return; }
      startCapture(c, pid, false, "tudo menos o Discord");
    } else {
      std::string name = jsonStr(msg, "name");
      DWORD pid = rootPidOf(ps, {wide(name)});
      if (!pid){ wsError(c, "Não encontrei " + name + " aberto."); return; }
      startCapture(c, pid, true, name);
    }
  }
}

static DWORD WINAPI connThread(void* arg){
  Conn* c = (Conn*)arg;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string req; char buf[2048];
  bool ok = false;
  while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384){
    int n = recv(c->s, buf, sizeof buf, 0);
    if (n <= 0) break;
    req.append(buf, n);
  }
  std::string key = httpHeader(req, "Sec-WebSocket-Key");
  c->origin = httpHeader(req, "Origin");
  if (key.empty()){
    const char* r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"; sendAll(c->s, r, (int)strlen(r));
  } else if (!originAllowed(c->origin)){
    const char* r = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"; sendAll(c->s, r, (int)strlen(r));
  } else {
    std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                    wsAcceptKey(key) + "\r\n\r\n";
    ok = sendAll(c->s, r.data(), (int)r.size());
  }
  if (ok){
    logf("Telão conectado (origem: %s)", c->origin.c_str());
    while (c->alive){
      unsigned char h[2];
      if (!recvExact(c->s, (char*)h, 2)) break;
      int op = h[0] & 0x0F; bool masked = h[1] & 0x80; uint64_t len = h[1] & 0x7F;
      if (len == 126){ unsigned char e[2]; if (!recvExact(c->s, (char*)e, 2)) break; len = (e[0] << 8) | e[1]; }
      else if (len == 127){ unsigned char e[8]; if (!recvExact(c->s, (char*)e, 8)) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
      if (len > (1u << 20)) break;
      unsigned char mask[4] = {0, 0, 0, 0};
      if (masked && !recvExact(c->s, (char*)mask, 4)) break;
      std::string p((size_t)len, '\0');
      if (len && !recvExact(c->s, &p[0], (int)len)) break;
      for (size_t i = 0; i < p.size(); i++) p[i] ^= mask[i & 3];
      if (op == 0x8){ wsSend(c, 0x8, nullptr, 0); break; }
      if (op == 0x9){ wsSend(c, 0xA, p.data(), p.size()); continue; }
      if (op == 0x1) handleCommand(c, p);
    }
    logf("Telão desconectado");
  }
  InterlockedExchange(&c->alive, 0);
  stopCapture(c);
  stopVideo(c);
  closesocket(c->s);
  DeleteCriticalSection(&c->sendLock);
  delete c;
  CoUninitialize();
  return 0;
}

/* Windows 11: processo com janela minimizada/invisível perde a precisão de timer que pediu
   (timers voltam a ~16 ms → 60 fps vira 30) e pode cair em EcoQoS. Aqui o Telão Desktop
   passa o próprio PID e este helper tira todos os processos dele dessa economia. */

static bool unthrottle(DWORD pid){
  HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
  if (!h) return false;
  PROCESS_POWER_THROTTLING_STATE st{};
  st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  st.StateMask = 0;   // controla os dois mecanismos e deixa os dois DESLIGADOS
  BOOL ok = SetProcessInformation(h, ProcessPowerThrottling, &st, sizeof st);
  if (!ok){   // Windows 10 não conhece o bit de timer: aplica só o de velocidade
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    ok = SetProcessInformation(h, ProcessPowerThrottling, &st, sizeof st);
  }
  CloseHandle(h);
  return ok != FALSE;
}
static DWORD WINAPI boostThread(void*){
  HANDLE root = OpenProcess(SYNCHRONIZE, FALSE, gBoostRoot);
  size_t lastCount = 0;
  for (;;){
    if (!root || WaitForSingleObject(root, 0) == WAIT_OBJECT_0) ExitProcess(0);   // o Telão fechou: o helper fecha junto
    std::vector<ProcInfo> ps;
    for (auto& p : listProcesses()) ps.push_back({p.pid, p.parent, utf8(p.exe)});
    size_t n = 0;
    for (uint32_t pid : processTree(gBoostRoot, ps)) if (unthrottle(pid)) n++;
    InterlockedExchange(&gBoostCount, (LONG)n);
    if (n != lastCount){ logf("Modo desempenho aplicado a %u processos do Telão", (unsigned)n); lastCount = n; }
    Sleep(3000);
  }
}

int main(int argc, char** argv){
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], "--boost-tree") == 0) gBoostRoot = (DWORD)strtoul(argv[i + 1], nullptr, 10);
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleTitleW(L"Telão Audio");
  {
    typedef BOOL (WINAPI *PFN_SetDpiCtx)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_SetDpiCtx setDpi = u32 ? (PFN_SetDpiCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : nullptr;
    if (setDpi) setDpi((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  }
  // Sem "modo de edição rápida": clicar no console não congela o programa.
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE); DWORD mode = 0;
  if (GetConsoleMode(in, &mode)) SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);

  InitializeCriticalSection(&gLogLock);
  InitializeCriticalSection(&gOriginLock);
  InitializeCriticalSection(&gCapLock);
  InitializeCriticalSection(&gVidLock);
  loadOrigins();
  if (gBoostRoot){ HANDLE bt = CreateThread(nullptr, 0, boostThread, nullptr, 0, nullptr); if (bt) CloseHandle(bt); }

  printf("\n  Telão Audio\n  Captura o áudio de um app específico para o Telão.\n  Deixe esta janela aberta enquanto transmite; feche para encerrar.\n\n");

  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  BOOL excl = TRUE; setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0){
    if (gBoostRoot){
      // Outro Telão Audio já cuida do áudio; este fica só mantendo o desempenho até o Telão fechar.
      logf("Áudio já atendido por outro Telão Audio; mantendo só o modo desempenho.");
      for (;;) Sleep(60000);
    }
    printf("  Não consegui abrir a porta %u. Provavelmente o Telão Audio já está aberto.\n  Pressione Enter para sair.", PORT);
    getchar();
    return 1;
  }
  logf("Pronto. Esperando o Telão em 127.0.0.1:%u", PORT);

  for (;;){
    SOCKET s = accept(ls, nullptr, nullptr);
    if (s == INVALID_SOCKET) continue;
    BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
    Conn* c = new Conn();
    c->s = s; c->alive = 1;
    InitializeCriticalSection(&c->sendLock);
    HANDLE t = CreateThread(nullptr, 0, connThread, c, 0, nullptr);
    if (t) CloseHandle(t);
    else { closesocket(s); DeleteCriticalSection(&c->sendLock); delete c; }
  }
}
#endif
