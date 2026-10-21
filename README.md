# Telão

Compartilhamento de tela entre amigos, direto de PC pra PC. Sem conta, sem OBS, sem stream pública.

## Como usar

- **No navegador (Chrome ou Edge):** abra a [versão web](https://lightning1132.github.io/telaoapp/). Ela está sempre na versão mais nova.
- **App para Windows:** baixe o `Telao-Desktop-win64.zip` na [última versão](https://github.com/lightning1132/telaoapp/releases/latest), extraia e abra o `Telao.exe`. Depois disso, o app se atualiza sozinho.

Todo mundo entra com o mesmo código de sala. A voz continua no Discord; o Telão cuida só do vídeo.

## O que tem aqui

| Arquivo | Para quê |
|---|---|
| `telao.html` / `index.html` | A interface do Telão (a mesma no navegador e no app) |
| `latest.json` | Informa ao app qual é a versão mais nova (usado pela atualização automática) |
| `codigo/desktop/` | Código do app desktop (Electron) |
| `codigo/audio/` | Código do Telão Audio (captura do som só da janela do jogo, Windows) |

O vídeo vai direto entre os PCs (WebRTC). Nada passa por servidor nosso.
