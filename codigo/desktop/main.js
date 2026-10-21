// Telão Desktop — processo principal (Electron).
// A interface é a mesma página do Telão web (telao.html). Aqui ficam só as partes de desktop:
//   - seletor de janela/tela próprio (a página pede a lista e escolhe; o getDisplayMedia usa a escolha)
//   - Telão Audio embutido, iniciado escondido junto com o app
//   - sem economia de energia em segundo plano, para a transmissão não engasgar com o app minimizado
const {app, BrowserWindow, desktopCapturer, session, ipcMain, shell, Menu} = require('electron');
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
  win.webContents.setWindowOpenHandler(({url}) => {
    if (/^https?:\/\//i.test(url)) shell.openExternal(url);
    return {action: 'deny'};
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
      const sources = await desktopCapturer.getSources({types: ['window', 'screen']});
      const src = sources.find(s => s.id === chosen.id);
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

  // ---- atualização automática ----
  ipcMain.handle('telao:ready', (_e, v) => { updater.pageReady(v); return {shell: SHELL_VERSION, cached: !!(updater.loaded && updater.loaded.cached)}; });
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

app.on('before-quit', stopHelper);
app.on('window-all-closed', () => { stopHelper(); app.quit(); });
