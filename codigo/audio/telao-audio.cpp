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
//   zig c++ -target x86_64-windows-gnu -O2 -o telao-audio.exe telao-audio.cpp -lole32 -lws2_32 -luser32 -lgdi32
//   (motor.inc, no mesmo diretório, entra junto: codificar/decodificar vídeo na placa)

#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

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

/* ---------- uso da placa de vídeo: partes portáveis ----------
   O Windows mede o uso de cada "motor" da placa (3D, cópia, codificação, decodificação…) por processo,
   nos mesmos contadores que o Gerenciador de Tarefas usa ("GPU Engine"). Cada instância se chama
   pid_<PID>_luid_<placa>_phys_<n>_eng_<n>_engtype_<tipo>. Como o Gerenciador:
   uso de um conjunto de processos = o motor mais ocupado (somando esses processos nele). */
struct GpuUse { double mine = 0, enc = 0, dec = 0, sys = 0; int inst = 0; };
static bool parseGpuInst(const std::string& s, uint32_t* pid, std::string* engine, std::string* type){
  if (s.compare(0, 4, "pid_") != 0) return false;
  size_t b = s.find('_', 4);
  size_t l = s.find("_luid_"), t = s.find("_engtype_");
  if (b == std::string::npos || l == std::string::npos || t == std::string::npos || t < l || b > l) return false;
  *pid = (uint32_t)strtoul(s.substr(4, b - 4).c_str(), nullptr, 10);
  *engine = s.substr(l + 1, t - l - 1);
  *type = lower(s.substr(t + 9));
  return true;
}
static GpuUse gpuAggregate(const std::vector<std::pair<std::string, double>>& v, const std::vector<uint32_t>& mine){
  std::vector<std::string> eng; std::vector<std::string> typ; std::vector<double> all, my;
  GpuUse u;
  for (auto& it : v){
    uint32_t pid; std::string e, t;
    if (!parseGpuInst(it.first, &pid, &e, &t) || !(it.second >= 0)) continue;
    u.inst++;
    size_t k = 0;
    while (k < eng.size() && eng[k] != e) k++;
    if (k == eng.size()){ eng.push_back(e); typ.push_back(t); all.push_back(0); my.push_back(0); }
    all[k] += it.second;
    if (std::find(mine.begin(), mine.end(), pid) != mine.end()) my[k] += it.second;
  }
  for (size_t k = 0; k < eng.size(); k++){
    u.sys = std::max(u.sys, all[k]);
    u.mine = std::max(u.mine, my[k]);
    if (typ[k].find("encode") != std::string::npos) u.enc = std::max(u.enc, my[k]);
    if (typ[k].find("decode") != std::string::npos) u.dec = std::max(u.dec, my[k]);
  }
  u.sys = std::min(100.0, u.sys); u.mine = std::min(100.0, u.mine); u.enc = std::min(100.0, u.enc); u.dec = std::min(100.0, u.dec);
  return u;
}

/* Memória da placa (VRAM), contadores "GPU Process Memory" (pid_<PID>_luid_<placa>_phys_<n>) e
   "GPU Adapter Memory" (luid_<placa>_phys_<n>), em bytes. A placa "do jogo" é a que tem mais memória em uso. */
struct VramUse { double mine = 0, used = 0; std::string luid; };
static std::string luidOf(const std::string& s){
  size_t l = s.find("luid_"), p = s.find("_phys_");
  return l == std::string::npos || p == std::string::npos || p < l ? std::string() : lower(s.substr(l + 5, p - l - 5));
}
static VramUse vramAggregate(const std::vector<std::pair<std::string, double>>& procs, const std::vector<std::pair<std::string, double>>& adapters,
                             const std::vector<uint32_t>& mine){
  VramUse u;
  for (auto& a : adapters){ std::string l = luidOf(a.first); if (!l.empty() && a.second > u.used){ u.used = a.second; u.luid = l; } }
  for (auto& p : procs){
    if (p.first.compare(0, 4, "pid_") != 0 || !(p.second > 0)) continue;
    uint32_t pid = (uint32_t)strtoul(p.first.c_str() + 4, nullptr, 10);
    if (std::find(mine.begin(), mine.end(), pid) != mine.end()) u.mine += p.second;
  }
  return u;
}

/* ---------- camada por cima do jogo: partes portáveis ----------
   Reações sobem e somem (3 s) e mensagens do chat empilham embaixo (somem em ~9 s), como na camada antiga
   (overlay.html), mas desenhadas aqui: a janela transparente do Electron disputava a placa com a captura turbo. */
static std::vector<uint8_t> b64decode(const std::string& s){
  std::vector<uint8_t> out; out.reserve(s.size() * 3 / 4);
  int val = 0, bits = -8;
  for (unsigned char c : s){
    int d;
    if (c >= 'A' && c <= 'Z') d = c - 'A'; else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
    else if (c >= '0' && c <= '9') d = c - '0' + 52; else if (c == '+') d = 62; else if (c == '/') d = 63;
    else if (c == '=') break; else continue;
    val = (val << 6) | d; bits += 6;
    if (bits >= 0){ out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
  }
  return out;
}
// "data:image/png;base64,...." → bytes (vazio se não for imagem)
static std::vector<uint8_t> dataUrlBytes(const std::string& u){
  if (u.compare(0, 11, "data:image/") != 0) return {};
  size_t k = u.find(";base64,");
  if (k == std::string::npos || k > 40) return {};
  return b64decode(u.substr(k + 8));
}
// cubic-bezier(0, 0, .58, 1) = "ease-out" do CSS
static double easeOut(double x){
  if (x <= 0) return 0; if (x >= 1) return 1;
  double lo = 0, hi = 1, t = x;
  for (int i = 0; i < 30; i++){
    t = (lo + hi) / 2;
    double bx = 3 * (1 - t) * t * t * 0.58 + t * t * t;   // P1x = 0, P2x = .58
    if (bx < x) lo = t; else hi = t;
  }
  return 3 * (1 - t) * (1 - t) * t * 0 + 3 * (1 - t) * t * t * 1 + t * t * t;   // P1y = 0, P2y = 1
}
// Reação: deslocamento vertical (em alturas do próprio elemento, negativo = sobe), escala e opacidade aos t ms.
struct RxPose { double dy, scale, alpha; };
static RxPose rxPose(double ms){
  double p = ms / 3000.0;
  RxPose r{0, 0.6, 0};
  if (p <= 0) return r;
  if (p >= 1) return {-3.2, 1, 0};
  if (p < 0.1){ double e = easeOut(p / 0.1); return {-0.08 * e, 0.6 + 0.55 * e, e}; }
  double e = easeOut((p - 0.1) / 0.9);
  r.dy = -0.08 + (-3.2 + 0.08) * e; r.scale = 1.15 - 0.15 * e;
  r.alpha = p < 0.7 ? 1 : 1 - easeOut((p - 0.7) / 0.3);
  return r;
}
// Mensagem do chat: entra subindo 10 px (0,25 s), começa a sumir aos 8 s (0,8 s) e sai aos 8,9 s.
struct ChatPose { double dy, alpha; bool gone; };
static ChatPose chatPose(double ms){
  if (ms >= 8900) return {0, 0, true};
  if (ms < 250){ double e = easeOut(ms / 250); return {10 * (1 - e), e, false}; }
  if (ms >= 8000) return {0, std::max(0.0, 1 - (ms - 8000) / 800), false};
  return {0, 1, false};
}
// Faixa no lado direito de baixo da área do jogo (pixels físicos): 25% da largura (180 a 360 px na escala 100%).
struct ORect { int x, y, w, h; };
static ORect overlayRect(int ax, int ay, int aw, int ah, double scale){
  int w = (int)std::lround(std::min(360 * scale, std::max(180 * scale, aw * 0.25)));
  w = std::min(w, std::max(1, aw));
  int h = std::min(ah, (int)std::lround(640 * scale));
  return {ax + aw - w, ay + ah - h, w, std::max(1, h)};
}

/* ---------- ícone do Telao.exe: partes portáveis ----------
   Arquivo .ico → imagens; e o diretório de grupo (RT_GROUP_ICON) que o Windows guarda dentro do exe,
   que aponta para cada imagem pelo número do recurso (RT_ICON). */
struct IcoImg { uint8_t w = 0, h = 0, colors = 0; uint16_t planes = 0, bits = 0; std::vector<uint8_t> data; };
static uint16_t rd16(const std::vector<uint8_t>& b, size_t o){ return (uint16_t)(b[o] | (b[o + 1] << 8)); }
static uint32_t rd32(const std::vector<uint8_t>& b, size_t o){ return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); }
static bool parseIco(const std::vector<uint8_t>& f, std::vector<IcoImg>* out){
  out->clear();
  if (f.size() < 6 || rd16(f, 0) != 0 || rd16(f, 2) != 1) return false;
  size_t n = rd16(f, 4);
  if (!n || n > 64 || f.size() < 6 + 16 * n) return false;
  for (size_t i = 0; i < n; i++){
    size_t e = 6 + 16 * i;
    IcoImg im; im.w = f[e]; im.h = f[e + 1]; im.colors = f[e + 2]; im.planes = rd16(f, e + 4); im.bits = rd16(f, e + 6);
    uint32_t size = rd32(f, e + 8), off = rd32(f, e + 12);
    if (!size || off < 6 + 16 * n || (uint64_t)off + size > f.size()) return false;
    im.data.assign(f.begin() + off, f.begin() + off + size);
    out->push_back(std::move(im));
  }
  return true;
}
static std::vector<uint8_t> grpIconDir(const std::vector<IcoImg>& imgs, uint16_t firstId){
  std::vector<uint8_t> g;
  auto p16 = [&](uint16_t v){ g.push_back(v & 255); g.push_back(v >> 8); };
  auto p32 = [&](uint32_t v){ for (int i = 0; i < 4; i++) g.push_back((v >> (8 * i)) & 255); };
  p16(0); p16(1); p16((uint16_t)imgs.size());
  for (size_t i = 0; i < imgs.size(); i++){
    const IcoImg& im = imgs[i];
    g.push_back(im.w); g.push_back(im.h); g.push_back(im.colors); g.push_back(0);
    p16(im.planes); p16(im.bits); p32((uint32_t)im.data.size()); p16((uint16_t)(firstId + i));
  }
  return g;
}

