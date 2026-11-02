# Lake Controller Windowed Fix

> Histórico preservado da base `ed8756b`. As contagens e ensaios abaixo não
> validam a candidata 0.7. Consulte o [README atual](README.md) e
> [a matriz desta revisão](docs/IMPLEMENTATION.md). Comandos partem da raiz do repositório.

## Candidata de desenvolvimento — otimizações adicionais

Uma revisão local reduz consultas repetidas do cursor e reúne o snapshot do presenter sob um único lock de leitura, mantendo lifetime/erros nativos. Acrescenta `make profile`, um build de diagnóstico separado e opcional, desativado no runtime normal. Não inicia o Lake e não altera o INI, atalhos ou binários proprietários. **Ainda não há ganho adicional de FPS/CPU ou correção de leak comprovados.** Implementação, builds isolados e limites em [docs/OPTIMIZATION.md](docs/OPTIMIZATION.md).

Os builds finais normal (`dist-next/`) e diagnóstico (`dist-profile-v0.6.0/`) passaram em **1.487 verificações cada, sem falhas nem warnings**. A distribuição 0.6 anterior foi preservada; não houve instalação nem novo ensaio com o Lake real.

## Instalação simples — instalador nativo

Para o usuário final, o fluxo é **duplo clique no `LakeWindowedFix-Setup-v0.6.0.exe` do pacote de release > Instalar > atalho Lake Controller - Janela**. Detecta o Lake compatível, instala apenas no perfil do usuário e cria os atalhos, sem comandos ou edição manual de INI. O Lake original precisa estar instalado e não é substituído.

O instalador passou em **58 verificações de sandbox**, sem falhas, e na verificação somente-leitura da interface/detecção automática. Um cenário opcional de links simbólicos foi pulado por falta de privilégio, não contado como sucesso. Não instalamos no perfil real nem homologamos uso em campo. Detalhes em [docs/SETUP.md](docs/SETUP.md). Os comandos abaixo são para desenvolvedores/usuários avançados, não passos obrigatórios para o operador. O executável ainda não é assinado digitalmente; Windows/EDR podem exigir revisão conforme a política local.

Para desenvolvedores, o [Makefile](Makefile) oferece `make build`, `make installer`, `make package` e `make release`. A publicação é explícita, como prerelease experimental, com verificação de commit/hashes e sem incluir arquivos proprietários ou privados. As guardas de publicação têm **17 testes com Git/GitHub simulados**, sem mutação remota. Uso e pré-requisitos em [docs/RELEASE.md](docs/RELEASE.md).

## 0.6 experimental: custo de input e usabilidade

A candidata 0.6 mantém canvas **1088×612**, janela inicial **1024×576 lógica**, fontes/cursor da 0.5 e **32 bits de cor**. Trabalha em snapshots locais de geometria/input, uma única amostra de hit-test do cursor, eliminação de pintura GDI sem barras e consultas diagnósticas somente quando necessárias.

Acrescenta `[window] MinWidth=800` / `MinHeight=450` como mínimo **interativo e configurável**, limitado à área útil do monitor. `320/180` restaura o limite anterior. O renderer não é reconstruído por resize. Um monitor PowerShell somente-leitura permite observar CPU/memória por mais tempo, sem enviar input ou alterar o Lake.

