# Bluetooth Manager — overlay para Ultrahand

Overlay para a Nintendo Switch (1ª geração) que corre no **Ultrahand Overlay** e permite
emparelhar e gerir dispositivos Bluetooth sem sair do jogo.

## Funcionalidades

| Ecrã | O que faz |
|---|---|
| **Menu principal** | Estado do rádio Bluetooth e número de dispositivos emparelhados / ligados |
| **Emparelhar → Comando** | Ativa o modo de emparelhamento (SYNC). Mostra em tempo real os comandos novos que forem emparelhados |
| **Emparelhar → Auscultadores / colunas** | Procura dispositivos de áudio, lista-os à medida que aparecem e liga/emparelha com **A** |
| **Gerir → Dispositivos emparelhados** | Lista comandos e dispositivos de áudio, com estado *Ligado* atualizado a cada segundo |
| **Detalhe do dispositivo** | Tipo, endereço, VID:PID, estado · **Ligar** (áudio) · **Desligar** · **Esquecer** (confirmação em dois toques) |
| **Esquecer todos os comandos** | Limpa a base de dados de comandos (confirmação em dois toques) |

Ao sair de um ecrã, o modo de emparelhamento / pesquisa é sempre cancelado.

## Requisitos

- Atmosphère + **Ultrahand Overlay** instalado (HOS 16.0.0+ exigido pelo Ultrahand)
- Firmware **13.0.0+** (APIs de áudio Bluetooth e `GetDeviceInfo` usadas pelo overlay)

## Limitações do sistema (não do overlay)

- A Switch só suporta nativamente **comandos** (Joy-Con, Pro Controller, comandos licenciados)
  e **áudio** (auscultadores/colunas). Teclados, ratos ou telemóveis não são suportados pelo firmware.
- Comandos de outras marcas (PS4/PS5/Xbox…) só emparelham com o sysmodule
  [MissionControl](https://github.com/ndeadly/MissionControl) instalado; depois aparecem aqui normalmente.
- Com áudio Bluetooth ligado, só é possível usar 2 comandos sem fios e o jogo local sem fios fica indisponível.
- Microfones Bluetooth não são suportados.

## Compilar

### Opção A — GitHub Actions (sem instalar nada)
1. Cria um repositório no GitHub e envia esta pasta.
2. O workflow `.github/workflows/build.yml` compila automaticamente com a imagem oficial do devkitPro.
3. Descarrega o artefacto **ovl-bluetooth** no separador *Actions*.

### Opção B — Localmente
1. Instala o [devkitPro](https://devkitpro.org/wiki/Getting_Started) com `switch-dev`.
2. Instala as dependências do libultrahand:
   ```sh
   (dkp-)pacman -S switch-curl switch-zlib switch-minizip switch-mbedtls
   ```
3. Na raiz do projeto:
   ```sh
   make
   ```
O resultado fica em `out/switch/.overlays/ovl-bluetooth.ovl`.

> O `libultrahand` está incluído em `lib/libultrahand`. Para o atualizar, substitui essa pasta pela
> versão mais recente de https://github.com/ppkantorski/libultrahand.

## Instalar

Copia `ovl-bluetooth.ovl` para `sd:/switch/.overlays/` e abre o Ultrahand (por defeito **ZL + ZR + ↓**).
O overlay aparece como **Bluetooth Manager**.

## Estrutura

```
source/
  main.cpp          Ecrãs (MainGui, PairGamepadGui, PairAudioGui, DevicesGui, DeviceGui)
  bt_manager.hpp    API da camada Bluetooth
  bt_manager.cpp    Implementação sobre btm / btm:sys da libnx
lib/libultrahand/   Biblioteca de UI (fork do libtesla)
```

### Serviços usados

| Ação | Função libnx |
|---|---|
| Listar emparelhados | `btmGetDeviceInfo` + `btmGetDeviceCondition` + `btmsysGetConnectedAudioDevices` |
| Emparelhar comando | `btmsysStartGamepadPairing` / `btmsysCancelGamepadPairing` |
| Procurar áudio | `btmsysStartAudioDeviceDiscovery` / `btmsysGetDiscoveredAudioDevice` |
| Ligar áudio | `btmsysConnectAudioDevice` |
| Desligar | `btmsysDisconnectAudioDevice` (áudio) · `btmHidDisconnect` (comando) |
| Esquecer | `btmsysRemoveAudioDevicePairing` (áudio) · `btmRemoveDeviceInfo` (comando) |
| Esquecer todos os comandos | `btmsysClearGamepadPairingDatabase` |

Os erros são mostrados no formato `2XXX-YYYY`, igual ao do sistema.

## Personalização

- Tema / papel de parede próprios: `sd:/config/ovl-bluetooth/theme.ini` e `wallpaper.rgba`
- Traduções: `sd:/config/ovl-bluetooth/lang/<código>.json`

## Licença

GPLv2 (por usar o libultrahand).