/* ---------- motor de vídeo: partes portáveis ----------
   Quadros comprimidos (Annex B: 00 00 01 antes de cada NAL) de H.264 e H.265 e o cabeçalho dos quadros que vão e vêm
   entre o Telão e o Telão Audio pelo WebSocket. */
struct Nal { size_t off, len; int type; };
static std::vector<Nal> annexbNals(const uint8_t* p, size_t n, bool hevc){
  std::vector<Nal> v;
  size_t start = (size_t)-1;
  auto push = [&](size_t a, size_t b){ if (b > a) v.push_back({a, b - a, hevc ? (p[a] >> 1) & 0x3f : p[a] & 0x1f}); };
  for (size_t i = 0; i + 2 < n; ){
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1){
      size_t sc = (i > 0 && p[i - 1] == 0) ? i - 1 : i;
      if (start != (size_t)-1) push(start, sc);
      start = i + 3; i += 3;
    } else i++;
  }
  if (start != (size_t)-1 && start < n) push(start, n);
  return v;
}
struct AuInfo { bool key = false, params = false; int nals = 0; };
// key: tem quadro completo (IDR no H.264; IDR/CRA/BLA no H.265). params: traz SPS/PPS (e VPS no H.265).
static AuInfo auInfo(const uint8_t* p, size_t n, bool hevc){
  AuInfo a; bool sps = false, pps = false, vps = !hevc;
  for (const Nal& x : annexbNals(p, n, hevc)){
    a.nals++;
    if (hevc){ if (x.type >= 16 && x.type <= 21) a.key = true; if (x.type == 32) vps = true; if (x.type == 33) sps = true; if (x.type == 34) pps = true; }
    else { if (x.type == 5) a.key = true; if (x.type == 7) sps = true; if (x.type == 8) pps = true; }
  }
  a.params = sps && pps && vps;
  return a;
}
// Quadro completo sem SPS/PPS (alguns codificadores só mandam no começo): coloca o cabeçalho na frente.
static std::vector<uint8_t> withParams(const uint8_t* p, size_t n, const std::vector<uint8_t>& seq, bool hevc){
  std::vector<uint8_t> o;
  AuInfo a = auInfo(p, n, hevc);
  if (a.key && !a.params && !seq.empty()){ o.assign(seq.begin(), seq.end()); if (!(seq.size() >= 3 && seq[0] == 0 && seq[1] == 0)){ o.clear(); } }
  o.insert(o.end(), p, p + n);
  return o;
}
// Cabeçalho de 24 bytes: "TEF1" (Telão Audio → Telão, quadro codificado) ou "TDF1" (Telão → Telão Audio, para decodificar).
static void engHeader(uint8_t* p, const char* magic, uint32_t id, uint32_t flags, uint32_t seq, uint64_t ts){
  memcpy(p, magic, 4);
  for (int i = 0; i < 4; i++){ p[4 + i] = (id >> (8 * i)) & 255; p[8 + i] = (flags >> (8 * i)) & 255; p[12 + i] = (seq >> (8 * i)) & 255; }
  for (int i = 0; i < 8; i++) p[16 + i] = (ts >> (8 * i)) & 255;
}
static bool engParse(const uint8_t* p, size_t n, const char* magic, uint32_t* id, uint32_t* flags, uint32_t* seq, uint64_t* ts){
  if (n < 24 || memcmp(p, magic, 4) != 0) return false;
  uint32_t a = 0, b = 0, c = 0; uint64_t t = 0;
  for (int i = 3; i >= 0; i--){ a = (a << 8) | p[4 + i]; b = (b << 8) | p[8 + i]; c = (c << 8) | p[12 + i]; }
  for (int i = 7; i >= 0; i--) t = (t << 8) | p[16 + i];
  *id = a; *flags = b; *seq = c; *ts = t;
  return true;
}
// Vídeo dentro da janela: centralizado, sem distorcer (faixas pretas nas sobras).
struct FRect { float l, t, r, b; };
static FRect letterbox(int vw, int vh, int W, int H){
  if (vw <= 0 || vh <= 0 || W <= 0 || H <= 0) return {0, 0, (float)W, (float)H};
  double k = std::min((double)W / vw, (double)H / vh);
  float w = (float)(vw * k), h = (float)(vh * k);
  return {(W - w) / 2, (H - h) / 2, (W + w) / 2, (H + h) / 2};
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
  {
    uint32_t pid; std::string e, t;
    check(parseGpuInst("pid_1234_luid_0x00000000_0x0000D1B2_phys_0_eng_3_engtype_VideoEncode", &pid, &e, &t) && pid == 1234 &&
          e == "luid_0x00000000_0x0000D1B2_phys_0_eng_3" && t == "videoencode", "placa: lê o nome do contador");
    check(!parseGpuInst("_Total", &pid, &e, &t) && !parseGpuInst("pid_x", &pid, &e, &t), "placa: ignora nomes estranhos");
    const char* L = "_luid_0x00000000_0x0000D1B2_phys_0_eng_";
    auto I = [&](int p, int eng, const char* ty){ return "pid_" + std::to_string(p) + L + std::to_string(eng) + "_engtype_" + ty; };
    std::vector<std::pair<std::string, double>> v = {
      {I(10, 0, "3D"), 60}, {I(20, 0, "3D"), 5}, {I(21, 0, "3D"), 2},      // jogo 60%, Telão 7% no 3D
      {I(20, 3, "VideoEncode"), 9}, {I(21, 3, "VideoEncode"), 1},          // NVENC: Telão 10%
      {I(22, 4, "VideoDecode"), 4}, {I(10, 1, "Copy"), 3},
      {"pid_30_luid_0x00000000_0x0000AAAA_phys_0_eng_0_engtype_3D", 30},     // outra placa
    };
    GpuUse u = gpuAggregate(v, {20, 21, 22});
    check(u.mine > 9.9 && u.mine < 10.1, "placa: Telão = motor mais ocupado (codificação 10%)");
    check(u.enc > 9.9 && u.enc < 10.1 && u.dec > 3.9 && u.dec < 4.1, "placa: codificação e decodificação separadas");
    check(u.sys > 66.9 && u.sys < 67.1, "placa: PC inteiro = 3D com jogo + Telão (67%)");
    v.push_back({I(20, 3, "VideoEncode"), 150});
    check(gpuAggregate(v, {20}).mine == 100, "placa: nunca passa de 100%");
    check(gpuAggregate({}, {20}).inst == 0, "placa: sem contadores = zero");
  }
  {
    check(b64decode("aGVsbG8=") == std::vector<uint8_t>({'h','e','l','l','o'}) && b64decode("YQ").size() == 1, "camada: base64");
    check(dataUrlBytes("data:image/png;base64,iVBORw0KGgo=").size() == 8 && dataUrlBytes("javascript:alert(1)").empty(), "camada: só aceita imagem em data URL");
    check(std::fabs(easeOut(0.5) - 0.5) > 0.05 && easeOut(0) == 0 && easeOut(1) == 1 && easeOut(0.25) < easeOut(0.75), "camada: curva ease-out");
    RxPose a = rxPose(0), b = rxPose(300), c = rxPose(1500), d = rxPose(2990), e = rxPose(3000);
    check(a.alpha == 0 && std::fabs(a.scale - 0.6) < 1e-9 && std::fabs(b.alpha - 1) < 1e-6 && std::fabs(b.scale - 1.15) < 1e-6, "camada: reação aparece crescendo (0,6 → 1,15)");
    check(c.dy < b.dy && c.alpha == 1 && d.alpha < 0.05 && e.dy == -3.2 && e.alpha == 0, "camada: reação sobe e some em 3 s");
    ChatPose m0 = chatPose(0), m1 = chatPose(1000), m2 = chatPose(8400), m3 = chatPose(9000);
    check(m0.alpha == 0 && m0.dy == 10 && m1.alpha == 1 && m1.dy == 0 && m2.alpha > 0.4 && m2.alpha < 0.6 && m3.gone, "camada: mensagem entra, fica 8 s e some");
    ORect r = overlayRect(0, 0, 2560, 1440, 1.0);
    check(r.w == 360 && r.h == 640 && r.x == 2200 && r.y == 800, "camada: faixa de 360x640 no canto de baixo à direita (1440p)");
    r = overlayRect(100, 50, 640, 480, 1.5);
    check(r.w == 270 && r.h == 480 && r.x == 470 && r.y == 50, "camada: janela pequena e escala 150%");
  }
  {
    std::vector<std::pair<std::string, double>> pr = {
      {"pid_20_luid_0x00000000_0x0000D1B2_phys_0", 300e6}, {"pid_21_luid_0x00000000_0x0000D1B2_phys_0", 100e6},
      {"pid_10_luid_0x00000000_0x0000D1B2_phys_0", 6e9}, {"pid_20_luid_0x00000000_0x0000AAAA_phys_0", 50e6}, {"_Total", 1}};
    std::vector<std::pair<std::string, double>> ad = {{"luid_0x00000000_0x0000AAAA_phys_0", 1e9}, {"luid_0x00000000_0x0000D1B2_phys_0", 7.2e9}};
    VramUse v = vramAggregate(pr, ad, {20, 21});
    check(v.mine > 449e6 && v.mine < 451e6, "vram: Telão soma os processos dele (450 MB)");
    check(v.used > 7.1e9 && v.luid == "0x00000000_0x0000d1b2", "vram: placa do jogo = a mais usada");
    check(luidOf("garbage").empty(), "vram: nome estranho não quebra");
  }
  {
    // .ico com 2 imagens (16 e 256 px; 256 vira 0 no arquivo)
    std::vector<uint8_t> a = {1, 2, 3}, b = {9, 8, 7, 6, 5};
    std::vector<uint8_t> f = {0, 0, 1, 0, 2, 0};
    auto e = [&](uint8_t w, uint32_t size, uint32_t off){ uint8_t x[16] = {w, w, 0, 0, 1, 0, 32, 0}; for (int i = 0; i < 4; i++){ x[8 + i] = (size >> (8 * i)) & 255; x[12 + i] = (off >> (8 * i)) & 255; } f.insert(f.end(), x, x + 16); };
    e(16, 3, 38); e(0, 5, 41); f.insert(f.end(), a.begin(), a.end()); f.insert(f.end(), b.begin(), b.end());
    std::vector<IcoImg> im;
    check(parseIco(f, &im) && im.size() == 2 && im[0].w == 16 && im[0].data == a && im[1].w == 0 && im[1].bits == 32 && im[1].data == b, "ícone: lê o .ico");
    std::vector<uint8_t> g = grpIconDir(im, 7);
    check(g.size() == 6 + 14 * 2 && g[4] == 2 && g[6] == 16 && g[6 + 8] == 3 && g[6 + 12] == 7 && g[20 + 12] == 8 && g[20 + 8] == 5, "ícone: diretório do grupo aponta para os recursos 7 e 8");
    std::vector<uint8_t> bad = f; bad[2] = 2;
    check(!parseIco(bad, &im), "ícone: recusa arquivo que não é .ico");
    std::vector<uint8_t> cut(f.begin(), f.end() - 2);
    check(!parseIco(cut, &im), "ícone: recusa .ico cortado");
  }
  {
    // motor: Annex B
    const uint8_t h264[] = {0,0,0,1, 0x67, 1, 2, 0,0,0,1, 0x68, 3, 0,0,1, 0x65, 9, 9, 0, 0,0,1, 0x41, 7};
    auto ns = annexbNals(h264, sizeof h264, false);
    check(ns.size() == 4 && ns[0].type == 7 && ns[1].type == 8 && ns[2].type == 5 && ns[2].len == 3 && ns[3].type == 1, "motor: NALs do H.264 (início de 3 e 4 bytes)");
    AuInfo a = auInfo(h264, sizeof h264, false);
    check(a.key && a.params && a.nals == 4, "motor: quadro completo com SPS/PPS");
    const uint8_t idrOnly[] = {0,0,0,1, 0x65, 1, 2};
    a = auInfo(idrOnly, sizeof idrOnly, false);
    check(a.key && !a.params, "motor: quadro completo sem SPS/PPS");
    std::vector<uint8_t> seq = {0,0,0,1, 0x67, 1, 0,0,0,1, 0x68, 2};
    auto w = withParams(idrOnly, sizeof idrOnly, seq, false);
    check(w.size() == seq.size() + sizeof idrOnly && auInfo(w.data(), w.size(), false).params, "motor: põe SPS/PPS na frente do quadro completo");
    const uint8_t pf[] = {0,0,1, 0x41, 5};
    check(withParams(pf, sizeof pf, seq, false).size() == sizeof pf, "motor: quadro parcial passa igual");
    // H.265: VPS(32)=0x40, SPS(33)=0x42, PPS(34)=0x44, IDR_W_RADL(19)=0x26, TRAIL_R(1)=0x02 (tipo << 1)
    const uint8_t h265[] = {0,0,0,1, 0x40, 1, 0,0,0,1, 0x42, 1, 0,0,0,1, 0x44, 1, 0,0,0,1, 0x26, 1, 5};
    a = auInfo(h265, sizeof h265, true);
    check(a.key && a.params && a.nals == 4, "motor: H.265 com VPS/SPS/PPS e IDR");
    const uint8_t h265p[] = {0,0,0,1, 0x02, 1, 7};
    a = auInfo(h265p, sizeof h265p, true);
    check(!a.key && !a.params, "motor: H.265 quadro parcial");
    const uint8_t cra[] = {0,0,1, 0x2a, 1};   // CRA (21)
    check(auInfo(cra, sizeof cra, true).key, "motor: CRA conta como quadro completo");
    check(annexbNals((const uint8_t*)"\x01\x02", 2, false).empty(), "motor: lixo não quebra");
    uint8_t hd[24]; engHeader(hd, "TEF1", 7, 1, 300, 123456789012ULL);
    uint32_t id, fl, sq; uint64_t ts;
    check(engParse(hd, 24, "TEF1", &id, &fl, &sq, &ts) && id == 7 && fl == 1 && sq == 300 && ts == 123456789012ULL, "motor: cabeçalho ida e volta");
    check(!engParse(hd, 24, "TDF1", &id, &fl, &sq, &ts) && !engParse(hd, 20, "TEF1", &id, &fl, &sq, &ts), "motor: cabeçalho errado é recusado");
    FRect r = letterbox(1920, 1080, 1000, 1000);
    check(std::fabs(r.t - 218.75f) < 0.01f && r.l == 0 && r.r == 1000, "motor: vídeo 16:9 numa janela quadrada (faixas em cima e embaixo)");
    r = letterbox(1920, 1080, 1920, 1080);
    check(r.l == 0 && r.t == 0 && r.r == 1920 && r.b == 1080, "motor: mesma proporção ocupa tudo");
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
#include <pdh.h>
#include <pdhmsg.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <memory>
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

/* ---------- uso da placa de vídeo (contadores do Windows) ----------
   Só roda enquanto o Telão pede (painel "Ao vivo" aberto): uma linha de trabalho própria lê os contadores
   "GPU Engine" a cada 1 s e guarda o último resultado; o pedido responde na hora com ele.
   Se ninguém pedir por 8 s, a linha para e solta tudo. */
static CRITICAL_SECTION gGpuLock;
static HANDLE gGpuThread = nullptr;
static volatile LONGLONG gGpuAsked = 0;
static DWORD gGpuRoot = 0;
static std::string gGpuJson;
static bool gGpuNone = false;   // este Windows não tem os contadores: não tenta de novo
typedef PDH_STATUS (WINAPI *PFN_PdhOpenQueryW)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
typedef PDH_STATUS (WINAPI *PFN_PdhAddEnglishCounterW)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
typedef PDH_STATUS (WINAPI *PFN_PdhCollectQueryData)(PDH_HQUERY);
typedef PDH_STATUS (WINAPI *PFN_PdhGetFormattedCounterArrayW)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS (WINAPI *PFN_PdhCloseQuery)(PDH_HQUERY);
static void gpuSetJson(const std::string& j){ EnterCriticalSection(&gGpuLock); gGpuJson = j; LeaveCriticalSection(&gGpuLock); }
static DWORD WINAPI gpuThread(void*){
  HMODULE pdh = LoadLibraryW(L"pdh.dll");
  auto open = pdh ? (PFN_PdhOpenQueryW)GetProcAddress(pdh, "PdhOpenQueryW") : nullptr;
  auto add = pdh ? (PFN_PdhAddEnglishCounterW)GetProcAddress(pdh, "PdhAddEnglishCounterW") : nullptr;
  auto collect = pdh ? (PFN_PdhCollectQueryData)GetProcAddress(pdh, "PdhCollectQueryData") : nullptr;
  auto getArr = pdh ? (PFN_PdhGetFormattedCounterArrayW)GetProcAddress(pdh, "PdhGetFormattedCounterArrayW") : nullptr;
  auto close = pdh ? (PFN_PdhCloseQuery)GetProcAddress(pdh, "PdhCloseQuery") : nullptr;
  PDH_HQUERY q = nullptr; PDH_HCOUNTER ctr = nullptr, cProcMem = nullptr, cAdMem = nullptr;
  bool ok = open && add && collect && getArr && close && open(nullptr, 0, &q) == ERROR_SUCCESS &&
            add(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &ctr) == ERROR_SUCCESS;
  bool vram = ok && add(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &cProcMem) == ERROR_SUCCESS &&
              add(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &cAdMem) == ERROR_SUCCESS;
  // tamanho da memória de cada placa (para "X de Y GB")
  std::vector<std::pair<std::string, double>> vramTotal;
  if (videoApis()){
    IDXGIFactory1* fac = nullptr;
    if (SUCCEEDED(pCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))){
      IDXGIAdapter1* a = nullptr;
      for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++){
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d))){ char k[40]; snprintf(k, sizeof k, "0x%08lx_0x%08lx", (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart); vramTotal.push_back({k, (double)d.DedicatedVideoMemory}); }
        a->Release();
      }
      fac->Release();
    }
  }
  if (!ok){
    gpuSetJson("{\"type\":\"gpu\",\"ok\":false,\"error\":\"este Windows não mostra o uso da placa por processo\"}");
    logf("Uso da placa de vídeo: contadores do Windows indisponíveis");
  } else {
    logf("Uso da placa de vídeo: medindo (painel do Telão aberto)");
    collect(q);
    std::vector<uint32_t> mine; ULONGLONG treeAt = 0;
    std::vector<unsigned char> buf;
    while ((LONGLONG)GetTickCount64() - gGpuAsked < 8000){
      Sleep(1000);
      ULONGLONG now = GetTickCount64();
      if (!treeAt || now - treeAt > 5000){
        std::vector<ProcInfo> ps;
        for (auto& p : listProcesses()) ps.push_back({p.pid, p.parent, utf8(p.exe)});
        mine = gGpuRoot ? processTree(gGpuRoot, ps) : std::vector<uint32_t>();
        mine.push_back(GetCurrentProcessId());   // a captura turbo roda aqui
        treeAt = now;
      }
      if (collect(q) != ERROR_SUCCESS) continue;
      auto read = [&](PDH_HCOUNTER c, std::vector<std::pair<std::string, double>>* v) -> bool {
        DWORD size = 0, count = 0;
        v->clear();
        if (!c || getArr(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != (PDH_STATUS)PDH_MORE_DATA || !size) return false;
        buf.assign(size, 0);
        auto items = (PPDH_FMT_COUNTERVALUE_ITEM_W)buf.data();
        if (getArr(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) return false;
        v->reserve(count);
        for (DWORD i = 0; i < count; i++)
          if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            v->push_back({utf8(items[i].szName ? items[i].szName : L""), items[i].FmtValue.doubleValue});
        return true;
      };
      std::vector<std::pair<std::string, double>> v, pm, am;
      if (!read(ctr, &v)) continue;
      GpuUse u = gpuAggregate(v, mine);
      char b[320];
      int n = snprintf(b, sizeof b, "{\"type\":\"gpu\",\"ok\":true,\"mine\":%.1f,\"enc\":%.1f,\"dec\":%.1f,\"sys\":%.1f,\"n\":%d", u.mine, u.enc, u.dec, u.sys, (int)mine.size());
      if (vram && read(cProcMem, &pm) && read(cAdMem, &am)){
        VramUse vr = vramAggregate(pm, am, mine);
        double total = 0;
        for (auto& t : vramTotal) if (t.first == vr.luid) total = t.second;
        n += snprintf(b + n, sizeof b - n, ",\"vmine\":%.0f,\"vused\":%.0f,\"vtotal\":%.0f", vr.mine / 1048576, vr.used / 1048576, total / 1048576);
      }
      snprintf(b + n, sizeof b - n, "}");
      gpuSetJson(b);
    }
    logf("Uso da placa de vídeo: parou (painel fechado)");
  }
  if (q && close) close(q);
  if (pdh) FreeLibrary(pdh);
  EnterCriticalSection(&gGpuLock);
  CloseHandle(gGpuThread); gGpuThread = nullptr;
  if (!ok) gGpuNone = true;
  LeaveCriticalSection(&gGpuLock);
  return 0;
}
static void gpuRequest(Conn* c, const std::string& msg){
  if (!(c->origin == "null" || c->origin.rfind("file://", 0) == 0)){ wsText(c, "{\"type\":\"gpu\",\"ok\":false,\"error\":\"só no app do Telão\"}"); return; }
  DWORD pid = (DWORD)strtoul(jsonStr(msg, "pid").c_str(), nullptr, 10);
  EnterCriticalSection(&gGpuLock);
  gGpuRoot = gBoostRoot ? gBoostRoot : pid;
  gGpuAsked = (LONGLONG)GetTickCount64();
  if (!gGpuThread && !gGpuNone){ gGpuJson.clear(); gGpuThread = CreateThread(nullptr, 0, gpuThread, nullptr, 0, nullptr); }
  std::string j = gGpuJson.empty() ? "{\"type\":\"gpu\",\"ok\":true,\"wait\":true}" : gGpuJson;
  LeaveCriticalSection(&gGpuLock);
  wsText(c, j);
}

/* ---------- prioridade do Telão na placa de vídeo ----------
   Com o jogo usando a placa quase toda, a captura e a codificação ficam na fila atrás dele (é o mesmo problema
   do OBS). Enquanto a pessoa transmite, o Telão pede ao Windows prioridade "alta" na placa para os processos
   dele; ao parar, volta ao normal. O Windows pode recusar sem permissão de administrador: aí nada muda. */
typedef LONG (APIENTRY *PFN_SetGpuPrio)(HANDLE, int);
static volatile LONG gGprioOn = 0;
static Conn* gGprioOwner = nullptr;
static int gGprioOk = 0, gGprioFail = 0; static LONG gGprioStatus = 0;
static void applyGpuPrio(bool on){
  static PFN_SetGpuPrio fn = (PFN_SetGpuPrio)GetProcAddress(LoadLibraryW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass");
  int ok = 0, fail = 0; LONG last = 0;
  if (fn){
    std::vector<ProcInfo> ps;
    for (auto& p : listProcesses()) ps.push_back({p.pid, p.parent, utf8(p.exe)});
    std::vector<uint32_t> pids = gBoostRoot ? processTree(gBoostRoot, ps) : std::vector<uint32_t>();
    pids.push_back(GetCurrentProcessId());
    for (uint32_t pid : pids){
      HANDLE h = pid == GetCurrentProcessId() ? GetCurrentProcess() : OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
      if (!h){ fail++; continue; }
      LONG st = fn(h, on ? 4 /*HIGH*/ : 2 /*NORMAL*/);
      if (st == 0) ok++; else { fail++; last = st; }
      if (h != GetCurrentProcess()) CloseHandle(h);
    }
  } else fail = 1;
  gGprioOk = ok; gGprioFail = fail; gGprioStatus = last;
}
static void gprioRequest(Conn* c, const std::string& msg){
  if (!(c->origin == "null" || c->origin.rfind("file://", 0) == 0)) return;
  bool on = jsonStr(msg, "on") == "1";
  InterlockedExchange(&gGprioOn, on ? 1 : 0);
  gGprioOwner = on ? c : nullptr;
  applyGpuPrio(on);
  char b[160];
  snprintf(b, sizeof b, "{\"type\":\"gprio\",\"on\":%s,\"ok\":%d,\"fail\":%d,\"status\":\"0x%08lx\"}", on ? "true" : "false", gGprioOk, gGprioFail, (unsigned long)gGprioStatus);
  wsText(c, b);
  if (on || gGprioOk) logf("Prioridade na placa de vídeo: %s (%d processos ok, %d recusados)", on ? "alta" : "normal", gGprioOk, gGprioFail);
}

/* ---------- camada por cima do jogo (nativa) ----------
   Janela transparente, sempre por cima, que não recebe clique nem foco e não aparece em captura de tela
   (WDA_EXCLUDEFROMCAPTURE). Desenho por software (Direct2D sobre uma imagem na memória + DirectWrite) e
   entregue ao Windows com UpdateLayeredWindow: não usa a placa do Telão, então não atrapalha a transmissão.
   Só anima (30 quadros/s) enquanto há algo na tela; sem nada, a janela some e a linha de trabalho fica parada. */
#define OV_TICK_MS 33
struct OvImg { std::vector<ID2D1Bitmap*> frames; std::vector<int> delay; int total = 0; float w = 0, h = 0;
  ID2D1Bitmap* at(double ms) const { if (frames.empty()) return nullptr; if (frames.size() == 1 || total <= 0) return frames[0];
    int t = (int)fmod(ms, (double)total); for (size_t i = 0; i < frames.size(); i++){ if (t < delay[i]) return frames[i]; t -= delay[i]; } return frames.back(); }
  ~OvImg(){ for (auto* b : frames) if (b) b->Release(); } };
struct OvItem { bool chat = false; double born = 0; std::wstring text, who; std::shared_ptr<OvImg> img, av; float xFrac = 0; };
struct OvCmd { int kind = 0; int x = 0, y = 0, w = 0, h = 0; bool chat = false; std::string text, who, img, av; };
static CRITICAL_SECTION gOvLock;
static std::vector<OvCmd*> gOvQueue;
static HWND gOvHwnd = nullptr;
static HANDLE gOvThread = nullptr, gOvReady = nullptr;
static volatile LONG gOvState = 0;   // 0 nada, 1 pronta, -1 não deu (Windows sem Direct2D/DirectWrite)
static Conn* gOvOwner = nullptr;
typedef HRESULT (WINAPI *PFN_D2D1CreateFactory)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
typedef HRESULT (WINAPI *PFN_DWriteCreateFactory)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
template <class T> static void safeRelease(T*& p){ if (p){ p->Release(); p = nullptr; } }

struct Ov {
  ID2D1Factory1* d2 = nullptr; IDWriteFactory* dw = nullptr; IWICImagingFactory* wic = nullptr;
  IWICBitmap* surf = nullptr; ID2D1RenderTarget* rt = nullptr; ID2D1DeviceContext* dc = nullptr;
  ID2D1SolidColorBrush *white = nullptr, *black = nullptr, *green = nullptr, *bubble = nullptr, *label = nullptr;
  IDWriteTextFormat *fEmoji = nullptr, *fBig = nullptr, *fLabel = nullptr, *fChat = nullptr;
  HDC memDC = nullptr; HBITMAP dib = nullptr; void* bits = nullptr;
  int W = 0, H = 0, X = 0, Y = 0; double scale = 1; bool visible = false, timer = false;
  std::vector<OvItem> items;
  std::vector<std::pair<size_t, std::shared_ptr<OvImg>>> cache;   // imagens já decodificadas (fotos de perfil se repetem)
  double t0 = 0;
  double now(){ return nowMs() - t0; }
} gOv;
static thread_local Ov* gOvCur = nullptr;   // janelas de vídeo do motor desenham com o próprio contexto
static Ov& ovCur(){ return gOvCur ? *gOvCur : gOv; }

static void ovFreeTarget(){
  Ov& o = gOv;
  o.items.clear(); o.cache.clear();
  safeRelease(o.white); safeRelease(o.black); safeRelease(o.green); safeRelease(o.bubble); safeRelease(o.label);
  safeRelease(o.fEmoji); safeRelease(o.fBig); safeRelease(o.fLabel); safeRelease(o.fChat);
  safeRelease(o.dc); safeRelease(o.rt); safeRelease(o.surf);
  if (o.memDC){ DeleteDC(o.memDC); o.memDC = nullptr; }
  if (o.dib){ DeleteObject(o.dib); o.dib = nullptr; o.bits = nullptr; }
}
static IDWriteTextFormat* ovFont(float px, DWRITE_FONT_WEIGHT wgt){
  IDWriteTextFormat* f = nullptr;
  if (gOv.dw) gOv.dw->CreateTextFormat(L"Segoe UI", nullptr, wgt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"pt-br", &f);
  return f;
}
// (Re)cria a imagem onde se desenha, no tamanho da faixa.
static bool ovMakeTarget(int w, int h){
  Ov& o = gOv;
  ovFreeTarget();
  o.W = w; o.H = h;
  if (FAILED(o.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &o.surf))) return false;
  D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(o.d2->CreateWicBitmapRenderTarget(o.surf, &rp, &o.rt))) return false;
  o.rt->QueryInterface(__uuidof(ID2D1DeviceContext), (void**)&o.dc);   // emoji colorido (Windows 8.1+); sem isso, sai monocromático
  o.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // ClearType não combina com transparência
  o.rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &o.white);
  o.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.45f), &o.black);
  o.rt->CreateSolidColorBrush(D2D1::ColorF(0x7cf29a, 1), &o.green);
  o.rt->CreateSolidColorBrush(D2D1::ColorF(10 / 255.f, 12 / 255.f, 16 / 255.f, 0.82f), &o.bubble);
  o.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &o.label);
  double s = o.scale;
  o.fEmoji = ovFont((float)(52 * s), DWRITE_FONT_WEIGHT_NORMAL);
  o.fBig = ovFont((float)(30 * s), DWRITE_FONT_WEIGHT_EXTRA_BOLD);
  o.fLabel = ovFont((float)(13 * s), DWRITE_FONT_WEIGHT_NORMAL);
  o.fChat = ovFont((float)(15 * s), DWRITE_FONT_WEIGHT_NORMAL);
  BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
  HDC sdc = GetDC(nullptr);
  o.memDC = CreateCompatibleDC(sdc);
  o.dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &o.bits, nullptr, 0);
  ReleaseDC(nullptr, sdc);
  if (!o.dib || !o.bits) return false;
  SelectObject(o.memDC, o.dib);
  return o.fEmoji && o.fBig && o.fLabel && o.fChat && o.white;
}