Saída separada **`dist-v0.6\`**, preservando 0.3/0.4/0.5. Build Zig x86: **1.159 PASS, zero falhas e zero warnings, oito suítes**. O ensaio OFFLINE confirmou mínimo padrão/opt-out, 48/48 cliques Home/Modules, resize, margens e foco. Uma observação de cinco minutos completou 300 amostras válidas. Em execuções novas, a seta nativa teve 240/240 amostras visíveis após resize e foco; houve também um estado anterior oculto, com fallback bitmap e causa não determinada.

Não se promete ganho de FPS nem correção de vazamento: a memória privada também cresceu no controle 0.4. Resultados, limites e uso do monitor em [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## 0.5 experimental: fontes e cursor

A 0.5 mantém o canvas **1088×612** e a janela inicial **1024×576 pixels lógicos**, no modo escalado (`ScaleRendering=1`, `NativeResize=0`), e acrescenta:

- **`[render] SmoothFonts=1`**: antialiasing em escala de cinza nos pedidos Arial/Courier New do renderer, sem trocar tamanho, peso ou fonte solicitados. Fontes bitmap/LCD continuam originais; a melhoria visual é parcial, sem blur geral da tela.
- **`[input] SystemCursor=1`**: seta padrão do Windows independente dos frames do Lake. Substitui o cursor bitmap do canvas e suas formas especializadas; `SystemCursor=0` restaura o comportamento original. Subclasses dos controles filhos cobrem `WM_SETCURSOR`; em separado, `HookShowCursor` isola a normalização legada em somente quatro retornos verificados do Build 007, com guardas de renderer/cliente/thread de UI/modo ativo. Usa contador lógico local à thread, inicialização real balanceada e créditos próprios limitados; demais chamadas continuam nativas, sem mascarar HRESULTs gráficos.
- **32 bits de cor permanecem como padrão, sem flag pública de 16 bpp.** O ensaio RGB565/16 bpp terminou com exit 0, mas apresentou artefatos em texto/botões/cursor e não demonstrou ganho de cadência: mediana **52 ms**, p95 **64 ms** nas duas amostras, sem cenas idênticas ou benchmark prolongado. Não foi alterada a profundidade global do Windows; a arquitetura continua x86.

**Último build: 746 PASS, zero FAIL, zero warnings, sete suítes** (`build-final-05.txt`). No PID `4188`, **32 bpp/OFFLINE, sem Frida**, o percurso aviso → No → Modules manteve canvas vazio. Startup/barra, Modules/barra e Modules/canvas tiveram **240/240 amostras de cursor visível cada**, sem divergências de posição, medidas por `GetCursorInfo` em trajetórias de **2 s a 120 Hz**, com **120 posições distintas** cada. O encerramento normal ao pedido de pausa do usuário foi confirmado com **exit 0**.

**Reteste após a retomada, no mesmo build:** Home/Modules, resize **1280×720 → 800×600 → 1280×720**, margens, Alt+Tab/retorno e minimizar/restaurar foram operados em OFFLINE. As trajetórias repetidas na barra, canvas e margens tiveram **240/240 amostras visíveis**; a primeira trajetória após resize teve **238/240**, com duas amostras ocultas transitórias. O Windows inicialmente sinalizou `CURSOR_SUPPRESSED` (modo associado a toque/caneta); um evento de movimento de mouse restabeleceu a seta sem alterar o patch. Esse estado não deve ser confundido com contador negativo de `ShowCursor`.

A validação continua limitada ao ambiente e às trajetórias documentados: não garante ausência de transições breves nem aprova toda a interface, touch/pen, áudio, desempenho de GPU ou estabilidade prolongada. Resultados completos e limites em [UI-QUALITY.md](docs/UI-QUALITY.md).

Detalhes, limites, testes e comparação de cor em [docs/UI-QUALITY.md](docs/UI-QUALITY.md). Binários, logs e evidências privadas não são incluídos nos commits. Originais e distribuições 0.3/0.4 permanecem preservados; a distribuição local é **`dist-v0.5\`**, separada das versões anteriores:

```powershell
.\dist-v0.5\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

Esta seção registra a 0.5 preservada no commit `915c517`. Para a revisão atual, use a saída 0.6 dos exemplos de build abaixo. INIs existentes não são migrados automaticamente: confira as opções ao lado da DLL.

## Base 0.4 e histórico

