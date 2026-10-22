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
const os = require('os');
const {spawn} = require('child_process');
const path = require('path');
const fs = require('fs');
const crypto = require('crypto');

// Nível de compatibilidade do "casco" do app (main.js, preload.js, Telão Audio).
// Sobe quando uma página nova passa a depender de algo que só um app novo tem.
const SHELL_VERSION = 12;
// Arquivos do app que podem ser trocados pela atualização leve (dentro de resources/). O resto é o Electron.
const APP_FILES = ['app.asar', 'telao-audio.exe'];

const CHECK_EVERY_MS = 6 * 60 * 60 * 1000;
const APP_OK_TIMEOUT_S = 25;
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
    const notes = Array.isArray(man.notes) ? man.notes.filter(x => typeof x === 'string').slice(0, 10) : [];
    const download = typeof man.download === 'string' && /^https:\/\//.test(man.download) ? man.download : null;
    // 1) app novo? (pacote leve: só o código do app e o Telão Audio)
    const appRes = await this.checkApp(man, notes, download);
    if (appRes && appRes.status !== 'erro') return appRes;
    // 2) interface nova?
    const current = this.loaded ? this.loaded.version : this.bundledVersion;
    if (cmpVer(man.version, current) <= 0) return appRes || {status: 'atualizado', version: current};
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
  // ---------- atualização do app ----------
  appBad(){ try{ return JSON.parse(fs.readFileSync(path.join(app.getPath('userData'), 'app-bad.json'), 'utf8')).version; }catch{ return null; } }
  canWrite(dir){
    try{ const t = path.join(dir, '.telao-write-test'); fs.writeFileSync(t, 'x'); fs.unlinkSync(t); return true; }catch{ return false; }
  }
  async checkApp(man, notes, download){
    const a = man.app;
    if (!a || !isVersion(a.version) || cmpVer(a.version, app.getVersion()) <= 0) return null;
    if (this.appBad() === a.version) return {status: 'app-ignorada', version: a.version};
    if (!app.isPackaged) return null;
    const res = process.resourcesPath;
    const files = a.electron === process.versions.electron && a.files && typeof a.files === 'object'
      ? Object.keys(a.files).filter(n => APP_FILES.includes(n)) : [];
    if (!files.length || !this.canWrite(res)){
      // runtime do Electron mudou (ou pasta sem permissão): precisa do zip completo
      this.pending = {kind: 'app', version: a.version, notes, download};
      this.send(this.pending);
      return {status: 'precisa-app', version: a.version, reason: files.length ? 'pasta sem permissão de escrita' : 'Electron novo'};
    }
    const staging = path.join(app.getPath('userData'), 'app-update', a.version);
    try{
      fs.mkdirSync(staging, {recursive: true});
      for (const name of files){
        const f = a.files[name], target = path.join(staging, name);
        if (!f || !/^[0-9a-f]{64}$/i.test(f.sha256 || '')) throw new Error('manifesto sem sha256 para ' + name);
        if (fs.existsSync(target) && sha256(fs.readFileSync(target)) === f.sha256.toLowerCase()) continue;   // já baixado
        const buf = await fetchBuf(new URL(f.path, this.url).toString(), 120000);
        if (f.size && buf.length !== f.size) throw new Error(name + ': tamanho não confere');
        if (sha256(buf) !== f.sha256.toLowerCase()) throw new Error(name + ': arquivo não confere (sha256)');
        fs.writeFileSync(target + '.tmp', buf); fs.renameSync(target + '.tmp', target);
      }
    }catch(e){ return {status: 'erro', error: e.message}; }
    this.appStaged = {version: a.version, staging, files};
    this.pending = {kind: 'app-ready', version: a.version, notes};
    this.send(this.pending);
    return {status: 'app-baixado', version: a.version};
  }
  // Fecha o app; um processo separado (o próprio executável em modo Node) troca os arquivos e reabre.
  applyApp(){
    const st = this.appStaged;
    if (!st) return false;
    const ud = app.getPath('userData');
    const args = {
      pid: process.pid, exe: process.execPath, argv: process.argv.slice(1), version: st.version,
      staging: st.staging, resources: process.resourcesPath, files: st.files,
      okFile: path.join(ud, 'app-ok-' + st.version), badFile: path.join(ud, 'app-bad.json'), logFile: path.join(ud, 'app-update.log'),
      timeout: APP_OK_TIMEOUT_S,
    };
    try{ fs.unlinkSync(args.okFile); }catch{}
    const script = path.join(os.tmpdir(), 'telao-troca-' + Date.now() + '.js');
    fs.writeFileSync(script, SWAPPER);
    const env = Object.assign({}, process.env, {ELECTRON_RUN_AS_NODE: '1', ELECTRON_NO_ASAR: '1'});
    const child = spawn(process.execPath, [script, JSON.stringify(args)], {detached: true, stdio: 'ignore', env, windowsHide: true});
    child.unref();
    this.applying = true;   // a troca do app está esperando: nada mais mexe na pasta ao fechar
    setTimeout(() => app.quit(), 300);
    return true;
  }
  // O app novo confirma que abriu (chamado quando a página avisa que carregou).
  markAppOk(){
    const ud = app.getPath('userData');
    try{ fs.writeFileSync(path.join(ud, 'app-ok-' + app.getVersion()), new Date().toISOString()); }catch{}
    // limpa downloads de versões já instaladas
    try{
      for (const v of fs.readdirSync(path.join(ud, 'app-update')))
        if (isVersion(v) && cmpVer(v, app.getVersion()) <= 0) fs.rmSync(path.join(ud, 'app-update', v), {recursive: true, force: true});
    }catch{}
  }

  start(){
    const run = () => this.check().catch(() => {});
    setTimeout(run, 5000);
    setInterval(run, CHECK_EVERY_MS);
  }
}