// Imagem (foto de perfil ou figurinha; GIF anima) → bitmaps prontos para desenhar, reduzidos a no máximo maxSide.
static std::shared_ptr<OvImg> ovDecode(Ov& o, const std::string& url, int maxSide){
  if (url.empty() || !o.rt) return nullptr;
  size_t key = std::hash<std::string>()(url) ^ (size_t)maxSide;
  for (auto& c : o.cache) if (c.first == key) return c.second;
  std::vector<uint8_t> bytes = dataUrlBytes(url);
  if (bytes.size() < 16) return nullptr;
  IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr;
  auto img = std::make_shared<OvImg>();
  if (SUCCEEDED(o.wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(bytes.data(), (DWORD)bytes.size())) &&
      SUCCEEDED(o.wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))){
    UINT n = 0; dec->GetFrameCount(&n);
    GUID fmt{}; dec->GetContainerFormat(&fmt);
    bool gif = IsEqualGUID(fmt, GUID_ContainerFormatGif) && n > 1;
    n = std::min<UINT>(n, gif ? 60 : 1);
    UINT cw = 0, ch = 0;   // tela lógica do GIF (os quadros podem ser menores e deslocados)
    if (gif){
      IWICMetadataQueryReader* q = nullptr;
      if (SUCCEEDED(dec->GetMetadataQueryReader(&q))){
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"/logscrdesc/Width", &v)) && v.vt == VT_UI2) cw = v.uiVal; PropVariantClear(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"/logscrdesc/Height", &v)) && v.vt == VT_UI2) ch = v.uiVal; PropVariantClear(&v);
        q->Release();
      }
    }
    std::vector<uint8_t> canvas, prev;
    for (UINT i = 0; i < n; i++){
      IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
      if (FAILED(dec->GetFrame(i, &fr))) break;
      UINT fw = 0, fh = 0; fr->GetSize(&fw, &fh);
      int left = 0, top = 0, delay = 100, disposal = 0;
      if (gif){
        IWICMetadataQueryReader* q = nullptr;
        if (SUCCEEDED(fr->GetMetadataQueryReader(&q))){
          PROPVARIANT v; PropVariantInit(&v);
          if (SUCCEEDED(q->GetMetadataByName(L"/imgdesc/Left", &v)) && v.vt == VT_UI2) left = v.uiVal; PropVariantClear(&v);
          if (SUCCEEDED(q->GetMetadataByName(L"/imgdesc/Top", &v)) && v.vt == VT_UI2) top = v.uiVal; PropVariantClear(&v);
          if (SUCCEEDED(q->GetMetadataByName(L"/grctlext/Delay", &v)) && v.vt == VT_UI2) delay = std::max(20, v.uiVal * 10); PropVariantClear(&v);
          if (SUCCEEDED(q->GetMetadataByName(L"/grctlext/Disposal", &v)) && v.vt == VT_UI1) disposal = v.bVal; PropVariantClear(&v);
          q->Release();
        }
        if (!cw || !ch){ cw = fw; ch = fh; }
      } else { cw = fw; ch = fh; }
      if (!cw || !ch || cw > 4096 || ch > 4096){ fr->Release(); break; }
      std::vector<uint8_t> px((size_t)fw * fh * 4);
      bool ok = SUCCEEDED(o.wic->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(cv->CopyPixels(nullptr, fw * 4, (UINT)px.size(), px.data()));
      if (cv) cv->Release(); fr->Release();
      if (!ok) break;
      if (canvas.empty()) canvas.assign((size_t)cw * ch * 4, 0);
      if (disposal == 3) prev = canvas;
      for (UINT yy = 0; yy < fh; yy++){   // "source over" do quadro sobre a tela do GIF
        int cy = top + (int)yy; if (cy < 0 || cy >= (int)ch) continue;
        for (UINT xx = 0; xx < fw; xx++){
          int cx = left + (int)xx; if (cx < 0 || cx >= (int)cw) continue;
          const uint8_t* s = &px[((size_t)yy * fw + xx) * 4]; uint8_t* d = &canvas[((size_t)cy * cw + cx) * 4];
          unsigned a = s[3]; if (!a) continue;
          for (int k = 0; k < 4; k++) d[k] = (uint8_t)(s[k] + d[k] * (255 - a) / 255);
        }
      }
      // reduz (fotos de perfil e figurinhas aparecem pequenas)
      double k = std::min(1.0, (double)maxSide / std::max(cw, ch));
      UINT ow = std::max(1u, (UINT)std::lround(cw * k)), oh = std::max(1u, (UINT)std::lround(ch * k));
      std::vector<uint8_t> outPx;
      const uint8_t* src = canvas.data(); UINT pitch = cw * 4;
      if (ow != cw || oh != ch){
        IWICBitmap* cb = nullptr; IWICBitmapScaler* sc = nullptr;
        outPx.resize((size_t)ow * oh * 4);
        bool sok = SUCCEEDED(o.wic->CreateBitmapFromMemory(cw, ch, GUID_WICPixelFormat32bppPBGRA, cw * 4, (UINT)canvas.size(), canvas.data(), &cb)) &&
                   SUCCEEDED(o.wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(cb, ow, oh, WICBitmapInterpolationModeFant)) &&
                   SUCCEEDED(sc->CopyPixels(nullptr, ow * 4, (UINT)outPx.size(), outPx.data()));
        if (sc) sc->Release(); if (cb) cb->Release();
        if (!sok){ ow = cw; oh = ch; outPx.clear(); } else { src = outPx.data(); pitch = ow * 4; }
      }
      ID2D1Bitmap* b = nullptr;
      D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      if (SUCCEEDED(o.rt->CreateBitmap(D2D1::SizeU(ow, oh), src, pitch, &bp, &b))){ img->frames.push_back(b); img->delay.push_back(delay); img->total += delay; img->w = (float)ow; img->h = (float)oh; }
      if (disposal == 2){   // volta ao fundo (transparente) na área deste quadro
        for (UINT yy = 0; yy < fh; yy++){ int cy = top + (int)yy; if (cy < 0 || cy >= (int)ch) continue;
          for (UINT xx = 0; xx < fw; xx++){ int cx = left + (int)xx; if (cx < 0 || cx >= (int)cw) continue; memset(&canvas[((size_t)cy * cw + cx) * 4], 0, 4); } }
      } else if (disposal == 3 && !prev.empty()) canvas = prev;
    }
  }
  if (dec) dec->Release(); if (st) st->Release();
  if (img->frames.empty()) return nullptr;
  o.cache.push_back({key, img});
  if (o.cache.size() > 40) o.cache.erase(o.cache.begin());
  return img;
}

