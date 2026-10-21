// Ponte da camada de reações: só recebe eventos, não manda nada.
const {contextBridge, ipcRenderer} = require('electron');
contextBridge.exposeInMainWorld('overlay', {
  onReact: cb => ipcRenderer.on('overlay:react', (_e, d) => cb(d)),
});
