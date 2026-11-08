// Telão Desktop — processo principal (Electron).
// A interface é a mesma página do Telão web (telao.html). Aqui ficam só as partes de desktop:
//   - seletor de janela/tela próprio (a página pede a lista e escolhe; o getDisplayMedia usa a escolha)
//   - Telão Audio embutido, iniciado escondido junto com o app
//   - sem economia de energia em segundo plano, para a transmissão não engasgar com o app minimizado
const {app, BrowserWindow, desktopCapturer, session, ipcMain, shell, Menu, screen, net, powerMonitor} = require('electron');
const path = require('path');
const fs = require('fs');
const {spawn} = require('child_process');
const {Updater, SHELL_VERSION} = require('./updater');

// Janela de teste (--telao-perfil=teste): uma segunda janela do Telão no mesmo PC, com perfil próprio (outro nome, outra
// pessoa na sala), para assistir a si mesmo. Usa o Telão Audio e a página da janela principal e não atualiza nada.
const PROFILE = ((process.argv.find(a => a.startsWith('--telao-perfil=')) || '').split('=')[1] || '').replace(/[^a-z0-9]/gi, '').slice(0, 16);
const MAIN_USERDATA = app.getPath('userData');
if (PROFILE) app.setPath('userData', MAIN_USERDATA + '-' + PROFILE);

// Configurações que precisam valer antes do Chromium iniciar (alteradas pelo painel de diagnóstico).
const CONFIG_FILE = path.join(app.getPath('userData'), 'telao-config.json');
function readConfig(){ try{ return JSON.parse(fs.readFileSync(CONFIG_FILE, 'utf8')) || {}; }catch{ return {}; } }
function writeConfig(c){ try{ fs.mkdirSync(path.dirname(CONFIG_FILE), {recursive: true}); fs.writeFileSync(CONFIG_FILE, JSON.stringify(c, null, 2)); return true; }catch{ return false; } }
const config = readConfig();
// Placa/driver na lista de bloqueio do Chromium → sem aceleração de vídeo. Opcional, ligado pelo usuário.
if (config.ignoreGpuBlocklist) app.commandLine.appendSwitch('ignore-gpu-blocklist');

// Encoder pela placa de vídeo no WebRTC:
//  - PlatformH264CbpEncoding: permite a placa também no H.264 Constrained Baseline (amigos em versões antigas pedem esse perfil)
//  - WebRtcAV1HWEncode: permite AV1 pela placa (RTX 40/50, Radeon 7000+, Intel Arc)
// Pode ser desligado no painel de diagnóstico caso cause problema.
const GPU_FEATURES = 'PlatformH264CbpEncoding,WebRtcAV1HWEncode';
// Economia de memória (experimental, Opções → Desempenho; vale ao reiniciar): os serviços de rede e de áudio rodam
// dentro do processo principal em vez de um processo cada (cada processo tem uma base fixa de memória; medido no
// Electron 44: 485 → 361 MB numa janela simples), sem o processo de reserva para abas e sem o cache de voltar página.
// O custo: se um desses serviços travar, leva o app junto (separados, o Chromium só os reinicia).
const MEM_SAVE = !!config.memSave;
const ENABLE = [...(config.disableGpuWebrtcFeatures ? [] : [GPU_FEATURES]), ...(MEM_SAVE ? ['NetworkServiceInProcess2'] : [])];
if (ENABLE.length) app.commandLine.appendSwitch('enable-features', ENABLE.join(','));
if (MEM_SAVE) app.commandLine.appendSwitch('disable-features', 'AudioServiceOutOfProcess,SpareRendererForSitePerProcess,BackForwardCache');

// Com o jogo em primeiro plano, o Telão fica atrás/minimizado: não deixar o Chromium "dormir".
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');

if (!app.requestSingleInstanceLock()){ app.quit(); }

let win = null;
let helper = null;
let pendingSource = null;   // {id, systemAudio}: escolhido na página, consumido pelo getDisplayMedia
let updater = null;

function helperPath(){
  return app.isPackaged
    ? path.join(process.resourcesPath, 'telao-audio.exe')
    : path.join(__dirname, 'telao-audio.exe');
}