Shim de compatibilidade **experimental**, MIT, para **Lake Controller 5.8 Build 007, x86**, voltado ao Windows 10/11. A **0.4 foi compilada e operada em OFFLINE no Windows 11, a 125% DPI**, com janela redimensionável, interface completa e mouse ajustado. **648 verificações sintéticas passaram, zero falhas.** Isso não valida outras GPUs, Windows 10 ou áudio em produção.

## 0.4: janela redimensionável, controles maiores e canvas fixo

- O novo padrão é **`ScaleRendering=1` e `NativeResize=0`**. A moldura privada `LakeWindowedFix.ScaledHost.v1` tem título, bordas, resize, maximizar e minimizar. A janela começa com área cliente de **1024×576 pixels lógicos**, ajustada à área útil; o mínimo interativo é **320×180**.
- O renderer mantém um canvas virtual **fixo de 1088×612**, cerca de **29% menor em cada dimensão** que o candidato 1536×864, para melhorar a leitura dos controles na mesma janela. É definido antes de construir a interface, não reduzido durante o resize. A barra completa foi verificada no ensaio real.
- Resize/restaurar não encaminham `WM_SIZE` a `ContourParent` nem solicitam seu teardown/rebuild. A observação final registrou **9.315 apresentações com HRESULT 0 e nenhuma chamada de criação de superfície** durante a amostra de resize/movimento/foco; não cobre toda a árvore gráfica ou toda a vida do aplicativo.
- `WrapDirectDraw7` usa proxies próprios completos: 30 entradas de `IDirectDraw7` e 49 da primária, com assinatura SDK e `this` real. **O `lpVtbl` nativo nunca é alterado.** O probe DirectDraw real confirmou QI de D3D7 e cooperação preservadas; o clone intermediário foi descartado.
- A apresentação adapta o **`Blt` da primária com fonte inteira**, consultando `GetSurfaceDesc`, preservando proporção e letterbox preto. Superfícies offscreen continuam nativas para o D3D7; fontes que sejam proxies são desembrulhadas antes do `Blt` real. O clipper pode ser associado ao host visível, sem mudar as flags originais de `SetCooperativeLevel`. **Não há port D3D7 → D3D11, sucesso DirectDraw forçado ou capacidades falsificadas.**
- A integração combina **IAT + proxies + seed guardado de dois dados de layout do Build 007**, após a seleção nativa de modo. O ensaio confirmou layout e superfície fonte em **1088×612**. Não altera arquivos proprietários, `GetDeviceCaps` ou capacidades; detalhes dos dados em memória estão em [ANALYSIS.md](docs/ANALYSIS.md).
- O mouse faz hit-test nos HWNDs filhos em coordenadas virtuais: a posição física pode atingir `WorkWindow` enquanto a imagem mostra `ButtonBar`. O main escala `lParam` no dispatch; filhos recebem coordenadas locais virtuais uma vez. Menus e diálogos foram operados; touch, IME e outros controles continuam sem validação universal.
- Se o driver recusar `COLORFILL` na primária, as faixas pretas usam GDI **somente dentro do host**. A apresentação continua devolvendo seu HRESULT real, sem mascarar erros do desenho principal.

Foram operados resize por borda, formatos 4:3/16:9, mínimo 320×180 lógico, maximização maior que o HWND-canvas, movimento, wheel, minimizar/restaurar e um ciclo Alt+Tab. A sessão final encerrou por **Quit → Yes, código 0**, sem processo restante. Cobertura e limites em [VALIDATION.md](docs/VALIDATION.md).

### Executar a compilação local 0.4

A saída é **`dist-v0.4\`**, sem substituir `dist-v0.3\`. As pastas `dist*` são saídas locais ignoradas pelo Git. Mantenha launcher, DLL e INI da mesma versão; INIs existentes são preservados pelo build, não migrados automaticamente.

Com outras instâncias fechadas, primeiro em OFFLINE e fora de produção:

```powershell
.\dist-v0.4\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

Feche outras instâncias antes de testar; o launcher não as encerra. Se o processo continuar fechando, compare em uma execução separada somente as flags internas, **sem DLL/host/hooks**:

```powershell
.\dist-v0.4\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only --wait
```

`--wait` aguarda o término sem timeout na observação normal, consulta `GetExitCodeProcess` e imprime/grava decimal e hexadecimal em `logs\LakeWindowedFix-<PID>-exit.log` ao lado do launcher. O log interno esperado é `logs\LakeWindowedFix-<PID>.log` ao lado da DLL; há fallback em `%TEMP%\LakeWindowedFix\` quando disponível. Código zero não prova correção visual; primeira chance não prova crash. A comparação nativa não isola sozinha uma causa e já apresentou imagem deslocada ao mover a janela no ensaio anterior.

### Histórico 0.1 → 0.3

- **0.1/0.2:** a 0.1 podia manter o host oculto após escolher a rede. A 0.2 corrigiu a visibilidade, mas o usuário relatou depois o encerramento do PID histórico `14208` após `Native resize complete`/`host ready`. Sem código final, evento Aplicativo 1000/1001/1002 correspondente ou dump encontrado, crash versus saída normal permanece **indeterminado** ([R2](docs/ANALYSIS.md#r2--encerramento-após-o-setup-da-02-e-diagnóstico-na-03)).
- **0.3:** tamanho fixo diagnóstico por padrão, `--wait` e observadores de saída; **133 PASS, zero falhas**, nas três suítes sintéticas, sem GPU. Preserva quit nativo pendente da UI sem usar quits de workers; os registros críticos usam try-lock de melhor esforço.
- **Ensaio real autorizado da 0.3:** três execuções encerraram normalmente com exit 0. O modo fixo recortou a barra inferior em 125% DPI; o modo nativo sem host deslocou a imagem ao mover; o resize legado demorou cerca de cinco segundos e ainda cortou comandos da direita. Um ciclo de movimento, minimizar/restaurar e Alt+Tab funcionou no modo fixo, sem demonstrar estabilidade prolongada. Esses problemas motivam separar canvas e apresentação na 0.4 ([ANALYSIS.md](docs/ANALYSIS.md), R3). O aviso de 32 bits e uma primeira chance observada não impediram a continuação offline.

**O executável e as DLLs originais não são alterados no disco.** Nenhum componente proprietário é distribuído. Este projeto não é oficial e não modifica licenças, firmware, rede ou processamento de áudio.

> A validação real foi limitada ao ambiente documentado e a uma sessão vazia. Não é garantia universal de GPU/driver, perda de dispositivo, input ou áudio/rede. Não use inicialmente em áudio de produção.

## O que a análise encontrou

- O EXE carrega `Contour.dll` dinamicamente. É essa DLL que importa **DirectDraw**, usa interfaces **DirectDraw/Direct3D 7**, GDI e GDI+.
- Não foi encontrado import normal de `d3d9.dll` ou `opengl32.dll` nos quatro arquivos inspecionados. Isso não exclui todo carregamento dinâmico possível.
- Existem opções internas `windowed` e `nonfullscreen`. O parser e os ramos que usam essas opções foram inspecionados, não apenas as strings.
- A janela gráfica normalmente recebe `WS_POPUP | WS_SYSMENU` (`0x80080000`). O `WM_SIZE` de `ContourParent` refaz superfícies e layout, com teardown global e caminhos de erro que exigem validação; não é uma operação comprovadamente segura.
- Há um caminho nativo de `WM_DISPLAYCHANGE` que pode chamar `PostQuitMessage(0)` mesmo em windowed. Não há evidência de que tenha ocorrido no encerramento observado da 0.2.
- O handler legado de foco tenta `SetFocus(main)` tanto ao ganhar quanto ao perder foco.
- O aviso legado de 32 bits envolve `DDCKEYCAPS_SRCBLTCLRSPACE` (`0x400`), suporte a intervalo de color key em `Blt`, não uma capacidade genérica de renderizar em 32 bpp. A 0.4 não esconde o aviso nem falsifica `GetCaps`.

Veja [docs/ANALYSIS.md](docs/ANALYSIS.md) para hashes, RVAs, evidências, limites e decisões técnicas.

## Como funciona

1. O launcher verifica SHA-256 do EXE e de `Contour.dll`. Builds diferentes são recusados, sem opção de ignorar a verificação.
2. Inicia uma **nova** instância com `windowed nonfullscreen`, diretório de trabalho correto e pausa antes do entry point do EXE. Um `INT3` transitório de um byte é colocado/restaurado **só na memória do filho**; nenhum byte do arquivo é alterado. As DLLs de importação inicial já foram inicializadas nesse momento.
3. Carrega `LakeWindowedFix.dll` e chama sua inicialização **fora de `DllMain`/loader lock**. Só depois libera a thread principal. Não injeta em processos existentes.
4. Redireciona IATs apenas no EXE e em `Contour.dll`, incluindo `DirectDrawCreateEx` no modo escalado; `LoadLibraryA/W` alcança o carregamento posterior de `Contour.dll` e `GetProcAddress` cobre os lookups selecionados.
   O objeto retornado passa pelo proxy completo, sem modificar a vtable nativa. Além das IATs, há a escrita guardada das duas dimensões de layout por RVAs específicos do Build 007, após a seleção de modo; não é uma solução baseada somente em IAT/COM.
5. No padrão escalado, cria a moldura privada redimensionável e o cliente `Lake Controller` como `WS_CHILD` com canvas fixo. A apresentação escala a fonte inteira para o viewport atual, preservando proporção, sem reconstruir o renderer para cada tamanho de janela.
6. Mantém o pedido nativo de visibilidade e o setup agendado por `0x8032`/fallback do EXE. `HostSetupCompleted` continua sendo apenas setup da janela, não teste de saúde gráfica. O resize de `ContourParent` só volta no opt-in legado `NativeResize=1`, que desabilita escala.
7. Impede a recuperação indevida de foco **apenas** quando `WM_KILLFOCUS` sai desse host. `WM_ACTIVATE` e `WM_ACTIVATEAPP` não são falsificados nem descartados globalmente.
8. Neutraliza alterações `ChangeDisplaySettings[A/W/Ex]` vindas dos imports cobertos. Consultas `CDS_TEST` continuam reais.
9. Encaminha o fechamento da moldura ao `WM_CLOSE` original do renderer, respeitando confirmação/recusa e cleanup nativos.
10. Observa os caminhos de saída cobertos e exceções selecionadas sem impedir o comportamento original. Endereço/RVA só é atribuído ao EXE ou a `Contour.dll` quando estiver nas imagens cacheadas; não é uma captura universal de todas as saídas.

**Por que não simplesmente `ddraw.dll` ou `d3d9.dll`?** Mudar só estilos e inventar sucesso de `SetDisplayMode`/`SetCooperativeLevel` pode deixar recursos inconsistentes. O novo bridge usa proxies COM completos dentro de `LakeWindowedFix.dll`, não uma DLL gráfica para autoload. O backend continua sendo o Windows original. `Flip`, `BltFast`, fontes parciais e interfaces/caminhos não observados não se tornam automaticamente escalados.

**Não basta copiar a DLL e abrir o EXE original.** A DLL tem nome próprio, não é carregada automaticamente. Use o launcher.

## Compilar do zero

A 0.6 x86 Zig passou em **oito suítes, 1.159 verificações**, incluindo a política do monitor de recursos; resultados e limites em [PERFORMANCE.md](docs/PERFORMANCE.md). A base 0.5 preservada passou em sete suítes/746 verificações. MSVC/MinGW são alternativas documentadas, não toolchains executadas neste ambiente. Os exemplos abaixo usam **`dist-v0.6`**; preserve as saídas históricas 0.3/0.4/0.5.

### MSVC / Visual Studio 2022

1. Instale **Visual Studio 2022 Build Tools** (ou Visual Studio Community) com o workload **Desenvolvimento para desktop com C++**, MSVC x86/x64, Windows SDK e CMake.
2. Instale Git, obtenha este repositório e abra um **Developer PowerShell for VS 2022** com `cmake.exe` no PATH.
3. Na raiz do projeto:

```powershell
.\scripts\Build.ps1 -Toolchain MSVC -OutputDirectory dist-v0.6
```

O script usa `-A Win32`; a integração inclui cenários Win32, testes separados ScaleGeometry, UiQuality e Presenter, e cálculos PowerShell do monitor. `-OutputDirectory` funciona em todas as toolchains; os exemplos isolam a 0.6 em `dist-v0.6\` (sem o parâmetro, a saída padrão continua `dist\`). O CRT do MSVC é estático (`/MT`). Python e o software Lake **não são necessários para compilar ou rodar os testes sintéticos**.

Equivalente CMake:

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A Win32
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
cmake --install build-msvc --config Release --prefix dist-v0.6
Copy-Item .\config\LakeWindowedFix.ini .\dist-v0.6\LakeWindowedFix.ini
```

