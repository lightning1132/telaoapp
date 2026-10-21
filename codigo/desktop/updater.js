// Atualização automática do Telão Desktop.
//
// Como funciona:
//   - O repositório publica um latest.json:
//       {"version": "1.5.0", "minShell": 2, "page": "telao.html", "sha256": "...",
//        "download": "https://github.com/.../releases/tag/v1.5.0", "notes": ["..."]}
//   - Se só a interface (telao.html) mudou e o app instalado é compatível (minShell <= SHELL_VERSION),
//     a página nova é baixada, conferida (sha256 + versão declarada dentro dela) e guardada em
//     userData/page. Ela passa a ser usada no próximo início (ou na hora, se a pessoa aceitar).
//   - Se a versão nova precisa de um app mais novo (minShell maior), só avisa e oferece o download.
//   - Rede de segurança: se a página baixada não avisar que abriu em 15 s, ou travar,
//     ela é marcada como ruim e o app volta para a página que veio instalada.
'use strict';
const {app, net} = require('electron');
const path = require('path');
const fs = require('fs');
const crypto = require('crypto');

// Nível de compatibilidade do "casco" do app (main.js, preload.js, Telão Audio).
// Sobe quando uma página nova passa a depender de algo que só um app novo tem.
const SHELL_VERSION = 3;

const CHECK_EVERY_MS = 6 * 60 * 60 * 1000;
const READY_TIMEOUT_MS = 15000;

function cmpVer(a, b){
  const pa = String(a || '0').split('.').map(n => parseInt(n, 10) || 0);
  const pb = String(b || '0').split('.').map(n => parseInt(n, 10) || 0);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  return 0;
}
const isVersion = v => typeof v === 'string' && /^\d+\.\d+\.\d+$/.test(v);
const pageVersionOf = html => { const m = /const TELAO_VERSION = '(\d+\.\d+\.\d+)'/.exec(html); return m ? m[1] : null; };
const sha256 = buf => crypto.createHash('sha256').update(buf).digest('hex');

function manifestUrl(appDir){
  // TELAO_UPDATE_URL só para testes locais; em produção vem do update.json empacotado.
  if (process.env.TELAO_UPDATE_URL) return process.env.TELAO_UPDATE_URL;
  try{ return JSON.parse(fs.readFileSync(path.join(appDir, 'update.json'), 'utf8')).manifest || null; }catch{ return null; }
}
function allowedUrl(u){
  try{
    const x = new URL(u);
    if (x.protocol === 'https:') return true;
    return !!process.env.TELAO_UPDATE_URL && x.protocol === 'http:' && (x.hostname === '127.0.0.1' || x.hostname === 'localhost');
  }catch{ return false; }
}

async function fetchBuf(url, timeoutMs){
  if (!allowedUrl(url)) throw new Error('endereço não permitido');
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs || 15000);
  try{
    const r = await net.fetch(url, {signal: ctl.signal, cache: 'no-store', redirect: 'follow'});
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return Buffer.from(await r.arrayBuffer());
  }finally{ clearTimeout(t); }
}

class Updater {
  constructor(appDir){
    this.appDir = appDir;
    this.dir = path.join(app.getPath('userData'), 'page');
    this.metaFile = path.join(this.dir, 'meta.json');
    this.bundledPage = path.join(appDir, 'telao.html');
    this.bundledVersion = (() => { try{ return pageVersionOf(fs.readFileSync(this.bundledPage, 'utf8')); }catch{ return null; } })();
    this.url = manifestUrl(appDir);
    this.loaded = null;       // {file, version, cached}
    this.pending = null;      // última novidade encontrada (para mandar à página quando ela abrir)
    this.webContents = null;
    this.readyTimer = null;
  }
  meta(){ try{ return JSON.parse(fs.readFileSync(this.metaFile, 'utf8')) || {}; }catch{ return {}; } }
  saveMeta(m){ try{ fs.mkdirSync(this.dir, {recursive: true}); fs.writeFileSync(this.metaFile, JSON.stringify(m, null, 2)); }catch{} }

