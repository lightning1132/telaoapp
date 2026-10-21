// Telão Desktop — processo principal (Electron).
// A interface é a mesma página do Telão web (telao.html). Aqui ficam só as partes de desktop:
//   - seletor de janela/tela próprio (a página pede a lista e escolhe; o getDisplayMedia usa a escolha)
//   - Telão Audio embutido, iniciado escondido junto com o app
//   - sem economia de energia em segundo plano, para a transmissão não engasgar com o app minimizado
const {app, BrowserWindow, desktopCapturer, session, ipcMain, shell, Menu, screen, net} = require('electron');
const path = require('path');
const fs = require('fs');
const {spawn} = require('child_process');
const {Updater, SHELL_VERSION} = require('./updater');

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
if (!config.disableGpuWebrtcFeatures) app.commandLine.appendSwitch('enable-features', GPU_FEATURES);

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

function createWindow(){
  win = new BrowserWindow({
    width: 1280, height: 800, minWidth: 720, minHeight: 480,
    backgroundColor: '#0d0f12',
    title: 'Telão',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false,
      additionalArguments: ['--telao-app-version=' + app.getVersion(), '--telao-shell=' + SHELL_VERSION],
    },
  });
  Menu.setApplicationMenu(null);
  // Página: a baixada pela atualização automática (se mais nova e saudável) ou a instalada.
  const page = updater.choosePage();
  win.loadFile(page.file);
  updater.watch(win);

  // Links externos abrem no navegador; nada de janelas novas dentro do app.
  win.webContents.setWindowOpenHandler(({url, frameName}) => {
    // Janela flutuante do Telão: popup em branco da própria página, sempre por cima.
    if ((url === 'about:blank' || url === '') && /^telao-float-/.test(frameName || '')){
      return {action: 'allow', overrideBrowserWindowOptions: {
        width: 520, height: 300, minWidth: 240, minHeight: 140, alwaysOnTop: true, autoHideMenuBar: true,
        backgroundColor: '#000000', title: 'Telão', fullscreenable: true, minimizable: true,
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
  ipcMain.handle('telao:openClips', () => { try{ fs.mkdirSync(clipsDir(), {recursive: true}); }catch{} return shell.openPath(clipsDir()); });

  // ---- diagnóstico de vídeo ----
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
      features: app.commandLine.getSwitchValue('enable-features') || '(nenhuma)',
      versions: {app: app.getVersion(), shell: SHELL_VERSION, page: updater && updater.loaded ? updater.loaded.version + (updater.loaded.cached ? ' (atualizada)' : ' (instalada)') : '?',
                 electron: process.versions.electron, chrome: process.versions.chrome},
      platform: process.platform + ' ' + require('os').release(),
    };
  });
  ipcMain.handle('telao:setConfig', (_e, key, value) => {
    if (key !== 'ignoreGpuBlocklist' && key !== 'disableGpuWebrtcFeatures') return false;
    const c = readConfig(); c[key] = !!value;
    return writeConfig(c);
  });
  ipcMain.handle('telao:restart', () => { app.relaunch(); app.exit(0); });
  ipcMain.handle('telao:gpuPage', () => {
    const w = new BrowserWindow({width: 1000, height: 800, title: 'Detalhes da placa de vídeo', autoHideMenuBar: true});
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
  ipcMain.handle('telao:ready', (_e, v) => { updater.pageReady(v); updater.markAppOk(); return {shell: SHELL_VERSION, cached: !!(updater.loaded && updater.loaded.cached)}; });
  ipcMain.handle('telao:applyApp', () => updater.applyApp());
  ipcMain.handle('telao:checkUpdate', () => updater.check());
  ipcMain.handle('telao:applyUpdate', () => {
    if (!win) return false;
    const page = updater.choosePage();
    win.loadFile(page.file);
    updater.watch(win);
    return page.version;
  });

  updater = new Updater(__dirname);
  startHelper();
  createWindow();
  updater.start();
});

app.on('before-quit', () => { stopHelper(); if (overlay && !overlay.isDestroyed()) overlay.destroy(); });
app.on('window-all-closed', () => { stopHelper(); app.quit(); });
// a camada não é "janela de verdade": fechar a janela principal fecha o app mesmo com ela aberta
app.on('browser-window-created', (_e, w) => {
  w.on('closed', () => {
    if (w !== win) return;
    for (const o of BrowserWindow.getAllWindows()) if (!o.isDestroyed()) o.destroy();   // camada e janelas flutuantes
  });
});