static IDWriteTextLayout* ovLayout(const std::wstring& t, IDWriteTextFormat* f, float maxW){
  IDWriteTextLayout* l = nullptr;
  if (ovCur().dw && f) ovCur().dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), f, maxW, 10000, &l);
  return l;
}
static void ovText(IDWriteTextLayout* l, float x, float y, ID2D1Brush* b){
  Ov& o = ovCur();
  if (o.dc) o.dc->DrawTextLayout(D2D1::Point2F(x, y), l, b, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  else o.rt->DrawTextLayout(D2D1::Point2F(x, y), l, b);
}
static void ovCircleImg(ID2D1Bitmap* b, float x, float y, float d, float alpha){
  Ov& o = ovCur();
  if (!b) return;
  ID2D1EllipseGeometry* g = nullptr; ID2D1Layer* layer = nullptr;
  if (SUCCEEDED(o.d2->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(x + d / 2, y + d / 2), d / 2, d / 2), &g)) && SUCCEEDED(o.rt->CreateLayer(&layer))){
    o.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g), layer);
    o.rt->DrawBitmap(b, D2D1::RectF(x, y, x + d, y + d), alpha);
    o.rt->PopLayer();
  }
  safeRelease(layer); safeRelease(g);
}

// Uma reação: conteúdo (emoji, texto curto ou figurinha) com o nome embaixo, subindo e sumindo.
static void ovDrawReact(const OvItem& it, double t){
  Ov& o = ovCur(); double s = o.scale;
  RxPose p = rxPose(t);
  if (p.alpha <= 0.003) return;
  IDWriteTextLayout* body = nullptr; float bw, bh;
  bool big = !it.img && it.text.size() > 2;
  if (it.img){ bw = bh = (float)(96 * s); }
  else {
    body = ovLayout(it.text, big ? o.fBig : o.fEmoji, (float)o.W);
    if (!body) return;
    body->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{}; body->GetMetrics(&m); bw = m.widthIncludingTrailingWhitespace; bh = m.height;
  }
  IDWriteTextLayout* nm = ovLayout(it.who, o.fLabel, (float)o.W);
  float nw = 0, nh = (float)(13 * s * 1.3);
  if (nm){ nm->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); DWRITE_TEXT_METRICS m{}; nm->GetMetrics(&m); nw = m.widthIncludingTrailingWhitespace; nh = m.height; }
  float av = it.av ? (float)(18 * s) : 0, gap = it.av ? (float)(5 * s) : 0;
  float lw = (float)(16 * s) + av + gap + nw, lh = std::max(av, nh) + (float)(4 * s);
  float ew = std::max(bw, lw), eh = bh + (float)(3 * s) + lh;
  float left = (float)(it.xFrac * o.W), bottom = (float)(o.H - o.H * 0.06);
  float cx = left + ew / 2, cy = bottom - eh / 2 + (float)(p.dy * eh);
  o.rt->SetTransform(D2D1::Matrix3x2F::Scale((float)p.scale, (float)p.scale, D2D1::Point2F(cx, cy)));
  float x0 = cx - ew / 2, y0 = cy - eh / 2;
  float a = (float)p.alpha;
  ID2D1Layer* layer = nullptr;
  bool fade = a < 0.999f && SUCCEEDED(o.rt->CreateLayer(&layer));
  if (fade) o.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), layer);
  if (it.img){
    ID2D1Bitmap* b = it.img->at(t);
    float k = std::min(bw / std::max(1.f, it.img->w), bh / std::max(1.f, it.img->h)), iw = it.img->w * k, ih = it.img->h * k;
    if (b) o.rt->DrawBitmap(b, D2D1::RectF(cx - iw / 2, y0 + (bh - ih) / 2, cx + iw / 2, y0 + (bh + ih) / 2));
  } else {
    float bx = cx - bw / 2;
    if (big){   // contorno escuro para ler em cima de qualquer jogo
      float d = (float)(1.2 * s);
      for (int i = 0; i < 4; i++) ovText(body, bx + (i == 0 ? -d : i == 1 ? d : 0), y0 + (i == 2 ? -d : i == 3 ? d : 0), o.black);
      ovText(body, bx, y0, o.white);
    } else {
      ovText(body, bx, y0 + (float)(3 * s), o.black);   // sombra
      ovText(body, bx, y0, o.white);
    }
  }
  float lx = cx - lw / 2, ly = y0 + bh + (float)(3 * s);
  o.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(lx, ly, lx + lw, ly + lh), (float)(6 * s), (float)(6 * s)), o.label);
  if (it.av) ovCircleImg(it.av->at(t), lx + (float)(8 * s), ly + (lh - av) / 2, av, 1);
  if (nm) ovText(nm, lx + (float)(8 * s) + av + gap, ly + (lh - nh) / 2, o.white);
  if (fade) o.rt->PopLayer();
  safeRelease(layer);
  o.rt->SetTransform(D2D1::Matrix3x2F::Identity());
  safeRelease(body); safeRelease(nm);
}

