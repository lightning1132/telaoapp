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

static void handleCommand(Conn* c, const std::string& msg){
  std::string cmd = jsonStr(msg, "cmd");
  if (cmd == "list"){ wsText(c, buildAppList()); return; }
  if (cmd == "stop"){ stopCapture(c); return; }
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