// Chave do Telão Audio: aleatória, criada uma vez e guardada na pasta de dados da janela principal (a janela de teste usa
// a mesma). O Telão Audio aberto pelo app só atende motor, captura de vídeo, camada e placa de vídeo a quem mostra essa
// chave; um site no navegador nunca a conhece. Fica igual entre aberturas: se um Telão Audio de antes ainda estiver
// segurando a porta, a chave continua batendo.
let helperKeyCache = null;
function helperKey(){
  if (helperKeyCache) return helperKeyCache;
  const f = path.join(MAIN_USERDATA, 'telao-audio-chave.txt');
  try{ const k = fs.readFileSync(f, 'utf8').trim(); if (/^[0-9a-f]{64}$/.test(k)) return (helperKeyCache = k); }catch{}
  const k = require('crypto').randomBytes(32).toString('hex');
  try{ fs.mkdirSync(MAIN_USERDATA, {recursive: true}); fs.writeFileSync(f, k); }catch{}
  return (helperKeyCache = k);
}

function startHelper(){
  if (process.platform !== 'win32') return;
  const exe = helperPath();
  if (!fs.existsSync(exe)) return;
  try{
    // Se já houver um Telão Audio aberto (porta ocupada), este sai sozinho e a página usa o que já existe.
    // --boost-tree: o helper tira os processos do Telão da economia de energia do Windows 11
    // (com o app minimizado, o Windows cortava o FPS pela metade).
    const me = spawn(exe, ['--boost-tree', String(process.pid), '--key', helperKey()], {windowsHide: true, stdio: 'ignore'});
    helper = me;
    // caiu com erro: abre de novo (até 5 vezes em 10 min); a página reconecta e o motor volta sozinho
    me.on('exit', code => {
      if (helper === me) helper = null;
      if (helperQuitting || code === 0) return;
      const now = Date.now(); helperFalls = helperFalls.filter(t => now - t < 600000); helperFalls.push(now);
      if (helperFalls.length <= 5) setTimeout(() => { if (!helper && !helperQuitting) startHelper(); }, 1500);
    });
    me.on('error', () => { if (helper === me) helper = null; });
  }catch{ helper = null; }
}
let helperQuitting = false, helperFalls = [];
function stopHelper(){ helperQuitting = true; if (helper){ try{ helper.kill(); }catch{} helper = null; } }

// Ícone do Telão no Telao.exe instalado (uma vez, sem baixar o app inteiro): ao fechar, o Telão Audio espera o app sair,
// grava o ícone numa cópia do exe, confere e troca (o original fica como Telao.exe.old). Se falhar, tenta no máximo 3 vezes.
const ICON_MARK = () => path.join(app.getPath('userData'), 'exe-icone.txt');
const ICON_TRIES = () => path.join(app.getPath('userData'), 'exe-icone-tentativas.json');
function exeIconDone(){ try{ return fs.readFileSync(ICON_MARK(), 'utf8').trim().toLowerCase() === process.execPath.toLowerCase(); }catch{ return false; } }
function scheduleExeIcon(){
  if (PROFILE) return;
  if (process.platform !== 'win32' || !app.isPackaged || (updater && updater.applying) || exeIconDone()) return;
  try{
    const exe = process.execPath, hp = helperPath(), ud = app.getPath('userData');
    if (!fs.existsSync(hp) || !fs.readFileSync(hp).includes(Buffer.from('--patch-icon', 'utf16le'))) return;   // Telão Audio antigo não sabe fazer
    let t = {}; try{ t = JSON.parse(fs.readFileSync(ICON_TRIES(), 'utf8')) || {}; }catch{}
    if (t.exe === exe && t.n >= 3) return;
    const probe = path.join(path.dirname(exe), '.telao-write-test');
    fs.writeFileSync(probe, 'x'); fs.unlinkSync(probe);   // pasta sem permissão: nem tenta
    const ico = path.join(ud, 'icone.ico');
    fs.writeFileSync(ico, fs.readFileSync(path.join(__dirname, 'icon.ico')));
    fs.writeFileSync(ICON_TRIES(), JSON.stringify({exe, n: (t.exe === exe ? t.n || 0 : 0) + 1}));
    spawn(hp, ['--patch-icon', String(process.pid), exe, ico, ICON_MARK(), path.join(ud, 'icone.log')], {detached: true, stdio: 'ignore', windowsHide: true}).unref();
  }catch{}
}
// Depois que o exe com o ícone abriu bem, a cópia antiga pode sair.
function cleanExeIcon(){
  if (process.platform !== 'win32' || !app.isPackaged || !exeIconDone()) return;
  try{ fs.unlinkSync(process.execPath + '.old'); }catch{}
}