O último comando manual substitui a configuração; o script de build a preserva. Confira `ScaleRendering=1`, `NativeResize=0` e as novas seções antes do reteste: um INI antigo não é migrado automaticamente. Se sua política de scripts bloquear arquivos baixados, revise-os e desbloqueie apenas esses arquivos conforme a política local; não é necessário mudar a política global da máquina.

### MinGW-w64 x86

Com CMake, Ninja e compilador **i686** disponíveis no PATH:

```powershell
.\scripts\Build.ps1 -Toolchain MinGW -MinGWCompiler i686-w64-mingw32-g++.exe -OutputDirectory dist-v0.6
```

Uma toolchain exclusivamente x64 não serve. Não misture DLL x64 com o processo Lake x86. Não instale DLLs deste projeto em `System32`/`SysWOW64`.

### Zig portátil (alternativa)

O script aceita Zig **0.14.1** já extraído; não faz downloads nem modifica PATH/configuração global:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:\tools\zig\zig.exe -OutputDirectory dist-v0.6
```

Também é possível obter a distribuição Python desse compilador em uma pasta local ignorada, explicitamente:

```powershell
python -m pip install --target .\build-tools\python ziglang==0.14.1
.\scripts\Build.ps1 -Toolchain Zig -ZigPath .\build-tools\python\ziglang\zig.exe -OutputDirectory dist-v0.6
```

Esse comando baixa uma toolchain de terceiros; é opcional. Consulte [docs/VALIDATION.md](docs/VALIDATION.md) para o que foi efetivamente compilado/testado.

## Usar sem instalar (recomendado para o primeiro teste)

1. Feche outras instâncias do Lake. Faça backup dos seus presets/configurações pelos mecanismos normais do software.
2. Com o build 0.6, em ambiente de teste sem processadores de áudio em produção, execute:

```powershell
.\dist-v0.6\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