// Mensagens do chat: empilhadas a partir de baixo, alinhadas à direita; a mais nova embaixo.
static void ovDrawChat(double now){
  Ov& o = ovCur(); double s = o.scale;
  float maxW = (float)(o.W - 16 * s), y = (float)(o.H - 12 * s);
  for (int i = (int)o.items.size() - 1; i >= 0; i--){
    const OvItem& it = o.items[i];
    if (!it.chat) continue;
    ChatPose p = chatPose(now - it.born);
    if (p.gone) continue;
    float av = it.av ? (float)(26 * s) : 0, gap = it.av ? (float)(7 * s) : 0;
    float padX = (float)(10 * s), padY = (float)(6 * s);
    float textMax = std::max(40.f, maxW - 2 * padX - av - gap);
    std::wstring full = it.who + (it.text.empty() ? L"" : L"  " + it.text);
    IDWriteTextLayout* l = ovLayout(full, o.fChat, textMax);
    if (!l) continue;
    DWRITE_TEXT_RANGE nr{0, (UINT32)it.who.size()};
    l->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, nr);
    l->SetDrawingEffect(o.green, nr);
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m);
    float tw = m.widthIncludingTrailingWhitespace, th = m.height;
    float sw = 0, sh = 0;
    if (it.img){ float k = std::min((float)(120 * s) / std::max(1.f, it.img->w), (float)(120 * s) / std::max(1.f, it.img->h)); k = std::min(k, (float)s); sw = it.img->w * k; sh = it.img->h * k; }
    float bodyW = std::max(tw, sw), bodyH = th + (it.img ? (float)(4 * s) + sh : 0);
    float bw = 2 * padX + av + gap + bodyW, bh = 2 * padY + std::max(av, bodyH);
    float bx = (float)(o.W - 8 * s) - bw, by = y - bh + (float)(p.dy * s);
    ID2D1Layer* layer = nullptr;
    bool fade = p.alpha < 0.999 && SUCCEEDED(o.rt->CreateLayer(&layer));
    if (fade) o.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), (float)p.alpha), layer);
    o.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), (float)(10 * s), (float)(10 * s)), o.bubble);
    if (it.av) ovCircleImg(it.av->at(now - it.born), bx + padX, by + padY + (float)(1 * s), av, 1);
    float tx = bx + padX + av + gap;
    ovText(l, tx, by + padY, o.white);
    if (it.img){
      ID2D1Bitmap* b = it.img->at(now - it.born);
      if (b) o.rt->DrawBitmap(b, D2D1::RectF(tx, by + padY + th + (float)(4 * s), tx + sw, by + padY + th + (float)(4 * s) + sh));
    }
    if (fade) o.rt->PopLayer();
    safeRelease(layer); safeRelease(l);
    y = by - (float)(6 * s);
    if (y < 0) break;
  }
}

