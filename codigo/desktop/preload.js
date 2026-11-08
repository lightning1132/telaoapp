// Ponte mínima entre a página do Telão e o processo principal. A página detecta o modo desktop por window.telaoDesktop.
const {contextBridge, ipcRenderer} = require('electron');
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