O launcher, a DLL e o INI devem permanecer juntos em `dist-v0.6\`; não é necessário escrever na pasta do Lake. O próprio Lake ainda pode precisar das permissões e dependências originais da instalação.

3. Confira interface inteira, barra inferior/comandos da direita, movimento, resize/maximizar, mouse, Alt+Tab e minimizar/restaurar seguindo [a matriz](docs/VALIDATION.md). Não habilite `NativeResize=1` para obter resize no padrão novo: isso desliga a escala e volta ao rebuild legado.
4. Para voltar ao comportamento original, feche o Lake e inicie `Lake Controller.exe` diretamente. Os hooks só existem no novo processo lançado pelo patch.

### Instalação opcional ao lado do EXE

Se desejar manter tudo na pasta do Lake, copie **somente**:

```text
LakeWindowedLauncher.exe
LakeWindowedFix.dll
LakeWindowedFix.ini
```

Inicie `LakeWindowedLauncher.exe`, não o EXE original. Não substitua `Contour.dll`, `ddraw.dll`, `user32.dll` ou qualquer DLL do fabricante.

Há também um instalador conservador que verifica hashes, recusa sobrescrever arquivos existentes e grava um manifesto dos seus próprios arquivos:

```powershell
.\scripts\Install.ps1 -AppDirectory "C:\Program Files (x86)\Lake\Lake Controller v5.8" -SourceDirectory dist-v0.6
```

O exemplo seleciona explicitamente a compilação isolada nova com `-SourceDirectory dist-v0.6`. Os mesmos três nomes de arquivo são usados; não instale enquanto houver uma instância do Lake aberta. O monitor pode ser executado separadamente da distribuição; o instalador continua copiando somente os três arquivos de runtime.

Escrever em `Program Files` normalmente exige PowerShell elevado. O script não se autoeleva. Para remover uma instalação feita por esse script:

```powershell
.\scripts\Uninstall.ps1 -AppDirectory "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

