// Ponte mínima entre a página do Telão e o processo principal. A página detecta o modo desktop por window.telaoDesktop.
const {contextBridge, ipcRenderer, sharedTexture} = require('electron');

// Captura turbo: o processo principal manda as texturas importadas para cá; a cada "quadro pronto" a página
// pede o quadro N e ele vai para ela por um MessagePort (VideoFrame não atravessa o contextBridge).
const turboSlots = new Map();   // geração → [texturas]
let turboPort = null;
try{
  if (sharedTexture) sharedTexture.setSharedTextureReceiver(async ({importedSharedTexture}, gen, slot) => {
    let a = turboSlots.get(gen); if (!a) turboSlots.set(gen, a = []);
    if (a[slot]) try{ a[slot].release(); }catch{}
    a[slot] = importedSharedTexture;
  });
}catch{}
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
  overlayShow: (sourceId, rect) => ipcRenderer.invoke('telao:overlayShow', sourceId, rect),
  overlayHide: () => ipcRenderer.invoke('telao:overlayHide'),
  overlayReact: (emoji, who) => ipcRenderer.send('telao:overlayReact', emoji, who),
  overlayItem: item => ipcRenderer.send('telao:overlayItem', item),
  screenForWindow: m => ipcRenderer.invoke('telao:screenFor', m),
  steamSearch: term => ipcRenderer.invoke('telao:steamSearch', term),
  metrics: () => ipcRenderer.invoke('telao:metrics'),
  wco: parseInt(arg('telao-wco'), 10) || 0,
  titleBar: c => ipcRenderer.invoke('telao:titleBar', c),
  idle: () => ipcRenderer.invoke('telao:idle'),
  onPower: cb => ipcRenderer.on('telao:power', (_e, ev) => cb(String(ev))),
  floatWindows: true,
  turbo: !!sharedTexture,
  mainPid: parseInt(arg('telao-main-pid'), 10) || 0,
  monitorRect: id => ipcRenderer.invoke('telao:monitorRect', id),
  turboOpen: () => {
    if (turboPort) try{ turboPort.close(); }catch{}
    const ch = new MessageChannel(); turboPort = ch.port1;
    window.postMessage({telaoTurboPort: true}, '*', [ch.port2]);
    return true;
  },
  turboImport: info => ipcRenderer.invoke('telao:turboImport', info),
  turboFrame: (gen, slot) => {
    const a = turboSlots.get(gen), t = a && a[slot];
    if (!t || !turboPort) return false;
    const f = t.getVideoFrame();
    turboPort.postMessage({gen, slot, f}, [f]);
    return true;
  },
  turboRelease: gen => {
    const a = turboSlots.get(gen);
    if (a){ turboSlots.delete(gen); for (const t of a) if (t) try{ t.release(); }catch{} }
    return ipcRenderer.invoke('telao:turboRelease', gen);
  },
});