// Barra do Telão junto com a da janela (Windows): os botões de minimizar/maximizar/fechar continuam sendo os do
// próprio Windows (com o menu de encaixe do Windows 11), desenhados por cima do canto direito da barra da página.
// A página que vem com este app e as mais novas sabem disso (shell 10+); TELAO_WCO=0 desliga, TELAO_WCO=1 liga em outro sistema.
const WCO_H = 52;
// Ícone do Telão nas janelas (barra de tarefas e Alt+Tab). O do Telao.exe (atalho, Explorer) vem do pacote completo.
const APP_ICON = path.join(__dirname, process.platform === 'win32' ? 'icon.ico' : 'icon.png');
const useWco = () => process.env.TELAO_WCO === '1' || (process.platform === 'win32' && process.env.TELAO_WCO !== '0');
const SPLASH_HTML = '<!doctype html><meta charset="utf-8"><title>Telão</title><style>html,body{height:100%;margin:0;background:#0d0f12;color:#e9ebee;font:15px "Segoe UI",system-ui,sans-serif;display:grid;place-items:center}' +
  'b{font-size:28px;letter-spacing:-.01em}p{color:#8a93a2;margin:10px 0 0}i{display:block;width:180px;height:3px;margin:22px auto 0;background:#2a303a;border-radius:2px;overflow:hidden;position:relative}' +
  'i:after{content:"";position:absolute;left:-40%;width:40%;height:100%;background:#7cf29a;animation:a 1.1s linear infinite}@keyframes a{to{left:100%}}</style>' +
  '<div style="text-align:center"><b>telão</b><p>Procurando atualização…</p><i></i></div>';