O desinstalador confere hashes antes de remover arquivos; preserva um INI editado. Na instalação manual, feche o Lake e remova apenas os três arquivos acima.

## Configuração

Edite `LakeWindowedFix.ini` **ao lado da DLL**, não necessariamente na pasta do Lake:

```ini
[window]
Width=1024
Height=576
MinWidth=800
MinHeight=450

[compatibility]
ScaleRendering=1
NativeResize=0

[render]
Width=1088
Height=612
SmoothFonts=1

[input]
SystemCursor=1
```

| Modo | Flags | Comportamento |
| --- | --- | --- |
| Escalado, padrão 0.4/0.5/0.6 | `ScaleRendering=1`, `NativeResize=0` | Moldura redimensionável/maximizável; canvas fixo; sem rebuild nativo pelo resize do host |
| Fixo diagnóstico, legado 0.3 | `ScaleRendering=0`, `NativeResize=0` | Framebuffer original; sem resize/maximizar; `[window]` não ajusta o tamanho; pode haver recorte |
| Rebuild nativo, EXPERIMENTAL | `NativeResize=1` | Tem precedência e desabilita a escala; recria recursos e pode continuar cortando a UI |

No modo escalado, **`[window]` controla só a janela**: padrão inicial 1024×576 e mínimo interativo padrão 800×450 na 0.6. `MinWidth`/`MinHeight` são configuráveis; `320/180` restaura o limite pequeno anterior. Tamanho inicial e mínimo são ajustados à área útil quando necessário. São pixels lógicos, não dimensões físicas garantidas em DPI alto. Valores fora de 320–8192 na largura ou 180–8192 na altura voltam ao padrão de cada opção; detalhes em [PERFORMANCE.md](docs/PERFORMANCE.md).

**`[render]` controla o canvas virtual fixo**: padrão **1088×612**, largura aceita **1024–4096**, altura **600–4096**. Fora dessas faixas, volta a 1088/612. Trocar o canvas exige fechar/reabrir o Lake; não é resize em tempo real. O presenter consulta a superfície fonte atual. Uma janela menor escala a interface inteira, mas não torna o texto legível em qualquer tamanho mínimo.

Com `NativeResize=1`, o mínimo legado é 800×600; use, por exemplo, `[window] Width=1024` e `Height=768` para comparar. A altura 576 do INI novo não satisfaz esse mínimo e cai no fallback legado de 768. Não há promessa de ausência de travamento, tela preta, recorte ou latência. Feche o aplicativo antes de mudar o INI e teste cada modo separadamente, fora de produção.

Para comparar somente as flags internas, **sem bordas/host/hooks**, há um diagnóstico:

```powershell
.\dist-v0.6\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only --wait
# Ou, sem compilar:
.\scripts\Start-NativeWindowed.ps1 -AppDirectory "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

Esses comandos executam o Lake real, incluindo seus comportamentos originais de rede/configuração. Não são testes automatizados seguros para um sistema ao vivo. O script alternativo não fornece o novo relatório `--wait` do launcher.

## Limites e segurança operacional

- Apenas os hashes documentados são aceitos. O nome “5.8” sozinho não garante equivalência de binários.
- O seed de layout escreve **dois `LONG` de dados em memória**, por RVAs do Build 007, após guardas; não é genérico para outros builds. Não virtualiza `GetDeviceCaps` nem altera código executável para dimensionar o layout. Foi observado no ensaio final com o canvas padrão.
- A DLL não implementa recuperação universal de `DDERR_SURFACELOST`, mudanças de monitor, RDP, hotplug ou profundidade de cor. O aviso legado de 32 bits continua visível; HRESULTs DirectDraw não são convertidos em sucesso.
- A escala e o mouse foram operados no Lake a 125% DPI, inclusive nos menus e diálogos citados. Windows 10, outros drivers, DPI misto, touch/IME, edição de texto e todos os demais controles não foram validados.
- Sem patch global de User32/DirectDraw, serviço, driver, registro, AppInit_DLLs ou injeção em terceiros.
- Hooks IAT de outros programas não são sobrescritos deliberadamente. Não combine inicialmente com DxWnd, overlays ou outros wrappers.
- Chamadas por outros módulos, ordinais, delay imports e `LoadLibraryEx` não estão cobertas. O caminho observado usa imports normais e `LoadLibraryA`.
- A inicialização anterior ao entry point é transitória; não mantém o aplicativo sob um debugger. Veja os detalhes em `docs/ANALYSIS.md`.
- Logs vão para `logs\LakeWindowedFix-<PID>.log` ao lado da DLL (fallback em `%TEMP%\LakeWindowedFix`). Registros comuns também usam `OutputDebugString`; observadores de saída/exceção não o usam e podem omitir linhas se o lock estiver ocupado. A escrita em arquivo continua síncrona. Com `--wait`, o launcher acrescenta `LakeWindowedFix-<PID>-exit.log`. Incluem PID/thread, etapas, mensagens e endereços/RVAs diagnósticos; não há coleta intencional de nomes de adaptadores, IPs, presets ou credenciais. Revise qualquer trecho antes de compartilhá-lo; não versione logs/dumps nem originais. A DLL fica carregada até o processo terminar; não tente descarregá-la a quente.
- O software original pode alterar arquivos ou adaptadores de rede por conta própria. Este projeto não neutraliza essas funções.

## Auditoria e contribuição

```text
src/                    DLL, launcher, políticas Win32 e exports x86
src/ddraw_scale.cpp     bridge de apresentação DD7 por objeto
src/scale_geometry.h    viewport proporcional e coordenadas
scripts/                build, instalação/remoção e inspeção offline
tests/fixture.cpp       aplicativo sintético, sem código proprietário
tests/Run-Fixture.ps1   cópias/configurações sintéticas isoladas por execução
config/                 configuração inicial
docs/                   evidências, limitações e roteiro de validação
```

Para reproduzir a inspeção sem executar o Lake:

```powershell
python -m venv build-analysis
.\build-analysis\Scripts\python.exe -m pip install -r .\scripts\requirements-analysis.txt
.\build-analysis\Scripts\python.exe .\scripts\Inspect-Target.py --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --disassemble
```

O relatório fica em `private\inspection.json`, ignorado pelo Git. É evidência local de pefile/Capstone, não saída de um descompilador REA. Não publique dumps, recursos, binários ou o relatório privado integral. Consulte [CONTRIBUTING.md](CONTRIBUTING.md).

Licença [MIT](LICENSE) para o código deste projeto. Marcas e binários Lake/Dolby pertencem aos respectivos titulares; a licença do projeto não os abrange.