// Processo da troca: espera o app fechar, guarda cópia dos arquivos atuais (.old), copia os novos,
// reabre o app e espera ele confirmar. Sem confirmação em N s: desfaz, marca a versão como ruim e reabre o antigo.
const SWAPPER = `'use strict';
process.noAsar = true;   // no modo Node do Electron, .asar vira "pasta": aqui precisamos copiar como arquivo comum
const fs = require('fs'), path = require('path'), {spawn} = require('child_process');
const a = JSON.parse(process.argv[2]);
const log = m => { try{ fs.appendFileSync(a.logFile, new Date().toISOString() + ' ' + m + '\\n'); }catch(e){} };
const alive = pid => { try{ process.kill(pid, 0); return true; }catch(e){ return e.code === 'EPERM'; } };
const sleep = ms => new Promise(r => setTimeout(r, ms));
function relaunch(){
  const env = Object.assign({}, process.env); delete env.ELECTRON_RUN_AS_NODE; delete env.ELECTRON_NO_ASAR;
  const c = spawn(a.exe, a.argv, {detached: true, stdio: 'ignore', env}); c.unref(); return c;
}
async function copyRetry(src, dst){
  for (let k = 0; k < 30; k++){ try{ fs.copyFileSync(src, dst); return; }catch(e){ await sleep(500); } }
  throw new Error('não consegui gravar ' + path.basename(dst));
}
async function restore(done){
  for (const f of done){ try{ await copyRetry(path.join(a.resources, f) + '.old', path.join(a.resources, f)); }catch(e){ log('falha ao restaurar ' + f); } }
}
(async () => {
  log('troca para ' + a.version + ': esperando o app fechar');
  for (let i = 0; i < 120 && alive(a.pid); i++) await sleep(250);
  if (alive(a.pid)){ log('o app não fechou; nada foi trocado'); return; }
  await sleep(700);
  const done = [];
  try{
    for (const f of a.files){
      const dst = path.join(a.resources, f);
      if (fs.existsSync(dst)) await copyRetry(dst, dst + '.old');
      await copyRetry(path.join(a.staging, f), dst);
      done.push(f);
    }
  }catch(e){
    log('erro: ' + e.message + '; desfazendo'); await restore(done); relaunch(); return;
  }
  try{ fs.unlinkSync(a.okFile); }catch(e){}
  log('arquivos trocados; abrindo ' + a.version);
  const child = relaunch();
  for (let i = 0; i < a.timeout * 4; i++){
    if (fs.existsSync(a.okFile)){ log('ok: ' + a.version + ' abriu'); return; }
    await sleep(250);
  }
  log(a.version + ' não confirmou em ' + a.timeout + ' s; voltando para a versão anterior');
  try{ process.kill(child.pid); }catch(e){}
  for (let i = 0; i < 40 && alive(child.pid); i++) await sleep(250);
  await sleep(500);
  await restore(done);
  try{ fs.writeFileSync(a.badFile, JSON.stringify({version: a.version, at: new Date().toISOString()})); }catch(e){}
  relaunch();
  log('versão anterior reaberta');
})().catch(e => log('erro inesperado: ' + e.message));
`;

module.exports = {Updater, SHELL_VERSION, APP_FILES, cmpVer, pageVersionOf, sha256, SWAPPER};
