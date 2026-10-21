// Ponte da camada por cima do jogo (reações e chat): só recebe eventos, não manda nada.
const {contextBridge, ipcRenderer} = require('electron');
contextBridge.exposeInMainWorld('overlay', {
  onItem: cb => ipcRenderer.on('overlay:item', (_e, d) => cb(d)),
});