static void ovPresent(){
  Ov& o = gOv;
  WICRect r{0, 0, o.W, o.H}; IWICBitmapLock* lk = nullptr;
  if (FAILED(o.surf->Lock(&r, WICBitmapLockRead, &lk))) return;
  UINT stride = 0, size = 0; BYTE* p = nullptr;
  lk->GetStride(&stride); lk->GetDataPointer(&size, &p);
  if (p) for (int y = 0; y < o.H; y++) memcpy((uint8_t*)o.bits + (size_t)y * o.W * 4, p + (size_t)y * stride, (size_t)o.W * 4);
  lk->Release();
  POINT pos{o.X, o.Y}, zero{0, 0}; SIZE sz{o.W, o.H};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(gOvHwnd, nullptr, &pos, &sz, o.memDC, &zero, 0, &bf, ULW_ALPHA);
}
// Teste: com TELAO_OV_DUMP=<pasta>, cada quadro desenhado também vai para um PNG (só para conferir o desenho).
static void ovDump(){
  static wchar_t dir[MAX_PATH] = L"?";
  if (dir[0] == L'?'){ if (!GetEnvironmentVariableW(L"TELAO_OV_DUMP", dir, MAX_PATH)) dir[0] = 0; }
  if (!dir[0]) return;
  static int n = 0; wchar_t path[MAX_PATH + 32]; swprintf(path, MAX_PATH + 32, L"%ls\\ov-%03d.png", dir, n++);
  IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fe = nullptr;
  if (SUCCEEDED(gOv.wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
      SUCCEEDED(gOv.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&fe, nullptr)) && SUCCEEDED(fe->Initialize(nullptr))){
    fe->SetSize(gOv.W, gOv.H); WICPixelFormatGUID pf = GUID_WICPixelFormat32bppPBGRA; fe->SetPixelFormat(&pf);
    fe->WriteSource(gOv.surf, nullptr); fe->Commit(); enc->Commit();
  }
  safeRelease(fe); safeRelease(enc); safeRelease(st);
}
static void ovFrame(){
  Ov& o = gOv;
  if (!o.rt) return;
  double now = o.now();
  o.items.erase(std::remove_if(o.items.begin(), o.items.end(), [&](const OvItem& it){ return now - it.born > (it.chat ? 8900 : 3000); }), o.items.end());
  int chats = 0;
  for (int i = (int)o.items.size() - 1; i >= 0; i--) if (o.items[i].chat && ++chats > 5) o.items.erase(o.items.begin() + i);
  o.rt->BeginDraw();
  o.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  for (auto& it : o.items) if (!it.chat) ovDrawReact(it, now - it.born);
  ovDrawChat(now);
  HRESULT ehr = o.rt->EndDraw();
  if (FAILED(ehr)){ static int said = 0; if (said++ < 3) logf("Camada: falha ao desenhar (0x%08lx)", (unsigned long)ehr); return; }
  ovDump();
  if (o.visible) ovPresent();
  if (o.items.empty() && o.visible){   // nada mais animando: some e para
    ShowWindow(gOvHwnd, SW_HIDE); o.visible = false;
    if (o.timer){ KillTimer(gOvHwnd, 1); o.timer = false; }
  }
}
static void ovApply(OvCmd* c){
  Ov& o = gOv;
  if (c->kind == 1){   // mostrar (área do jogo)
    o.scale = 1;
    MONITORINFO mi{}; mi.cbSize = sizeof mi;
    RECT ar{c->x, c->y, c->x + c->w, c->y + c->h};
    HMONITOR mon = MonitorFromRect(&ar, MONITOR_DEFAULTTONEAREST);
    typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor gdm = (PFN_GetDpiForMonitor)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    UINT dx = 96, dy = 96; if (gdm && SUCCEEDED(gdm(mon, 0, &dx, &dy)) && dx >= 96) o.scale = dx / 96.0;
    ORect r = overlayRect(c->x, c->y, c->w, c->h, o.scale);
    if (r.w != o.W || r.h != o.H || !o.rt){ if (!ovMakeTarget(r.w, r.h)) ovFreeTarget(); }
    o.X = r.x; o.Y = r.y;
    if (o.visible) SetWindowPos(gOvHwnd, HWND_TOPMOST, o.X, o.Y, o.W, o.H, SWP_NOACTIVATE);
  } else if (c->kind == 2){   // esconder
    o.items.clear();
    if (o.visible){ ShowWindow(gOvHwnd, SW_HIDE); o.visible = false; }
    if (o.timer){ KillTimer(gOvHwnd, 1); o.timer = false; }
  } else if (c->kind == 3 && o.rt){   // reação ou mensagem
    OvItem it; it.chat = c->chat; it.born = o.now();
    it.text = wide(c->text); it.who = wide(c->who);
    if (!c->img.empty()) it.img = ovDecode(gOv, c->img, (int)std::lround((c->chat ? 120 : 96) * o.scale * 1.5));
    if (!c->av.empty()) it.av = ovDecode(gOv, c->av, (int)std::lround(26 * o.scale * 2));
    if (it.text.empty() && !it.img) return;
    it.xFrac = (float)(0.08 + (rand() % 1000) / 1000.0 * 0.52);
    if (!it.chat && std::count_if(o.items.begin(), o.items.end(), [](const OvItem& x){ return !x.chat; }) > 24) return;
    o.items.push_back(std::move(it));
    if (!o.visible){
      ovFrame();
      ovPresent();
      SetWindowPos(gOvHwnd, HWND_TOPMOST, o.X, o.Y, o.W, o.H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
      o.visible = true;
    }
    if (!o.timer){ SetTimer(gOvHwnd, 1, OV_TICK_MS, nullptr); o.timer = true; }
  }
}
static LRESULT CALLBACK ovProc(HWND h, UINT m, WPARAM w, LPARAM l){
  if (m == WM_APP + 1){
    std::vector<OvCmd*> q;
    EnterCriticalSection(&gOvLock); q.swap(gOvQueue); LeaveCriticalSection(&gOvLock);
    for (auto* c : q){ ovApply(c); delete c; }
    return 0;
  }
  if (m == WM_TIMER && w == 1){ ovFrame(); return 0; }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(h, m, w, l);
}
static DWORD WINAPI ovThread(void*){
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Ov& o = gOv; o.t0 = nowMs();
  HMODULE d2m = LoadLibraryW(L"d2d1.dll"), dwm = LoadLibraryW(L"dwrite.dll");
  auto mkD2 = d2m ? (PFN_D2D1CreateFactory)GetProcAddress(d2m, "D2D1CreateFactory") : nullptr;
  auto mkDw = dwm ? (PFN_DWriteCreateFactory)GetProcAddress(dwm, "DWriteCreateFactory") : nullptr;
  bool ok = mkD2 && mkDw &&
            SUCCEEDED(mkD2(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&o.d2)) &&
            SUCCEEDED(mkDw(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&o.dw)) &&
            SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)&o.wic));
  if (ok){
    WNDCLASSEXW wc{}; wc.cbSize = sizeof wc; wc.lpfnWndProc = ovProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"TelaoCamada";
    RegisterClassExW(&wc);
    gOvHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"TelaoCamada", L"Telão — por cima do jogo", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    ok = gOvHwnd != nullptr;
    if (ok && !SetWindowDisplayAffinity(gOvHwnd, 0x11 /*WDA_EXCLUDEFROMCAPTURE*/)) SetWindowDisplayAffinity(gOvHwnd, 1 /*WDA_MONITOR*/);
  }
  InterlockedExchange(&gOvState, ok ? 1 : -1);
  logf(ok ? "Camada por cima do jogo: pronta (desenhada pelo Telão Audio)" : "Camada por cima do jogo: este Windows não tem Direct2D/DirectWrite; fica a do app");
  SetEvent(gOvReady);
  if (!ok){ CoUninitialize(); return 0; }
  PostMessageW(gOvHwnd, WM_APP + 1, 0, 0);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0){ TranslateMessage(&msg); DispatchMessageW(&msg); }
  return 0;
}
static bool ovEnsure(){
  EnterCriticalSection(&gOvLock);
  if (!gOvThread){ gOvReady = CreateEventW(nullptr, TRUE, FALSE, nullptr); gOvThread = CreateThread(nullptr, 0, ovThread, nullptr, 0, nullptr); }
  LeaveCriticalSection(&gOvLock);
  if (gOvReady) WaitForSingleObject(gOvReady, 3000);
  return gOvState == 1;
}
static void ovPost(OvCmd* c){
  EnterCriticalSection(&gOvLock); gOvQueue.push_back(c); LeaveCriticalSection(&gOvLock);
  if (gOvHwnd) PostMessageW(gOvHwnd, WM_APP + 1, 0, 0);
}
static void overlayCommand(Conn* c, const std::string& cmd, const std::string& msg){
  if (!(c->origin == "null" || c->origin.rfind("file://", 0) == 0)) return;
  if (cmd == "ovshow"){
    bool ok = ovEnsure();
    if (ok){
      OvCmd* o = new OvCmd; o->kind = 1;
      o->x = atoi(jsonStr(msg, "x").c_str()); o->y = atoi(jsonStr(msg, "y").c_str()); o->w = atoi(jsonStr(msg, "w").c_str()); o->h = atoi(jsonStr(msg, "h").c_str());
      if (o->w < 100 || o->h < 100){ delete o; ok = false; } else { gOvOwner = c; ovPost(o); }
    }
    wsText(c, ok ? "{\"type\":\"ovshow\",\"ok\":true}" : "{\"type\":\"ovshow\",\"ok\":false}");
  } else if (cmd == "ovhide"){
    if (gOvState == 1){ OvCmd* o = new OvCmd; o->kind = 2; ovPost(o); }
  } else if (cmd == "ovitem" && gOvState == 1 && gOvOwner == c){
    OvCmd* o = new OvCmd; o->kind = 3;
    o->chat = jsonStr(msg, "kind") == "chat";
    o->text = jsonStr(msg, "text").substr(0, o->chat ? 660 : 64); o->who = jsonStr(msg, "who").substr(0, 96);
    o->img = jsonStr(msg, "img"); o->av = jsonStr(msg, "av");
    if (o->img.size() > 1200000) o->img.clear();
    if (o->av.size() > 1200000) o->av.clear();
    ovPost(o);
  }
}

