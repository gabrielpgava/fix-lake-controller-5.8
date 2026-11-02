# Lake Controller 5.8 — Windowed Fix

[Português](README.md) | [English](README.en.md)

> **SOMENTE PATCH — o software Lake Controller não acompanha este pacote.**
> Lake Controller, Contour e demais arquivos proprietários não são distribuídos.
> É necessária uma instalação original legítima e licença quando exigida.
> Projeto independente, não oficial nem endossado pelo fabricante. A licença MIT
> cobre somente os arquivos próprios deste projeto, não o software ou as marcas
> de terceiros. [Aviso em português e inglês](NOTICE.md).

O aviso também acompanha o arquivo `LICENSE` do ZIP e do payload do instalador.

[Baixar instalador Windows v0.7.1](https://github.com/gabrielpgava/fix-lake-controller-5.8/releases/download/v0.7.1/LakeWindowedFix-Setup-v0.7.1.exe)
· [Release, ZIP e checksums](https://github.com/gabrielpgava/fix-lake-controller-5.8/releases/tag/v0.7.1)

Shim **experimental, x86**, restrito ao **Lake Controller 5.8 Build 007** pelos
hashes admitidos. Não altera executáveis proprietários no disco, licença,
firmware, presets, backend de áudio nem resolução/cor global do Windows.

## 0.7.1 — ícone do patch

O atalho **Lake Controller - Janela** agora tem ícone próprio com faixa vermelha
**PATCHED**. O atalho/ícone do Lake original não é substituído, permitindo manter
as duas opções na área de trabalho.

## 0.7.0 — confiabilidade

Implementa captura contínua por eixo, startup/ABI mais restritivos, restauração
do cache de apresentação, diagnóstico limitado e recuperação de instalação.
Builds passam a registrar sua própria proveniência; pacotes antigos não recebem
a identidade do HEAD atual. [Implementação e pendências](docs/IMPLEMENTATION.md).

**Não houve ensaio independente do Lake real nem de áudio/hardware nesta revisão.**
O usuário relatou um teste local positivo do candidato anterior (`8949dac`);
isso não valida automaticamente o instalador localizado ou outro computador.
Matemática, fixtures Win32/COM e sandbox não aprovam GPU, controles proprietários
ou sessões longas. Ensaios históricos estão separados em [HISTORY.md](HISTORY.md).
Resultados e comandos desta candidata: [VALIDATION-07.md](docs/VALIDATION-07.md).

## Instalar, usar e remover

1. Obtenha o setup v0.7.1 e confira seus checksums/manifesto. Esta release é
   experimental e **não assinada**; não desative Windows/EDR para executá-la.
2. Feche o Lake. No setup, confira a compatibilidade e selecione **Instalar**.
    O software original legítimo deve estar instalado; outro hash é recusado.
    Escolha **Português (Brasil)** ou **English** no campo **Idioma / Language**.
3. Abra pelo atalho **Lake Controller - Janela**. O ícone do patch tem uma faixa
   vermelha **PATCHED**; o atalho/ícone original do Lake não é substituído. Mantenha
   o original na área de trabalho para abrir a versão legada se o patch der problema.
   Comece em ambiente isolado, sem áudio ativo/equipamentos sob controle. O setup
   não inicia o Lake sozinho.
4. Para remover, use a entrada do patch em Aplicativos ou o setup externo.
   Configuração editada, logs e itens alheios são preservados; o recibo permite
   retomar remoção parcial/reinstalação. Não exclua a pasta recursivamente.

Reparo, atualização, rollback e recuperação: [SETUP-RECOVERY.md](docs/SETUP-RECOVERY.md).
Não use a instalação operacional como sandbox. **OFFLINE não isola a rede**:
isolamento real precisa ser estabelecido pelo operador fora do aplicativo.

Se já houver outra geração do patch instalada, use o setup externo:

```powershell
.\LakeWindowedFix-Setup-v0.7.1.exe --upgrade --lang pt-BR
```

Também são aceitos `--lang en`, `--check`, `--uninstall` e `--rollback`.
O idioma inicial acompanha o Windows (português → Português; demais → English),
sem alterar o idioma do sistema. Botões de diálogos nativos do Windows continuam
seguindo o próprio Windows. Caminhos/atalhos históricos são mantidos por compatibilidade.

### Portátil

Mantenha launcher/DLL/INI do mesmo build, fora de System32/SysWOW64:

```powershell
.\dist-release-v0.7.1\LakeWindowedLauncher.exe --app-dir "C:\caminho\Lake Controller v5.8" --wait
```

Não injeta em instância existente. O mutex serializa launchers do usuário/sessão;
detecção por nome continua recusando instância externa. `--wait` espera a saída
sem timeout de sessão e registra o código real; startup tem limites próprios.
Não reinicia/mata uma sessão por silêncio, modal ou ausência de frames.

Comparação diagnóstica: `--native-only --wait` usa **`windowed nonfullscreen`**,
sem DLL/host/hooks. Não equivale ao EXE original sem opções e também executa os
comportamentos nativos de rede/configuração. Não é um teste seguro de áudio ao vivo.

## Configuração — fechar e reabrir para aplicar

Edite o INI ao lado da DLL. Snapshot imutável por sessão, schema 1, SHA-256 da
configuração efetiva; diagnóstico distingue pedido válido, default, inválido e
override. Schema ausente em INIs legados é aceito; schema futuro é recusado.
Reparo mescla apenas chaves ausentes e mantém backup/edições/chaves desconhecidas.

| Opção | Default | Efeito |
| --- | --- | --- |
| `[render] Width/Height` | 1088×612 | Canvas fixo durante toda a sessão; aceita 1024–4096 / 600–4096 |
| `[window] Width/Height` | 1024×576 | Janela inicial lógica; resize muda apresentação, não canvas |
| `[window] MinWidth/MinHeight` | 800×450 | Mínimo interativo limitado à área útil; 320/180 permite limite anterior |
| `ScaleRendering / NativeResize` | 1 / 0 | Proporção/letterbox; rebuild nativo continua opt-in experimental |
| `SmoothFonts / SystemCursor` | 1 / 1 | Opções existentes; cursor bitmap/especializado com `SystemCursor=0` |
| `[diagnostics] DetailedInput` | 0 | Contadores do par getter/setter admitido; não ativa resíduos fracionários |

Mantidos **32 bpp** e awareness DPI legado; pixels lógicos não são promessa de
1:1 físico. Cursor híbrido e acumulação de pequenos deltas permanecem desativados
sem evidência semântica suficiente. [Input e limites](docs/INPUT-RELIABILITY.md).

## Diagnóstico

Logs locais em `logs\LakeWindowedFix-<PID>.log` junto à DLL, com fallback em TEMP.
Sessão tem GUID independente de PID, UTC, tempo monotônico, build e configuração.
Fila fixa descarta **somente registros diagnósticos** sob saturação, contando perdas;
nunca input/frames. Consumer fora do renderer/VEH, teto de 4 MiB por arquivo e
estatísticas de proxies em baixa frequência. Revise antes de compartilhar; sem upload.

- [Startup/identidade](docs/STARTUP-RELIABILITY.md), [diagnóstico](docs/DIAGNOSTICS.md).
- [Inspeção host/canvas e monitor até 24 h](docs/OBSERVATION.md).
- [DirectDraw próprio](docs/GRAPHICS-CHECK.md): `make graphics-check-build` compila
  e testa apenas CPU. GPU exige desktop ativo/desbloqueado confirmado explicitamente.
- [Presenter/Restore/COM](docs/PRESENTER-RELIABILITY.md): não promete recuperar todo
  o renderer proprietário. Primeira apresentação não comprova estabilidade/áudio.

Não descarregue a DLL fixada nem remova hooks durante uma sessão. Exceção de primeira
chance pode ser tratada; falta de resposta momentânea não comprova crash. Minidumps
não são coletados no VEH nem enviados; coletor externo permanece pendente.

## Desenvolver e validar

Windows x86; Zig **0.14.1** portátil e **LLVM objcopy** locais (sem download
automático pelos builds). LLVM objcopy separa símbolos, não substitui o Zig:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:\tools\zig\zig.exe -ObjcopyPath C:\tools\LLVM\bin\llvm-objcopy.exe -OutputDirectory dist-release-v0.7.1
.\scripts\Build-Installer.ps1 -ZigPath C:\tools\zig\zig.exe -ObjcopyPath C:\tools\LLVM\bin\llvm-objcopy.exe -AllowUnsignedDevelopment
make release-check ZIG=C:/tools/zig/zig.exe OBJCOPY=C:/tools/LLVM/bin/llvm-objcopy.exe
```

`release-check` compila/testa/empacota e valida candidato na branch de trabalho;
**não publica, cria tag nem faz upload**. `make publish-check` é o preflight de
publicação separado, mantendo a guarda `HEAD == origin/main`. Não faça merge apenas
para aprovar candidato. `make release`/`release-draft` são operações remotas explícitas,
separadas do build. [Proveniência, sequência e recuperação](docs/BUILD-PROVENANCE.md).

MSVC: Developer PowerShell x86 + CMake/SDK, `Build.ps1 -Toolchain MSVC`; setup fase
dois com `Build-Installer.ps1 -Toolchain MSVC -AllowUnsignedDevelopment`.
Workflow Windows x86 Zig/MSVC adicionado; execução remota não é presumida. MinGW GCC
permanece experimental sem evidência própria. Python é usado pelos snapshots de
inspeção; `-SkipTests` não produz um pacote distribuível. CMake direto sem stamp
é desenvolvimento não rastreado, recusado pelo empacotador.

Binários Zig finais são stripped antes dos testes/payload; imagens originais e
PDBs do **mesmo link otimizado**, com GUID/age e SHA-256, ficam no store privado
por BuildId. MSVC preserva o PE PDB-only sem alteração. Não entram no ZIP/setup
do operador. Seleção automática da ferramenta e limites: [SYMBOLS.md](docs/SYMBOLS.md).

Consulte [CONTRIBUTING.md](CONTRIBUTING.md), [matriz de requisitos](docs/IMPLEMENTATION.md)
e [limites históricos de análise](docs/ANALYSIS.md). Somente código/recursos próprios
MIT são distribuídos. Lake/Dolby e seus binários não fazem parte desta licença.
