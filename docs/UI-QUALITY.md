# Qualidade da UI — 0.5 em teste

**Candidata experimental, com validação limitada.** Mantém a geometria da 0.4: canvas **1088×612** e janela inicial **1024×576 pixels lógicos**. As duas opções novas aplicam-se somente ao modo escalado, com `ScaleRendering=1` e `NativeResize=0`.

O ensaio isolado de **16 bpp apresentou artefatos persistentes**; o padrão permanece **32 bits de cor, no ambiente original**, sem opção pública para forçar 16 bpp.

## Configuração

Trecho do `LakeWindowedFix.ini`, a ajustar com o aplicativo fechado:

```ini
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

`SmoothFonts=0` preserva a qualidade de fonte solicitada originalmente. `SystemCursor=0` restaura os cursores bitmap originais, incluindo suas formas especializadas.

## Fontes: melhoria parcial, sem blur geral

- Nos pedidos GDI `CreateFontIndirectA` originados em `Contour.dll` para **Arial** ou **Courier New**, usa uma cópia do `LOGFONTA` com apenas `lfQuality=ANTIALIASED_QUALITY` (**4**). Todos os demais campos são preservados; a estrutura de entrada e os arquivos originais não são alterados.
- Aplica antialiasing em escala de cinza na rasterização de origem, não um filtro de blur sobre toda a interface. Não substitui fontes bitmap, `Small Fonts` ou LCD.
- A apresentação continua escalando um bitmap: as fontes não passam a ser vetoriais na tela, e nem todos os degraus do upscale desaparecem.

## Cursor: seta do Windows independente da apresentação

- `SystemCursor=1` usa a seta padrão do Windows no canvas, independente da cadência de apresentação. Substitui os sprites e as indicações visuais especializadas do cursor do canvas; bordas de resize e diálogos Win32 próprios mantêm seu tratamento nativo.
- A supressão dos retângulos de cursor limita-se aos três retornos de `MapWindowPoints` do **Build 007**: RVAs `0x283fb`, `0x2a453` e `0x2a4e2`. Exige mapeamento tela → canvas, dois pontos e largura/altura positivas de até **512** cada. Os retângulos são deslocados para fora do canvas para recorte pelo desenho original, sem suprimir mapeamentos genéricos.
- Mantém o roteamento de input/cliques da 0.4 e não cria timer de redraw. Dentro do canvas, conserva o cursor bitmap como fallback se não confirmar a seta do sistema visível.
- Trata `ShowCursor` como contador, não como booleano: registra apenas seus próprios incrementos em créditos locais à thread, limitados a **16** pendentes. Restitui esses créditos ao perder foco para outra aplicação, sair do host ou destruí-lo; não normaliza o contador de outra aplicação.
- Em separado, subclasses de HWNDs filhos cobrem `WM_SETCURSOR` na área cliente dos controles, incluindo filhos existentes, futuros A/W e criados durante `WM_CREATE`. Mensagens não cobertas seguem o tratamento nativo; a subclass é removida na destruição. Essa cobertura, sozinha, não corrigia a normalização contínua do contador.
- O novo `HookShowCursor` virtualiza **somente os quatro retornos do Build 007** `0x11d76`, `0x11d84`, `0x6465d` e `0x6466a`, com identidade do renderer confirmada, cliente anexado, thread de UI proprietária e modo `SystemCursor` ativo. O contador lógico é local à thread: inicializa uma única vez com um par real `ShowCursor(TRUE)`/`ShowCursor(FALSE)` balanceado, desconta os créditos próprios e usa aritmética saturada. As normalizações seguintes ajustam apenas esse contador lógico, sem esgotar a visibilidade real.
- No caminho virtual, preserva o `LastError` de entrada; as demais chamadas continuam nativas, com retorno e `LastError` da API real. **Virtualizar esse contador legado não é medir a visibilidade real nem inventar um HRESULT gráfico:** os retornos DirectDraw/COM continuam reais.

## Observações concluídas até agora

Ambiente observado: **Windows 11 `10.0.26300` x64, DPI 125%**. Os tempos de apresentação abaixo são amostras instrumentadas anteriores, não benchmarks de GPU/áudio, universais ou prova de causalidade.

### Antes da pausa: runtime parcial sem Frida

O **PID `4188`**, com o build de **746 PASS**, rodou em **32 bpp, OFFLINE**, sem Frida durante as medições. O aviso foi confirmado, foi escolhido **No** na pergunta Recall e aberto **Modules**, com canvas vazio.

| Trajetória | Cursor do sistema visível | Divergências de posição |
| --- | --- | --- |
| Startup/barra | **240/240** | **0** |
| Modules/barra, após startup | **240/240** | **0** |
| Modules/canvas | **240/240** | **0** |

Cada trajetória durou **2 s a 120 Hz**, com **120 posições distintas** de ida/volta. A visibilidade foi medida por **`GetCursorInfo`**, não apenas por screenshot ou `GetCursor`. Registros privados: `cursor-final-motion-startup-bar.json`, `cursor-final-motion-modules-bar.json` e `cursor-final-motion-modules-canvas.json`. Não é avaliação humana subjetiva de latência nem aprovação da interface inteira.

A tentativa posterior de Home foi recusada pelo operador porque o ponto não pertencia à instância; **nenhum clique foi enviado**, e resize/foco não foram testados nessa sessão antes da pausa. A causa de o ponto estar fora não foi estabelecida. Ao pedido de pausa do usuário, foi enviado `WM_CLOSE` normal somente ao host vinculado; o launcher confirmou **exit 0 (`0x00000000`)**, em `LakeWindowedFix-4188-exit.log`. Isso confirma esse encerramento, não estabilidade prolongada.

O observador opcional `cursor-final-observe.js` foi preparado, mas **não foi carregado/executado**. Não há confirmação dinâmica adicional do hook por esse observador; as três medições acima são sem instrumentação Frida.

### Reteste após a retomada — mesmo build, PID `11408`

As fontes, DLL/launcher/INI e os dez arquivos históricos preservados foram conferidos contra o checkpoint antes de iniciar. **Não foi necessário alterar ou recompilar o patch.** A nova instância rodou em **32 bpp/OFFLINE**, com aviso → **No** → Modules/Home e canvas vazio, sem configurar adaptadores/processadores ou salvar/recuperar presets.

Os primeiros dois probes deram **0/240** amostras visíveis. O diagnóstico posterior, inclusive após `WM_SETCURSOR` no main, encontrou **`GetCursorInfo.flags=2` (`CURSOR_SUPPRESSED`)**, distinto de `flags=0` (oculto). Um evento de movimento de mouse dentro da janela mudou o estado para **1**, com a seta Windows `0x10003`, sem ajuste de `ShowCursor` ou alteração dos binários. Os primeiros probes não armazenavam os flags completos; não se afirma que cada uma de suas amostras tinha necessariamente o mesmo estado. O operador privado foi melhorado para registrar os flags e a supressão separadamente.

Com modo de mouse ativo, foram medidas as seguintes trajetórias de **2 s a 120 Hz**, cada uma com **120 posições distintas** e **zero divergências de posição**:

| Caso | Cursor visível | Observação |
| --- | --- | --- |
| Modules/barra e canvas, cliente físico 1280×720 | **240/240 cada** | Repetição do caso que falhava antes do hook |
| Primeira trajetória na barra após resize para 800×600 | **238/240** | Duas amostras com flags=0 aos aproximadamente 17 e 25 ms; causa não estabelecida |
| Repetição da barra e canvas em 800×600 | **240/240 cada** | Sem supressão do sistema nas amostras |
| Margens superior e inferior do letterbox | **240/240 cada** | Movimentos no host, sem cliques nas margens |
| Modules/barra no viewport menor | **240/240** | Cliques de Home/Modules também operados nessa geometria |
| Barra após Alt+Tab e retorno | **240/240** | Foco saiu do Lake; sem captura ou digitação na outra aplicação |
| Barra após minimizar/restaurar | **240/240** | Estados minimizado/restaurado confirmados |
| Barra após retornar a 1280×720 | **240/240** | Interface completa e canvas vazio na captura final |

O resize para **800×600** respondeu, preservou proporção e mostrou margens pretas. O ensaio confirma esses casos, **não ausência de transições breves**: a primeira trajetória de resize não passou em 240/240 e seu resultado não foi descartado. Reposicionar com `SetCursorPos` não equivale a medir input físico ou latência percebida; a leitura imediata também não comprova dispatch de cada mensagem pela UI.

Os registros `resume-01/motion-*.json`, ações e capturas são privados. Essa rodada usou Win32/Python, **sem Frida/REA conectado**, e não produz novos IDs Evidence REA nem confirmação dinâmica adicional dos quatro retornos virtualizados. O observador opcional continua não executado.

O host recebeu fechamento normal pelo agente, sem salvar configuração ou forçar o processo. O launcher confirmou **exit 0 (`0x00000000`)**, registrado em `LakeWindowedFix-11408-exit.log`. O log também registrou duas exceções de primeira chance `0xc0000005` fora das imagens cacheadas; a UI continuou funcionando. Sem atribuição de módulo/causa confirmada, isso não prova crash nem permite alegar execução sem exceções.

### Histórico: aprovação localizada não é aprovação da UI inteira

| Registro | Resultado observado e limite |
| --- | --- |
| Baseline 0.4, PID `12260`, OFFLINE | `GetCursorInfo`: `flags=0`, `hCursor=0`. Amostra após attach: intervalo entre `Blt`, mediana **52 ms**, p95 **65 ms**. |
| Candidata anterior 0.5, PID `4440`, movimento | **240/240** amostras visíveis em **2 s a 120 Hz**, **120 posições distintas**, **0 divergências**. Aprovação limitada à trajetória medida, não à barra após Modules nem à interface inteira. |
| Captura `BitBlt` do aviso | Sem ponteiro azul no canvas; a captura, isoladamente, não comprova a visibilidade do cursor do Windows. |
| Build anterior de **714 PASS**, PID `2556` | Barra no aviso inicial: **240/240** visíveis; após No/Modules: **0/240**, sem divergências de posição. Subclasses dos filhos não bastaram; esse candidato não aprovou o mouse na UI inteira. |

O trace nativo `ev_cba8efcf45c66f4edbf4fa8cd58a9fb5c283d582c326e765361ba795b8ae0614` identificou a sequência contínua `ShowCursor(TRUE) -> 0` / `ShowCursor(FALSE) -> -1` nos retornos `0x11d76`/`0x11d84`. A inspeção estática da rotina `0x11d30` mostrou guarda temporal de **50 ms** e normalização até **-1**, esgotando os créditos limitados da candidata; não é medição exata de cada intervalo dinâmico. O isolamento no build de **746 PASS** resolveu **esse caso medido** na barra após Modules, sem extrapolar para os casos ainda pendentes.

A amostra instrumentada anterior em **32 bpp**, PID `4440`, registrou **3.051 apresentações**, mediana **52 ms**, p95 **64 ms**, com pedidos de fonte de qualidade **4** observados: `ev_6177a305ccc589c3da12168db658b2d9d08eccfbed144e0394a0cecd7a299485`. Houve interação posterior do usuário; não se presume sessão inteira vazia ou sem interação, nem cobertura de casos não registrados.

### Formatos e aviso de vídeo

Evidence `ev_605b13eb74deba788313ac62b924aa559a141dd93b7c04c3c5eaf7ebb6638906`: primária **1920×1080 RGB32**, fonte **1088×612 RGB32** e layout **1088×612**.

O ambiente do Lake já continha `__COMPAT_LAYER='ElevateCreateProcess DWM8And16BitMitigation'`; isso não estabelece a causa do aviso. A verificação do aviso de 32 bpp consulta **`DDCKEYCAPS_SRCBLTCLRSPACE` (`0x400`)**, não uma incapacidade geral de trabalhar em 32 bits, e não prova lentidão.

### Ensaio isolado de 16 bpp

O processo próprio **PID `4752`** usou os mesmos binários/INI da candidata anterior de 32 bpp (PID `4440`), não o último build de 746 PASS, acrescentando `16BITCOLOR` a `__COMPAT_LAYER` **somente no ambiente do shell/launcher filho**, preservando as demais camadas. A variável foi restaurada; não houve alteração de registro nem de configuração global de vídeo.

Evidence de formatos `ev_8ea169b63cf6b4360906014dff56fd40d004adce6d532f1754bb2d5539783dd1`: camada efetiva `16BitColor ElevateCreateProcess DWM8And16BitMitigation`; `GetDeviceCaps` indicou **16 bpp**; primária **1920×1080 RGB16** e fonte **1088×612 RGB16**, com máscaras **`0xf800 / 0x07e0 / 0x001f`**; layout **1088×612**. O aviso de 32 bpp não apareceu: o startup chegou diretamente à pergunta **Recall**.

A evidência final `ev_b605c512e3e4bd8f17fdd607312fcaa370cfc50fc2dc5ea1222c4e92010f811f`, exportada em `color16-evidence.json`, observou **2.635 apresentações**, mediana **52 ms**, p95 **64 ms**, iguais aos valores da amostra em 32 bpp (**52/64 ms**). Não é benchmark controlado ou prolongado com cenas idênticas: as interfaces estavam em estágios diferentes, e a comparação não demonstra ganho de desempenho.

As capturas privadas `color16-startup.png`, `color16-after-format-query.png` e `color16-home.png` mostraram **faixas diagonais nos botões, lacunas nas letras e cursor bitmap corrompido**. O primeiro retângulo cinza era um segmento transitório que foi repintado, mas os demais artefatos persistiram. Foi escolhido **No**, sem recall, e o setup foi concluído. O processo encerrou com **exit 0**; o iniciador do encerramento não foi estabelecido. O achado limita-se à candidata 0.5 no Windows 11 observado; não determina a causa no backend/renderer nem demonstra que 16 bpp falhe sempre.

**Conclusão:** ensaio encerrado, sem aprovar a qualidade visual de 16 bpp nem demonstrar ganho de cadência. Manter **32 bits de cor** no ambiente original, **sem flag pública de 16 bpp**. Depois do ensaio foram verificados desktop em 32 bpp, ambiente pai sem `16BITCOLOR` e cursor OS visível. Profundidade de cor não altera a arquitetura **x86** do processo.

## Build, pendências e artefatos

- Último registro, `private/font-cursor-v05/build-final-05.txt`: **746 PASS, zero FAIL, zero warnings, sete suítes**. Contagem: **119** geometria + **77** qualidade de UI + **75** COM + **65 e 63** cenários legados + **5** saída legada + **338** modo escalado + **4** launcher. Inclui **1.000 normalizações lógicas** sem novas chamadas reais após o probe inicial; testes sintéticos não validam toda a interface proprietária ou a GPU.
- **Casos previstos para a retomada executados no build de 746 PASS:** Home/Modules, resize **800×600** com letterbox e retorno a 1280×720, barra menor/margens, **Alt+Tab/retorno** e **minimizar/restaurar**. Permanecem os limites e o resultado transitório de 238/240 acima; isso não é aprovação universal da interface.
- Sem validação de áudio/rede/hardware, outros drivers/DPI, Windows 10, touch/IME/acessibilidade, edição de texto, todos os cursores de ferramentas, perda de dispositivo ou estabilidade prolongada. Não há baseline de desempenho de GPU/áudio que permita alegar ganho nessas áreas.
- Saída local em **`dist-v0.5/`**, separada de `dist-v0.4/` e `dist-v0.3/`. Os hashes dos **dez arquivos preservados**, incluindo os quatro originais e as distribuições históricas, foram novamente conferidos antes da rodada da retomada.
- Evidências, logs, capturas e anexos de hashes permanecem privados/ignorados; nenhuma cópia proprietária acompanha este documento. Somente código, testes, configuração e documentação públicos são versionados.

## Referências

- [Microsoft: CURSORINFO](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-cursorinfo): distingue `CURSOR_SHOWING=1`, oculto=0 e `CURSOR_SUPPRESSED=2`, estado em que o sistema não desenha o cursor por input de toque/caneta. O patch não promete contrariar essa política do Windows; o fallback bitmap pode reaparecer quando a seta não está visível.

- [Microsoft: DWM sempre ativo](https://learn.microsoft.com/en-us/windows/win32/w8cookbook/desktop-window-manager-is-always-on): a compatibilidade pode virtualizar 8/16 bits para o aplicativo, mantendo o desktop em 32 bpp.
- [ChangeDisplaySettingsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsexa): restrições de modos abaixo de 32 bpp para manifestos Windows 8+, distintas da possibilidade de criar superfícies de 16 bits.
- [Catálogo de correções de compatibilidade](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/deployment/compatibility/compatibility-fixes-for-windows-8-windows-7-and-windows-vista): descreve `DWM8And16BitMitigation`; não garante aceleração ou imagem correta neste backend.

Essas fontes não constituem um contrato público da sintaxe `__COMPAT_LAYER=16BITCOLOR` no Windows 11. Neste ensaio, a confirmação veio das descrições reais das superfícies; a camada não foi incorporada como configuração pública do patch.