#include <deque>
#include "motor.inc"

static void handleCommand(Conn* c, const std::string& msg){
  std::string cmd = jsonStr(msg, "cmd");
  if (engineCommand(c, cmd, msg)) return;
  if (cmd == "list"){ wsText(c, buildAppList()); return; }
  if (cmd == "stop"){ stopCapture(c); return; }
  if (cmd == "vstart"){ startVideo(c, msg); return; }
  if (cmd == "vstop"){ stopVideo(c); return; }
  if (cmd == "gpu"){ gpuRequest(c, msg); return; }
  if (cmd == "gprio"){ gprioRequest(c, msg); return; }
  if (cmd == "ovshow" || cmd == "ovhide" || cmd == "ovitem"){ overlayCommand(c, cmd, msg); return; }
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
      if (len > (3u << 20)) break;   // até ~3 MB (figurinha + foto de perfil na camada por cima do jogo)
      unsigned char mask[4] = {0, 0, 0, 0};
      if (masked && !recvExact(c->s, (char*)mask, 4)) break;
      std::string p((size_t)len, '\0');
      if (len && !recvExact(c->s, &p[0], (int)len)) break;
      for (size_t i = 0; i < p.size(); i++) p[i] ^= mask[i & 3];
      if (op == 0x8){ wsSend(c, 0x8, nullptr, 0); break; }
      if (op == 0x9){ wsSend(c, 0xA, p.data(), p.size()); continue; }
      if (op == 0x1) handleCommand(c, p);
      else if (op == 0x2) presFrame(c, p);   // quadro do motor para decodificar
    }
    logf("Telão desconectado");
  }
  InterlockedExchange(&c->alive, 0);
  stopCapture(c);
  stopVideo(c);
  engineClosed(c);
  if (gGprioOwner == c){ gGprioOwner = nullptr; InterlockedExchange(&gGprioOn, 0); applyGpuPrio(false); }
  if (gOvOwner == c && gOvState == 1){ gOvOwner = nullptr; OvCmd* o = new OvCmd; o->kind = 2; ovPost(o); }
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
    if (gGprioOn) applyGpuPrio(true);   // processos novos do Telão (ex.: a parte de vídeo reiniciou) também
    if (n != lastCount){ logf("Modo desempenho aplicado a %u processos do Telão", (unsigned)n); lastCount = n; }
    Sleep(3000);
  }
}

/* ---------- ícone do Telao.exe ----------
   O Telão Desktop pede isto ao fechar (uma vez): grava o ícone do Telão dentro do Telao.exe instalado,
   sem baixar o app inteiro. Espera o app fechar, trabalha numa cópia, confere a cópia e só então troca
   (o original fica como Telao.exe.old). Qualquer falha deixa o exe como estava. */
