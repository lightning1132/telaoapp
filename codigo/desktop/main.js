// Telão Desktop — processo principal (Electron).
// A interface é a mesma página do Telão web (telao.html). Aqui ficam só as partes de desktop:
//   - seletor de janela/tela próprio (a página pede a lista e escolhe; o getDisplayMedia usa a escolha)
//   - Telão Audio embutido, iniciado escondido junto com o app
//   - sem economia de energia em segundo plano, para a transmissão não engasgar com o app minimizado
const {app, BrowserWindow, desktopCapturer, session, ipcMain, shell, Menu, screen, net, powerMonitor} = require('electron');
const sharedTexture = (() => { try{ return require('electron').sharedTexture || null; }catch{ return null; } })();
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
let overlay = null;          // camada transparente das reações, por cima do jogo

function helperPath(){
  return app.isPackaged
    ? path.join(process.resourcesPath, 'telao-audio.exe')
    : path.join(__dirname, 'telao-audio.exe');
}

function startHelper(){
  if (process.platform !== 'win32') return;
  const exe = helperPath();
  if (!fs.existsSync(exe)) return;
  try{
    // Se já houver um Telão Audio aberto (porta ocupada), este sai sozinho e a página usa o que já existe.
    // --boost-tree: o helper tira os processos do Telão da economia de energia do Windows 11
    // (com o app minimizado, o Windows cortava o FPS pela metade).
    helper = spawn(exe, ['--boost-tree', String(process.pid)], {windowsHide: true, stdio: 'ignore'});
    helper.on('exit', () => { helper = null; });
    helper.on('error', () => { helper = null; });
  }catch{ helper = null; }
}
function stopHelper(){ if (helper){ try{ helper.kill(); }catch{} helper = null; } }

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
      additionalArguments: ['--telao-app-version=' + app.getVersion(), '--telao-shell=' + SHELL_VERSION, '--telao-main-pid=' + process.pid,
                            '--telao-wco=' + (wco ? WCO_H : 0), ...(PROFILE ? ['--telao-perfil=' + PROFILE, '--telao-sala=' + ((process.argv.find(a => a.startsWith('--telao-sala=')) || '').split('=')[1] || '').replace(/[^a-z0-9-]/gi, '').slice(0, 40)] : [])],
    },
  });
  Menu.setApplicationMenu(null);
  // Página: a baixada pela atualização automática (se mais nova e saudável) ou a instalada.
  const page = updater.choosePage();
  win.loadFile(page.file);
  if (!PROFILE) updater.watch(win);

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
      const find = async () => (await desktopCapturer.getSources({types: ['window', 'screen']})).find(s => s.id === chosen.id);
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
      const label = w === win ? 'janela principal do Telão' : w === overlay ? 'camada por cima do jogo' : /^about:blank/.test(w.webContents.getURL()) ? 'janela flutuante' : 'outra janela do Telão';
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

  // ---- captura turbo: imagens que o Telão Audio deixa na placa de vídeo ----
  // O Telão Audio cria as texturas e já entrega os handles valendo neste processo; aqui elas viram
  // "texturas importadas" do Chromium e seguem para a página, que monta os quadros sem copiar nada.
  const turboTex = new Map();   // geração → [texturas importadas]
  const turboRelease = gen => { const l = turboTex.get(gen); if (!l) return; turboTex.delete(gen); for (const t of l) try{ t.release(); }catch{} };
  ipcMain.handle('telao:turboImport', async (e, info) => {
    if (!sharedTexture) return {ok: false, error: 'esta versão do app não tem textura compartilhada'};
    if (!win || win.isDestroyed() || e.sender !== win.webContents) return {ok: false, error: 'janela inválida'};
    const i = info || {}, gen = i.gen, w = i.w, h = i.h;
    if (!Number.isInteger(gen) || gen < 1 || !Number.isInteger(w) || !Number.isInteger(h) || w < 16 || h < 16 || w > 4096 || h > 4096 ||
        (i.fmt !== 'nv12' && i.fmt !== 'bgra') || !Array.isArray(i.slots) || !i.slots.length || i.slots.length > 8 || !i.slots.every(x => /^\d{1,20}$/.test(String(x))))
      return {ok: false, error: 'pedido inválido'};
    turboRelease(gen);
    const list = [];
    const colorSpace = i.fmt === 'nv12' ? {primaries: 'bt709', transfer: 'bt709', matrix: 'bt709', range: 'limited'}
                                        : {primaries: 'bt709', transfer: 'srgb', matrix: 'rgb', range: 'full'};
    try{
      for (let k = 0; k < i.slots.length; k++){
        const handle = Buffer.alloc(8); handle.writeBigUInt64LE(BigInt(i.slots[k]));
        const imp = sharedTexture.importSharedTexture({textureInfo: {pixelFormat: i.fmt, codedSize: {width: w, height: h}, colorSpace, handle: {ntHandle: handle}}});
        list.push(imp);
        await sharedTexture.sendSharedTexture({frame: win.webContents.mainFrame, importedSharedTexture: imp}, gen, k);
      }
    }catch(err){
      for (const t of list) try{ t.release(); }catch{}
      return {ok: false, error: String(err && err.message || err).slice(0, 200)};
    }
    turboTex.set(gen, list);
    return {ok: true};
  });
  ipcMain.handle('telao:turboRelease', (_e, gen) => { turboRelease(gen); return true; });
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
  ipcMain.handle('telao:openTest', (_e, room) => {
    if (PROFILE) return false;
    const r = String(room || '').replace(/[^a-z0-9-]/gi, '').slice(0, 40);
    try{ spawn(process.execPath, [...(app.isPackaged ? [] : [app.getAppPath()]), '--telao-perfil=teste', ...(r ? ['--telao-sala=' + r] : [])], {detached: true, stdio: 'ignore'}).unref(); return true; }catch{ return false; }
  });
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

  // ---- camada de reações sobre o jogo ----
  // Transparente, sempre por cima, não recebe clique nem foco e fica fora de capturas de tela
  // (quem compartilha a tela inteira não reenvia as reações na transmissão).
  const overlayDisplay = (sourceId, rect) => {
    const all = screen.getAllDisplays();
    const m = /^screen:(\d+):/.exec(String(sourceId || ''));
    if (m){ const d = all.find(x => String(x.id) === m[1]); if (d) return d; }
    if (rect && rect.mw > 0){
      const dip = process.platform === 'win32' ? screen.screenToDipRect(null, {x: rect.mx, y: rect.my, width: rect.mw, height: rect.mh})
                                               : {x: rect.mx, y: rect.my, width: rect.mw, height: rect.mh};
      return screen.getDisplayMatching(dip);
    }
    return screen.getPrimaryDisplay();
  };
  // A camada é uma faixa no lado direito da área do jogo e só fica visível enquanto há reações
  // animando (some ~3,5 s depois da última). Assim, na maior parte do tempo não há nada por cima do jogo.
  let overlayActive = false, overlayHideTimer = null, overlayBounds = null;
  const toDip = r => process.platform === 'win32' ? screen.screenToDipRect(null, r) : r;
  async function ensureOverlay(){
    if (overlay && !overlay.isDestroyed()) return overlay;
    overlay = new BrowserWindow({
      show: false, frame: false, transparent: true, resizable: false, movable: false, focusable: false,
      skipTaskbar: true, hasShadow: false, alwaysOnTop: true, fullscreenable: false, backgroundColor: '#00000000',
      webPreferences: {preload: path.join(__dirname, 'overlay-preload.js'), contextIsolation: true, nodeIntegration: false, backgroundThrottling: false},
    });
    overlay.setAlwaysOnTop(true, 'screen-saver');
    overlay.setIgnoreMouseEvents(true);
    overlay.setContentProtection(true);
    await overlay.loadFile(path.join(__dirname, 'overlay.html'));
    return overlay;
  }
  ipcMain.handle('telao:overlayShow', async (_e, sourceId, rect) => {
    const d = overlayDisplay(sourceId, rect);
    let a = d.bounds;   // área do jogo (DIP): monitor inteiro ou a janela escolhida
    if (rect && rect.w > 0 && !/^screen:/.test(String(sourceId))){
      const g = toDip({x: rect.x, y: rect.y, width: rect.w, height: rect.h});
      const x1 = Math.max(g.x, d.bounds.x), y1 = Math.max(g.y, d.bounds.y);
      const x2 = Math.min(g.x + g.width, d.bounds.x + d.bounds.width), y2 = Math.min(g.y + g.height, d.bounds.y + d.bounds.height);
      if (x2 - x1 > 100 && y2 - y1 > 100) a = {x: x1, y: y1, width: x2 - x1, height: y2 - y1};
    }
    const w = Math.round(Math.min(360, Math.max(180, a.width * 0.25)));
    overlayBounds = {x: a.x + a.width - w, y: a.y, width: w, height: a.height};
    const o = await ensureOverlay();
    o.setBounds(overlayBounds);
    overlayActive = true;
    return {display: d.id, bounds: overlayBounds};
  });
  ipcMain.handle('telao:overlayHide', () => {
    overlayActive = false; clearTimeout(overlayHideTimer);
    if (overlay && !overlay.isDestroyed()) overlay.hide();
    return true;
  });
  // A camada some sozinha quando nada mais está animando (reação ~3,5 s; mensagem do chat ~9 s).
  let overlayHideAt = 0;
  const overlayKeep = ms => {
    overlayHideAt = Math.max(overlayHideAt, Date.now() + ms);
    clearTimeout(overlayHideTimer);
    overlayHideTimer = setTimeout(() => { if (overlay && !overlay.isDestroyed()) overlay.hide(); overlayHideAt = 0; }, overlayHideAt - Date.now());
  };
  const overlayShowNow = () => { if (!overlay.isVisible()){ if (overlayBounds) overlay.setBounds(overlayBounds); overlay.showInactive(); } };
  ipcMain.on('telao:overlayReact', (_e, emoji, who) => {
    if (!overlayActive || !overlay || overlay.isDestroyed()) return;
    overlayShowNow();
    overlay.webContents.send('overlay:item', {kind: 'react', text: String(emoji).slice(0, 4), who: String(who || '').slice(0, 24)});
    overlayKeep(3500);
  });
  const IMG_URL = /^data:image\/(png|jpeg|webp|gif);base64,[A-Za-z0-9+/=]+$/;
  const cleanImg = v => typeof v === 'string' && v.length < 600000 && IMG_URL.test(v) ? v : '';
  ipcMain.on('telao:overlayItem', (_e, it) => {
    if (!overlayActive || !overlay || overlay.isDestroyed() || !it || typeof it !== 'object') return;
    const kind = it.kind === 'chat' ? 'chat' : 'react';
    const item = {kind, text: String(it.text || '').slice(0, kind === 'chat' ? 220 : 16), who: String(it.who || '').slice(0, 24), img: cleanImg(it.img), av: cleanImg(it.av)};
    if (!item.text && !item.img) return;
    overlayShowNow();
    overlay.webContents.send('overlay:item', item);
    overlayKeep(kind === 'chat' ? 9000 : 3500);
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

app.on('before-quit', () => { stopHelper(); if (overlay && !overlay.isDestroyed()) overlay.destroy(); });
app.on('will-quit', scheduleExeIcon);
app.on('window-all-closed', () => { stopHelper(); app.quit(); });
// a camada não é "janela de verdade": fechar a janela principal fecha o app mesmo com ela aberta
app.on('browser-window-created', (_e, w) => {
  w.on('closed', () => {
    if (w !== win) return;
    for (const o of BrowserWindow.getAllWindows()) if (!o.isDestroyed()) o.destroy();   // camada e janelas flutuantes
  });
});