  // Qual página abrir: a baixada (se for mais nova, compatível e não marcada como ruim) ou a instalada.
  choosePage(){
    const m = this.meta(), file = path.join(this.dir, 'telao.html');
    const ok = m.version && isVersion(m.version) && m.version !== m.bad && (m.minShell || 0) <= SHELL_VERSION &&
               cmpVer(m.version, this.bundledVersion) > 0 && fs.existsSync(file);
    if (ok){
      try{
        const html = fs.readFileSync(file);
        if (sha256(html) === m.sha256 && pageVersionOf(html.toString('utf8')) === m.version)
          return (this.loaded = {file, version: m.version, cached: true});
      }catch{}
    }
    return (this.loaded = {file: this.bundledPage, version: this.bundledVersion, cached: false});
  }

  // Chamado ao carregar a janela. Se a página baixada não der sinal de vida, volta para a instalada.
  watch(win){
    this.webContents = win.webContents;
    clearTimeout(this.readyTimer);
    if (!this.loaded || !this.loaded.cached) return;
    const v = this.loaded.version;
    const fallback = reason => {
      if (!this.loaded || !this.loaded.cached || this.loaded.version !== v) return;
      const m = this.meta(); m.bad = v; m.badReason = reason; this.saveMeta(m);
      this.loaded = {file: this.bundledPage, version: this.bundledVersion, cached: false};
      try{ win.loadFile(this.bundledPage); }catch{}
    };
    this.readyTimer = setTimeout(() => fallback('não abriu em ' + READY_TIMEOUT_MS / 1000 + ' s'), READY_TIMEOUT_MS);
    win.webContents.once('render-process-gone', () => fallback('travou ao abrir'));
  }
  pageReady(version){
    clearTimeout(this.readyTimer);
    if (this.pending) this.send(this.pending);
  }
  send(msg){ try{ if (this.webContents && !this.webContents.isDestroyed()) this.webContents.send('telao:update', msg); }catch{} }

  async check(){
    if (!this.url) return {status: 'sem-endereco'};
    let man;
    try{ man = JSON.parse((await fetchBuf(this.url, 10000)).toString('utf8')); }
    catch(e){ return {status: 'erro', error: e.message}; }
    if (!man || !isVersion(man.version)) return {status: 'erro', error: 'manifesto inválido'};
    const current = this.loaded ? this.loaded.version : this.bundledVersion;
    if (cmpVer(man.version, current) <= 0) return {status: 'atualizado', version: current};
    const notes = Array.isArray(man.notes) ? man.notes.filter(x => typeof x === 'string').slice(0, 10) : [];
    const download = typeof man.download === 'string' && /^https:\/\//.test(man.download) ? man.download : null;
    if ((man.minShell || 0) > SHELL_VERSION){
      this.pending = {kind: 'app', version: man.version, notes, download};
      this.send(this.pending);
      return {status: 'precisa-app', version: man.version};
    }
    const m = this.meta();
    if (m.bad === man.version) return {status: 'ignorada', version: man.version};
    if (m.version === man.version && fs.existsSync(path.join(this.dir, 'telao.html'))){
      this.pending = {kind: 'page', version: man.version, notes};
      this.send(this.pending);
      return {status: 'pronta', version: man.version};
    }
    try{
      const pageUrl = new URL(man.page || 'telao.html', this.url).toString();
      const html = await fetchBuf(pageUrl, 30000);
      if (!man.sha256 || sha256(html) !== String(man.sha256).toLowerCase()) throw new Error('arquivo não confere (sha256)');
      if (pageVersionOf(html.toString('utf8')) !== man.version) throw new Error('versão dentro do arquivo não confere');
      fs.mkdirSync(this.dir, {recursive: true});
      const tmp = path.join(this.dir, 'telao.html.tmp');
      fs.writeFileSync(tmp, html);
      fs.renameSync(tmp, path.join(this.dir, 'telao.html'));
      this.saveMeta({version: man.version, sha256: sha256(html), minShell: man.minShell || 0, at: new Date().toISOString()});
      this.pending = {kind: 'page', version: man.version, notes};
      this.send(this.pending);
      return {status: 'baixada', version: man.version};
    }catch(e){
      return {status: 'erro', error: e.message};
    }
  }
  start(){
    const run = () => this.check().catch(() => {});
    setTimeout(run, 5000);
    setInterval(run, CHECK_EVERY_MS);
  }
}

module.exports = {Updater, SHELL_VERSION, cmpVer, pageVersionOf, sha256};