#define WRT_ICON MAKEINTRESOURCEW(3)
#define WRT_GROUP_ICON MAKEINTRESOURCEW(14)
struct ResName { bool isInt = true; WORD id = 0; std::wstring s; LPCWSTR ptr() const { return isInt ? MAKEINTRESOURCEW(id) : s.c_str(); } };
struct OldGroup { ResName name; WORD lang = 0; std::vector<WORD> icons; };
static BOOL CALLBACK enumGroupLang(HMODULE m, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR p){
  auto* v = (std::vector<OldGroup>*)p;
  OldGroup g;
  if (IS_INTRESOURCE(name)){ g.name.isInt = true; g.name.id = (WORD)(uintptr_t)name; } else { g.name.isInt = false; g.name.s = name; }
  g.lang = lang;
  HRSRC r = FindResourceExW(m, type, name, lang);
  HGLOBAL h = r ? LoadResource(m, r) : nullptr;
  const uint8_t* d = h ? (const uint8_t*)LockResource(h) : nullptr;
  DWORD sz = r ? SizeofResource(m, r) : 0;
  if (d && sz >= 6){ uint16_t n = d[4] | (d[5] << 8); for (uint16_t i = 0; i < n && 6 + 14u * (i + 1) <= sz; i++){ size_t e = 6 + 14u * i; g.icons.push_back((WORD)(d[e + 12] | (d[e + 13] << 8))); } }
  v->push_back(g);
  return TRUE;
}
static BOOL CALLBACK enumGroupName(HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR p){
  EnumResourceLanguagesW(m, type, name, enumGroupLang, p);
  return TRUE;
}
static BOOL CALLBACK enumIconName(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR p){
  if (IS_INTRESOURCE(name)){ WORD* mx = (WORD*)p; *mx = std::max(*mx, (WORD)(uintptr_t)name); }
  return TRUE;
}
static FILE* gIconLog = nullptr;
static void ilog(const char* fmt, ...){
  if (!gIconLog) return;
  SYSTEMTIME t; GetLocalTime(&t);
  fprintf(gIconLog, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  va_list a; va_start(a, fmt); vfprintf(gIconLog, fmt, a); va_end(a);
  fputc('\n', gIconLog); fflush(gIconLog);
}
static bool readFileW(const std::wstring& p, std::vector<uint8_t>* out){
  HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
  bool ok = sz.QuadPart > 0 && sz.QuadPart < (1 << 20);
  if (ok){ out->resize((size_t)sz.QuadPart); DWORD rd = 0; ok = ReadFile(f, out->data(), (DWORD)out->size(), &rd, nullptr) && rd == out->size(); }
  CloseHandle(f);
  return ok;
}
static bool writeIcon(const std::wstring& tmp, const std::vector<IcoImg>& imgs, std::vector<uint8_t>* groupOut, ResName* nameOut, WORD* langOut){
  std::vector<OldGroup> groups; WORD maxIcon = 0;
  HMODULE m = LoadLibraryExW(tmp.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!m){ ilog("não consegui abrir a cópia do exe (%lu)", GetLastError()); return false; }
  EnumResourceNamesW(m, WRT_GROUP_ICON, enumGroupName, (LONG_PTR)&groups);
  EnumResourceNamesW(m, WRT_ICON, enumIconName, (LONG_PTR)&maxIcon);
  FreeLibrary(m);
  ResName name; name.isInt = true; name.id = 1; WORD lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
  if (!groups.empty()){ name = groups[0].name; lang = groups[0].lang; }
  if ((size_t)maxIcon + imgs.size() > 0xFFF0){ ilog("números de recurso esgotados"); return false; }
  WORD first = (WORD)(maxIcon + 1);
  HANDLE h = BeginUpdateResourceW(tmp.c_str(), FALSE);
  if (!h){ ilog("BeginUpdateResource falhou (%lu)", GetLastError()); return false; }
  bool ok = true;
  for (auto& g : groups){   // o ícone antigo sai inteiro (imagens e grupo)
    for (WORD id : g.icons) UpdateResourceW(h, WRT_ICON, MAKEINTRESOURCEW(id), g.lang, nullptr, 0);
    if (&g != &groups[0]) UpdateResourceW(h, WRT_GROUP_ICON, g.name.ptr(), g.lang, nullptr, 0);
  }
  for (size_t i = 0; ok && i < imgs.size(); i++)
    ok = UpdateResourceW(h, WRT_ICON, MAKEINTRESOURCEW(first + i), lang, (void*)imgs[i].data.data(), (DWORD)imgs[i].data.size()) != FALSE;
  std::vector<uint8_t> grp = grpIconDir(imgs, first);
  if (ok) ok = UpdateResourceW(h, WRT_GROUP_ICON, name.ptr(), lang, grp.data(), (DWORD)grp.size()) != FALSE;
  if (!ok) ilog("UpdateResource falhou (%lu)", GetLastError());
  if (!EndUpdateResourceW(h, ok ? FALSE : TRUE)){ ilog("EndUpdateResource falhou (%lu)", GetLastError()); return false; }
  *groupOut = grp; *nameOut = name; *langOut = lang;
  return ok;
}
static bool verifyIcon(const std::wstring& tmp, const std::vector<IcoImg>& imgs, const std::vector<uint8_t>& grp, const ResName& name, WORD lang){
  HMODULE m = LoadLibraryExW(tmp.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!m) return false;
  bool ok = false;
  HRSRC r = FindResourceExW(m, WRT_GROUP_ICON, name.ptr(), lang);
  if (r && SizeofResource(m, r) == grp.size()){
    const uint8_t* d = (const uint8_t*)LockResource(LoadResource(m, r));
    ok = d && memcmp(d, grp.data(), grp.size()) == 0;
    for (size_t i = 0; ok && i < imgs.size(); i++){
      size_t e = 6 + 14 * i; WORD id = (WORD)(grp[e + 12] | (grp[e + 13] << 8));
      HRSRC ri = FindResourceExW(m, WRT_ICON, MAKEINTRESOURCEW(id), lang);
      const uint8_t* di = ri ? (const uint8_t*)LockResource(LoadResource(m, ri)) : nullptr;
      ok = di && SizeofResource(m, ri) == imgs[i].data.size() && memcmp(di, imgs[i].data.data(), imgs[i].data.size()) == 0;
    }
  }
  FreeLibrary(m);
  return ok;
}
// telao-audio.exe --patch-icon <pid do Telão> <Telao.exe> <icone.ico> <marca> <log>
static int patchIconMain(int argc, wchar_t** argv){
  if (argc < 7) return 2;
  DWORD pid = (DWORD)wcstoul(argv[2], nullptr, 10);
  std::wstring exe = argv[3], ico = argv[4], marker = argv[5];
  gIconLog = _wfopen(argv[6], L"a");
  ilog("ícone: começando");
  HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (ph){ DWORD w = WaitForSingleObject(ph, 30000); CloseHandle(ph); if (w != WAIT_OBJECT_0){ ilog("o Telão não fechou; nada mudou"); return 3; } }
  Sleep(800);
  std::vector<uint8_t> icoBytes; std::vector<IcoImg> imgs;
  if (!readFileW(ico, &icoBytes) || !parseIco(icoBytes, &imgs)){ ilog("ícone novo ilegível"); return 4; }
  std::wstring tmp = exe + L".icone.tmp", old = exe + L".old";
  DeleteFileW(tmp.c_str());
  if (!CopyFileW(exe.c_str(), tmp.c_str(), FALSE)){ ilog("não consegui copiar o exe (%lu)", GetLastError()); return 5; }
  std::vector<uint8_t> grp; ResName name; WORD lang = 0;
  if (!writeIcon(tmp, imgs, &grp, &name, &lang) || !verifyIcon(tmp, imgs, grp, name, lang)){
    ilog("a cópia não ficou certa; o exe continua como estava");
    DeleteFileW(tmp.c_str()); return 6;
  }
  // troca: o original vira .old (renomear funciona mesmo se algo ainda estiver usando o exe)
  bool moved = false;
  for (int k = 0; k < 20 && !moved; k++){ moved = MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE; if (!moved) Sleep(500); }
  if (!moved){ ilog("não consegui trocar o exe (%lu); nada mudou", GetLastError()); DeleteFileW(tmp.c_str()); return 7; }
  if (!MoveFileExW(tmp.c_str(), exe.c_str(), 0)){
    ilog("falha ao colocar o exe novo (%lu); voltando o original", GetLastError());
    MoveFileExW(old.c_str(), exe.c_str(), 0); DeleteFileW(tmp.c_str()); return 8;
  }
  HANDLE mk = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (mk != INVALID_HANDLE_VALUE){ std::string u = utf8(exe); DWORD wr = 0; WriteFile(mk, u.data(), (DWORD)u.size(), &wr, nullptr); CloseHandle(mk); }
  // avisa o Explorer: atalhos e a pasta mostram o ícone novo sem precisar reiniciar
  typedef void (WINAPI *PFN_SHChangeNotify)(LONG, UINT, LPCVOID, LPCVOID);
  HMODULE sh = LoadLibraryW(L"shell32.dll");
  auto notify = sh ? (PFN_SHChangeNotify)GetProcAddress(sh, "SHChangeNotify") : nullptr;
  if (notify){ notify(0x00002000 /*SHCNE_UPDATEITEM*/, 0x0005 /*SHCNF_PATHW*/, exe.c_str(), nullptr); notify(0x08000000 /*SHCNE_ASSOCCHANGED*/, 0, nullptr, nullptr); }
  ilog("ícone aplicado (%u imagens)", (unsigned)imgs.size());
  return 0;
}

int main(int argc, char** argv){
  {
    typedef LPWSTR* (WINAPI *PFN_CmdToArgv)(LPCWSTR, int*);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    auto toArgv = sh ? (PFN_CmdToArgv)GetProcAddress(sh, "CommandLineToArgvW") : nullptr;
    int wc = 0; wchar_t** wv = toArgv ? toArgv(GetCommandLineW(), &wc) : nullptr;
    if (wv && wc >= 2 && wcscmp(wv[1], L"--patch-icon") == 0) return patchIconMain(wc, wv);
  }
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
  InitializeCriticalSection(&gGpuLock);
  InitializeCriticalSection(&gOvLock);
  InitializeCriticalSection(&gOriginLock);
  InitializeCriticalSection(&gCapLock);
  InitializeCriticalSection(&gVidLock);
  InitializeCriticalSection(&gMFLock);
  InitializeCriticalSection(&gEngLock);
  InitializeCriticalSection(&gPresLock);
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
