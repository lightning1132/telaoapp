const {contextBridge, ipcRenderer} = require('electron');
const arg = name => ((process.argv.find(a => a.startsWith('--' + name + '=')) || '').split('=')[1]) || '';

// Quadros dentro da página (o player do YouTube da jukebox): nada da ponte do Telão; no YouTube, só o bloqueio de anúncio.
const isMainFrame = typeof process.isMainFrame === 'boolean' ? process.isMainFrame : (() => { try{ return window.top === window; }catch{ return false; } })();
if (!isMainFrame){
  let host = ''; try{ host = location.hostname; }catch{}
  const testHost = arg('telao-adblock-host');
  if (/(^|\.)(youtube\.com|youtube-nocookie\.com)$/i.test(host) || (testHost && host === testHost)){
    let on = false; try{ on = ipcRenderer.sendSync('telao:adblockOn') === true; }catch{}
    // roda no mundo da página antes dos scripts do YouTube: tira os anúncios da resposta do player
    // (mesma técnica dos bloqueadores de navegador: as listas de anúncio somem antes de o player ler)
    if (on) try{ contextBridge.executeInMainWorld({func: () => {
      if (window.__telaoAdblock) return;
      window.__telaoAdblock = true;
      const KEYS = ['adPlacements', 'adSlots', 'playerAds', 'adBreakHeartbeatParams', 'adBreakParams'];
      const prune = o => {
        try{
          if (!o || typeof o !== 'object') return o;
          for (const k of KEYS) if (Object.prototype.hasOwnProperty.call(o, k)) delete o[k];
          if (o.playerResponse && typeof o.playerResponse === 'object') prune(o.playerResponse);
          if (Array.isArray(o)) for (const x of o) if (x && typeof x === 'object' && x.playerResponse) prune(x.playerResponse);
        }catch{}
        return o;
      };
      const jp = JSON.parse;
      JSON.parse = function(){ return prune(jp.apply(this, arguments)); };
      const rj = Response.prototype.json;
      Response.prototype.json = function(){ return rj.apply(this, arguments).then(prune); };
      let ipr;
      try{ Object.defineProperty(window, 'ytInitialPlayerResponse', {configurable: true, get(){ return ipr; }, set(v){ ipr = prune(v); }}); }catch{}
    }}); }catch{}
  }
} else {
('electron');
const arg = name => ((process.argv.find(a => a.startsWith('--' + name + '=')) || '').split('=')[1]) || '';

contextBridge.exposeInMainWorld('telaoDesktop', {
  version: ((process.argv.find(a => a.startsWith('--telao-app-version=')) || '').split('=')[1]) || '?',
  getSources: () => ipcRenderer.invoke('telao:sources'),
  select: (id, systemAudio) => ipcRenderer.invoke('telao:select', id, systemAudio),
  gpu: () => ipcRenderer.invoke('telao:gpu'),
  setConfig: (key, value) => ipcRenderer.invoke('telao:setConfig', key, value),
  restart: () => ipcRenderer.invoke('telao:restart'),
  openGpuPage: () => ipcRenderer.invoke('telao:gpuPage'),
  openClips: () => ipcRenderer.invoke('telao:openClips'),
  saveClip: (name, data) => ipcRenderer.invoke('telao:saveClip', name, data),
  shell: parseInt(((process.argv.find(a => a.startsWith('--telao-shell=')) || '').split('=')[1]) || '0', 10),
  ready: v => ipcRenderer.invoke('telao:ready', v),
  checkUpdate: () => ipcRenderer.invoke('telao:checkUpdate'),
  applyUpdate: () => ipcRenderer.invoke('telao:applyUpdate'),
  applyApp: () => ipcRenderer.invoke('telao:applyApp'),
  onUpdate: cb => ipcRenderer.on('telao:update', (_e, info) => cb(info)),
  screenForWindow: m => ipcRenderer.invoke('telao:screenFor', m),
  steamSearch: term => ipcRenderer.invoke('telao:steamSearch', term),
  // jukebox: bloqueio de anúncio no player
  ytStatus: () => ipcRenderer.invoke('telao:ytStatus'),
  ytAdblock: on => ipcRenderer.invoke('telao:ytAdblock', !!on),
  metrics: () => ipcRenderer.invoke('telao:metrics'),
  wco: parseInt(arg('telao-wco'), 10) || 0,
  titleBar: c => ipcRenderer.invoke('telao:titleBar', c),
  idle: () => ipcRenderer.invoke('telao:idle'),
  onPower: cb => ipcRenderer.on('telao:power', (_e, ev) => cb(String(ev))),
  hwnd: () => ipcRenderer.invoke('telao:hwnd'),
  profile: arg('telao-perfil'),
  room: arg('telao-sala'),
  openTest: (room, mode) => ipcRenderer.invoke('telao:openTest', room, mode),
  mode: arg('telao-modo'),
  openScene: (html, w, h) => ipcRenderer.invoke('telao:openScene', html, w, h),
  closeScene: () => ipcRenderer.invoke('telao:closeScene'),
  natGesture: () => ipcRenderer.invoke('telao:natGesture'),
  floatWindows: true,
  mainPid: parseInt(arg('telao-main-pid'), 10) || 0,
  helperKey: /^[0-9a-f]{64}$/.test(arg('telao-helper-key')) ? arg('telao-helper-key') : '',   // vai na conexão com o Telão Audio
  monitorRect: id => ipcRenderer.invoke('telao:monitorRect', id),
});
}
