// Ponte mínima entre a página do Telão e o processo principal. A página detecta o modo desktop por window.telaoDesktop.
const {contextBridge, ipcRenderer} = require('electron');

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
  onUpdate: cb => ipcRenderer.on('telao:update', (_e, info) => cb(info)),
  overlayShow: (sourceId, rect) => ipcRenderer.invoke('telao:overlayShow', sourceId, rect),
  overlayHide: () => ipcRenderer.invoke('telao:overlayHide'),
  overlayReact: (emoji, who) => ipcRenderer.send('telao:overlayReact', emoji, who),
  floatWindows: true,
});