function createWindow(){
  const wco = useWco();
  win = new BrowserWindow({
    width: 1280, height: 800, minWidth: 720, minHeight: 480,
    backgroundColor: '#0d0f12',
    title: PROFILE ? 'Telão (janela de teste)' : 'Telão',
    icon: APP_ICON,
    autoHideMenuBar: true,
    ...(wco ? {titleBarStyle: 'hidden', titleBarOverlay: {color: '#12151a', symbolColor: '#8a93a2', height: WCO_H}} : {}),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      spellcheck: !MEM_SAVE,
      additionalArguments: ['--telao-app-version=' + app.getVersion(), '--telao-shell=' + SHELL_VERSION, '--telao-main-pid=' + process.pid, '--telao-helper-key=' + helperKey(),
                            '--telao-wco=' + (wco ? WCO_H : 0), ...(PROFILE ? ['--telao-perfil=' + PROFILE, '--telao-sala=' + ((process.argv.find(a => a.startsWith('--telao-sala=')) || '').split('=')[1] || '').replace(/[^a-z0-9-]/gi, '').slice(0, 40),
                                           '--telao-modo=' + ((process.argv.find(a => a.startsWith('--telao-modo=')) || '').split('=')[1] || '').replace(/[^a-z]/gi, '').slice(0, 8)] : [])],
    },
  });
  Menu.setApplicationMenu(null);
  // Página: a baixada pela atualização automática (se mais nova e saudável) ou a instalada.
  // Antes, procura atualização (até 25 s, com uma tela de espera): todo mundo entra na versão mais nova.
  const openPage = () => { if (!win || win.isDestroyed()) return; const page = updater.choosePage(); win.loadFile(page.file); if (!PROFILE) updater.watch(win); };
  if (PROFILE || (!app.isPackaged && !process.env.TELAO_TESTE_INICIO) || process.argv.includes('--sem-atualizar')) openPage();
  else {
    win.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(SPLASH_HTML)).catch(() => {});   // (a página real substitui no meio: ERR_ABORTED é esperado)
    updater.startup().then(r => { if (r !== 'reiniciando') openPage(); }, openPage);
  }

  // Links externos abrem no navegador; nada de janelas novas dentro do app.
  win.webContents.setWindowOpenHandler(({url, frameName}) => {
    // Janela flutuante do Telão: popup em branco da própria página, sempre por cima.
    if ((url === 'about:blank' || url === '') && /^telao-float-/.test(frameName || '')){
      return {action: 'allow', overrideBrowserWindowOptions: {
        width: 520, height: 300, minWidth: 240, minHeight: 140, alwaysOnTop: true, autoHideMenuBar: true,
        backgroundColor: '#000000', title: 'Telão', icon: APP_ICON, fullscreenable: true, minimizable: true,
        webPreferences: {contextIsolation: true, nodeIntegration: false, backgroundThrottling: false},
      }};
    }
    if (/^https?:\/\//i.test(url)) shell.openExternal(url);
    return {action: 'deny'};
  });
  win.webContents.on('did-create-window', child => {
    child.setAlwaysOnTop(true, 'floating');
    child.setMenu(null);
  });
  win.webContents.on('will-navigate', (e, url) => {
    if (!url.startsWith('file://')){ e.preventDefault(); if (/^https?:\/\//i.test(url)) shell.openExternal(url); }
  });
  // F12 abre as ferramentas de desenvolvedor (útil para diagnosticar problemas).
  win.webContents.on('before-input-event', (e, input) => {
    if (input.type === 'keyDown' && input.key === 'F12'){ win.webContents.toggleDevTools(); e.preventDefault(); }
  });
  win.on('closed', () => { win = null; });
}

app.on('second-instance', () => {
  if (win){ if (win.isMinimized()) win.restore(); win.focus(); }
});

app.whenReady().then(() => {
  const ses = session.defaultSession;
  const allowed = new Set(['media', 'display-capture', 'clipboard-sanitized-write', 'clipboard-read', 'fullscreen']);
  ses.setPermissionRequestHandler((_wc, perm, cb) => cb(allowed.has(perm)));
  ses.setPermissionCheckHandler((_wc, perm) => allowed.has(perm));

  // getDisplayMedia na página → usa a fonte escolhida no seletor do próprio Telão.
  ses.setDisplayMediaRequestHandler(async (request, callback) => {
    const chosen = pendingSource;
    pendingSource = null;
    try{
      if (!chosen) return callback(null);
      // pelo id exato; senão, a mesma janela com outro sufixo (o id de getMediaSourceId, usado pela cena de teste, pode vir assim)
      const same = id => id.startsWith('window:') && chosen.id.startsWith('window:') && id.split(':')[1] === chosen.id.split(':')[1];
      const find = async () => { const l = await desktopCapturer.getSources({types: ['window', 'screen']}); return l.find(s => s.id === chosen.id) || l.find(s => same(s.id)); };
      let src = await find();
      for (let i = 0; !src && i < 3; i++){ await new Promise(r => setTimeout(r, 400)); src = await find(); }   // a lista às vezes vem vazia logo depois de outra consulta
      if (!src) return callback(null);
      const res = {video: src};
      if (request.audioRequested && chosen.systemAudio) res.audio = 'loopback';
      callback(res);
    }catch{ callback(null); }
  });

  ipcMain.handle('telao:sources', async () => {
    const list = await desktopCapturer.getSources({
      types: ['window', 'screen'],
      thumbnailSize: {width: 320, height: 180},
      fetchWindowIcons: true,
    });
    return list
      .filter(s => s.name && s.name !== 'Telão')
      .map(s => ({
        id: s.id,
        name: s.name,
        type: s.id.startsWith('screen') ? 'screen' : 'window',
        thumb: s.thumbnail && !s.thumbnail.isEmpty() ? s.thumbnail.toDataURL() : '',
        icon: s.appIcon && !s.appIcon.isEmpty() ? s.appIcon.toDataURL() : '',
      }));
  });
  // Medição de uso real (Diagnóstico): processador e memória de cada parte do Telão e do PC inteiro.
  const os = require('os');
  let cpuPrev = null;
  const cpuTimes = () => os.cpus().reduce((a, c) => { const t = c.times; a.busy += t.user + t.nice + t.sys + t.irq; a.all += t.user + t.nice + t.sys + t.irq + t.idle; return a; }, {busy: 0, all: 0});
  ipcMain.handle('telao:metrics', () => {
    const labels = new Map();
    for (const w of BrowserWindow.getAllWindows()){
      if (w.isDestroyed()) continue;
      let pid = 0; try{ pid = w.webContents.getOSProcessId(); }catch{}
      const label = w === win ? 'janela principal do Telão' : /^about:blank/.test(w.webContents.getURL()) ? 'janela flutuante' : 'outra janela do Telão';
      if (pid && !labels.has(pid)) labels.set(pid, label);
    }
    const names = {Browser: 'processo principal', GPU: 'vídeo e composição (placa)', Utility: 'serviços (rede, áudio)', Zygote: 'auxiliar', Sandbox_helper: 'auxiliar'};
    const procs = app.getAppMetrics().map(m => ({label: labels.get(m.pid) || (m.type === 'Utility' && m.serviceName ? 'serviço: ' + String(m.serviceName).replace(/^.*\./, '') : names[m.type] || m.type),
      cpu: Math.round(m.cpu.percentCPUUsage * 10) / 10, mem: Math.round(m.memory.workingSetSize / 1024),
      priv: m.memory.privateBytes != null ? Math.round(m.memory.privateBytes / 1024) : null}));
    const t = cpuTimes();
    let sysCpu = null;
    if (cpuPrev && t.all > cpuPrev.all) sysCpu = Math.round((t.busy - cpuPrev.busy) / (t.all - cpuPrev.all) * 1000) / 10;
    cpuPrev = t;
    return {procs, sysCpu, cores: os.cpus().length, memTotal: os.totalmem(), memFree: os.freemem()};
  });
  // Ausência: há quanto tempo ninguém mexe no PC (mouse/teclado em qualquer programa), e tela bloqueada / PC dormindo.
  ipcMain.handle('telao:idle', () => { try{ return {idle: powerMonitor.getSystemIdleTime(), locked: powerMonitor.getSystemIdleState(60) === 'locked'}; }catch{ return null; } });
  for (const ev of ['lock-screen', 'unlock-screen', 'suspend', 'resume']){
    powerMonitor.on(ev, () => { if (win && !win.isDestroyed()) win.webContents.send('telao:power', ev); });
  }
  // ---- jukebox (só no app): bloqueio de anúncio no player ----
  const jbConf = {adblock: (() => { try{ return JSON.parse(fs.readFileSync(path.join(MAIN_USERDATA, 'jukebox.json'), 'utf8')).adblock !== false; }catch{ return true; } })()};
  ipcMain.handle('telao:ytStatus', e => (win && e.sender === win.webContents) ? {adblock: !!jbConf.adblock} : null);
  // Bloqueio de anúncio no player: corta os pedidos de anúncio e de medição de anúncio.
  // Funciona como os bloqueadores de navegador; o YouTube às vezes muda e o anúncio volta até a lista ser ajustada.
  const AD_URLS = ['*://*.doubleclick.net/*', '*://*.googlesyndication.com/*', '*://*.googleadservices.com/*', '*://*.google.com/pagead/*',
    '*://*.youtube.com/pagead/*', '*://*.youtube.com/api/stats/ads*', '*://*.youtube.com/get_midroll_info*', '*://*.youtube.com/ptracking*',
    '*://*.youtube.com/youtubei/v1/log_event*', '*://*.youtube-nocookie.com/pagead/*'];
  ses.webRequest.onBeforeRequest({urls: AD_URLS}, (d, cb) => {
    // só o que o player da jukebox pede (nada do resto do app passa por esses endereços)
    cb({cancel: !!jbConf.adblock && d.resourceType !== 'mainFrame'});
  });
  ipcMain.handle('telao:ytAdblock', (e, on) => {
    if (!win || e.sender !== win.webContents) return null;
    jbConf.adblock = !!on;
    try{ fs.mkdirSync(MAIN_USERDATA, {recursive: true}); fs.writeFileSync(path.join(MAIN_USERDATA, 'jukebox.json'), JSON.stringify({adblock: jbConf.adblock})); }catch{}
    return {adblock: jbConf.adblock};
  });

  // Fundo da entrada: procura um jogo pelo nome na loja da Steam (no navegador a Steam bloqueia essa consulta).
  ipcMain.handle('telao:steamSearch', async (_e, term) => {
    term = String(term || '').replace(/\s+/g, ' ').trim().slice(0, 80);
    if (term.length < 2) return [];
    const base = process.env.TELAO_STEAM_SEARCH_URL || 'https://store.steampowered.com/api/storesearch/';
    try{
      const ctl = new AbortController(); const t = setTimeout(() => ctl.abort(), 8000);
      const r = await net.fetch(base + '?term=' + encodeURIComponent(term) + '&l=english&cc=BR', {signal: ctl.signal});
      clearTimeout(t);
      if (!r.ok) return [];
      const j = await r.json();
      return (Array.isArray(j.items) ? j.items : []).filter(x => x && Number.isInteger(x.id) && (!x.type || x.type === 'app'))
        .slice(0, 8).map(x => ({id: x.id, name: String(x.name || '').slice(0, 80)}));
    }catch{ return []; }
  });
  // Modo rápido: acha a "tela inteira" do monitor onde está o jogo (retângulo em pixels físicos, vindo do Telão Audio).
  ipcMain.handle('telao:screenFor', async (_e, m) => {
    if (!m) return null;
    const near = (a, b) => Math.abs(a - b) <= 2;
    const phys = d => process.platform === 'win32' && screen.dipToScreenRect ? screen.dipToScreenRect(null, d.bounds) : d.bounds;
    const disp = screen.getAllDisplays().find(d => { const b = phys(d); return near(b.x, m.x) && near(b.y, m.y) && near(b.width, m.w) && near(b.height, m.h); });
    let srcs = await desktopCapturer.getSources({types: ['screen'], thumbnailSize: {width: 0, height: 0}});
    if (!srcs.length){ await new Promise(r => setTimeout(r, 400)); srcs = await desktopCapturer.getSources({types: ['screen'], thumbnailSize: {width: 0, height: 0}}); }
    const s = (disp && srcs.find(x => String(x.display_id) === String(disp.id))) || (srcs.length === 1 ? srcs[0] : null);
    return s ? s.id : null;
  });
  ipcMain.handle('telao:select', (_e, id, systemAudio) => {
    pendingSource = {id: String(id), systemAudio: !!systemAudio};
    return true;
  });

  // Monitor escolhido no seletor ("screen:...") → retângulo em pixels físicos (o Telão Audio acha o monitor por ele).
  ipcMain.handle('telao:monitorRect', async (_e, sourceId) => {
    try{
      const srcs = await desktopCapturer.getSources({types: ['screen'], thumbnailSize: {width: 0, height: 0}});
      const s = srcs.find(x => x.id === String(sourceId));
      const d = (s && screen.getAllDisplays().find(x => String(x.id) === String(s.display_id))) || (srcs.length === 1 ? screen.getPrimaryDisplay() : null);
      if (!d) return null;
      const b = process.platform === 'win32' && screen.dipToScreenRect ? screen.dipToScreenRect(null, d.bounds) : d.bounds;
      return {x: b.x, y: b.y, w: b.width, h: b.height};
    }catch{ return null; }
  });

  // ---- clipes: a página manda o vídeo pronto e o app grava em Vídeos\Telão ----
  const clipsDir = () => path.join(app.getPath('videos'), 'Telão');
  ipcMain.handle('telao:saveClip', async (_e, name, data) => {
    try{
      let base = path.basename(String(name || 'clipe.webm')).replace(/[\\/:*?"<>|]+/g, '').slice(0, 150);
      if (!/\.webm$/i.test(base)) base += '.webm';
      if (!(data instanceof ArrayBuffer) && !ArrayBuffer.isView(data)) return {ok: false, error: 'dados inválidos'};
      const buf = Buffer.from(data instanceof ArrayBuffer ? data : data.buffer);
      if (buf.length > 300 * 1024 * 1024) return {ok: false, error: 'clipe grande demais'};
      fs.mkdirSync(clipsDir(), {recursive: true});
      let target = path.join(clipsDir(), base);
      for (let k = 2; fs.existsSync(target); k++) target = path.join(clipsDir(), base.replace(/\.webm$/i, ' (' + k + ').webm'));
      await fs.promises.writeFile(target, buf);
      return {ok: true, name: path.basename(target)};
    }catch(e){ return {ok: false, error: e.code || e.message}; }
  });
  // Motor de vídeo (nível 14): o Telão Audio desenha a tela de quem assiste numa janela "filha" desta (ele precisa do identificador)
  ipcMain.handle('telao:hwnd', e => {
    const w = BrowserWindow.fromWebContents(e.sender); if (!w) return '';
    try{ const b = w.getNativeWindowHandle(); return b.length >= 8 ? b.readBigUInt64LE(0).toString() : b.readUInt32LE(0).toString(); }catch{ return ''; }
  });
  // Abre a janela de teste (outro processo do Telão com perfil próprio). Se já estiver aberta, ela só vem para a frente.
  ipcMain.handle('telao:openTest', (_e, room, mode) => {
    if (PROFILE) return false;
    const r = String(room || '').replace(/[^a-z0-9-]/gi, '').slice(0, 40), m = String(mode || '').replace(/[^a-z]/gi, '').slice(0, 8);
    try{ spawn(process.execPath, [...(app.isPackaged ? [] : [app.getAppPath()]), '--telao-perfil=teste', ...(r ? ['--telao-sala=' + r] : []), ...(m ? ['--telao-modo=' + m] : [])], {detached: true, stdio: 'ignore'}).unref(); return true; }catch{ return false; }
  });
  // Teste padrão: janela sem moldura com a cena de teste (o HTML vem da própria página), para transmitir sempre a mesma coisa.
  let sceneWin = null;
  const closeScene = () => { if (sceneWin && !sceneWin.isDestroyed()) sceneWin.destroy(); sceneWin = null; };
  ipcMain.handle('telao:openScene', async (e, html, w, h) => {
    if (!win || e.sender !== win.webContents || PROFILE || typeof html !== 'string' || html.length > 300000) return null;
    closeScene();
    const d = screen.getPrimaryDisplay(), sf = d.scaleFactor || 1, wa = d.workAreaSize;
    let W = Math.max(320, Math.min(3840, Math.round(+w) || 1280)), H = Math.max(180, Math.min(2160, Math.round(+h) || 720));
    while ((W / sf > wa.width || H / sf > wa.height) && W > 640){ W = Math.round(W * 2 / 3 / 2) * 2; H = Math.round(H * 2 / 3 / 2) * 2; }   // não cabe: um tamanho menor na mesma proporção
    const file = path.join(app.getPath('userData'), 'cena-teste.html');
    try{ fs.writeFileSync(file, html.replace('__W__', String(W)).replace('__H__', String(H))); }catch{ return null; }
    sceneWin = new BrowserWindow({useContentSize: true, width: Math.round(W / sf), height: Math.round(H / sf), frame: false, resizable: false, show: true,
      title: 'Telão — cena de teste', backgroundColor: '#000000', webPreferences: {backgroundThrottling: false, contextIsolation: true, nodeIntegration: false, spellcheck: false}});
    sceneWin.on('closed', () => { sceneWin = null; });
    await sceneWin.loadFile(file);
    return {id: sceneWin.getMediaSourceId(), w: W, h: H};
  });
  ipcMain.handle('telao:closeScene', e => { if (win && e.sender === win.webContents) closeScene(); return true; });
  // Duplo clique na janela de vídeo do Telão Audio: tela cheia exige "gesto do usuário", que só o app consegue dar à página.
  ipcMain.handle('telao:natGesture', e => {
    if (!win || e.sender !== win.webContents) return false;
    return e.sender.executeJavaScript('window.__natGesture && window.__natGesture(), true', true).catch(() => false);
  });
  ipcMain.handle('telao:openClips', () => { try{ fs.mkdirSync(clipsDir(), {recursive: true}); }catch{} return shell.openPath(clipsDir()); });

  // ---- diagnóstico de vídeo ----
  // Cores dos botões do Windows na barra integrada (acompanham o tema escolhido na página).
  const HEX = /^#[0-9a-f]{6}$/i;
  ipcMain.handle('telao:titleBar', (e, c) => {
    if (!win || e.sender !== win.webContents || !useWco() || !c) return false;
    const color = String(c.color || '').trim(), symbolColor = String(c.symbolColor || '').trim();
    if (!HEX.test(color) || !HEX.test(symbolColor)) return false;
    try{ win.setTitleBarOverlay({color, symbolColor, height: WCO_H}); return true; }catch{ return false; }
  });
  ipcMain.handle('telao:gpu', async () => {
    let info = null;
    try{ info = await app.getGPUInfo('complete'); }
    catch{ try{ info = await app.getGPUInfo('basic'); }catch{} }
    const devices = (info && info.gpuDevice) || [];
    return {
      feature: app.getGPUFeatureStatus(),
      devices: devices.map(d => ({
        vendorId: d.vendorId, deviceId: d.deviceId, active: !!d.active,
        name: d.deviceString || '', driver: d.driverVersion || '', vendor: d.driverVendor || '',
      })),
      config: readConfig(),
      features: (app.commandLine.getSwitchValue('enable-features') || '(nenhuma)') + (app.commandLine.hasSwitch('disable-features') ? ' · sem: ' + app.commandLine.getSwitchValue('disable-features') : ''),
      memSave: MEM_SAVE,
      versions: {app: app.getVersion(), shell: SHELL_VERSION, page: updater && updater.loaded ? updater.loaded.version + (updater.loaded.cached ? ' (atualizada)' : ' (instalada)') : '?',
                 electron: process.versions.electron, chrome: process.versions.chrome},
      platform: process.platform + ' ' + require('os').release(),
    };
  });
  ipcMain.handle('telao:setConfig', (_e, key, value) => {
    if (key !== 'ignoreGpuBlocklist' && key !== 'disableGpuWebrtcFeatures' && key !== 'memSave') return false;
    const c = readConfig(); c[key] = !!value;
    return writeConfig(c);
  });
  ipcMain.handle('telao:restart', () => { app.relaunch(); app.exit(0); });
  ipcMain.handle('telao:gpuPage', () => {
    const w = new BrowserWindow({width: 1000, height: 800, title: 'Detalhes da placa de vídeo', icon: APP_ICON, autoHideMenuBar: true});
    w.loadURL('chrome://gpu');
    return true;
  });

  // ---- atualização automática ----
  ipcMain.handle('telao:ready', (_e, v) => { updater.pageReady(v); if (!PROFILE){ updater.markAppOk(); cleanExeIcon(); } return {shell: SHELL_VERSION, cached: !!(updater.loaded && updater.loaded.cached)}; });
  ipcMain.handle('telao:applyApp', () => PROFILE ? false : updater.applyApp());
  ipcMain.handle('telao:checkUpdate', () => PROFILE ? {status: 'janela de teste'} : updater.check());
  ipcMain.handle('telao:applyUpdate', () => {
    if (!win || PROFILE) return false;
    const page = updater.choosePage();
    win.loadFile(page.file);
    updater.watch(win);
    return page.version;
  });

  updater = new Updater(__dirname);
  if (PROFILE){ updater.dir = path.join(MAIN_USERDATA, 'page'); updater.metaFile = path.join(updater.dir, 'meta.json'); }   // mesma página da janela principal
  startHelper();
  createWindow();
  if (!PROFILE) updater.start();
});

app.on('before-quit', () => { stopHelper(); });
app.on('will-quit', scheduleExeIcon);
app.on('window-all-closed', () => { stopHelper(); app.quit(); });
// a camada não é "janela de verdade": fechar a janela principal fecha o app mesmo com ela aberta
app.on('browser-window-created', (_e, w) => {
  w.on('closed', () => {
    if (w !== win) return;
    for (const o of BrowserWindow.getAllWindows()) if (!o.isDestroyed()) o.destroy();   // camada e janelas flutuantes
  });
});
