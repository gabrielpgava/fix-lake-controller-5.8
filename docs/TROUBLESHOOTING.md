# Diagnóstico

## 0.6: mínimo configurável e recursos

A candidata 0.6 usa `dist-v0.6\` e acrescenta `[window] MinWidth=800` / `MinHeight=450` no modo escalado. São mínimos em pixels lógicos; `320/180` restaura o limite pequeno anterior. A 125% DPI, o padrão resultou em cliente mínimo 1000×562 físico. O Windows também pode limitar pedidos programáticos menores; não confunda pedido e tamanho efetivamente observado. Não altera o canvas nem pede rebuild do renderer. INIs existentes são preservados pelo build e não são migrados automaticamente. O monitor PowerShell somente-leitura, resultados e protocolo de comparação estão em [PERFORMANCE.md](PERFORMANCE.md); crescimento curto não confirma vazamento ou sua origem.

`SystemCursor=1` só suprime o bitmap após confirmar a seta nativa visível. Em um estado real da 0.6, `GetCursorInfo` permaneceu com flags=0/handle nulo e o bitmap continuou ativo, inclusive após movimentos sintéticos; esse caso não valida a seta independente. Execuções novas da mesma 0.6, com mínimo padrão e opt-out, tiveram 240/240 amostras visíveis após resize, como o controle 0.5; a causa do primeiro estado não foi determinada. Flags=0 (oculto) e `CURSOR_SUPPRESSED=2` (política de toque/caneta) são distintos. Não force contadores de `ShowCursor` ou políticas globais para ocultar o problema; compare uma execução nova e a base preservada antes de atribuir causa.

## 0.5: texto e movimento do cursor

A revisão 0.5 usa `dist-v0.5\` e acrescenta `[render] SmoothFonts=1` e `[input] SystemCursor=1`, somente no modo escalado. O texto recebe antialiasing na origem, não blur geral; o cursor passa a ser a seta do Windows, independente da cadência do renderer. Para comparar com o comportamento anterior, use `SmoothFonts=0` / `SystemCursor=0` com o Lake fechado. Fontes bitmap e formas especializadas do cursor têm limites explícitos em [UI-QUALITY.md](UI-QUALITY.md).

Não reduza a cor global do Windows para esconder o aviso de 32 bits: o teste isolado em 16 bpp realmente mudou as superfícies para RGB565, mas apresentou corrupção de texto/botões, sem ganho demonstrado no intervalo entre frames. Essa mensagem verifica uma capacidade específica de color-key, não a causa comprovada de lentidão. A arquitetura do processo continua x86 em ambos os modos de cor.

Se a seta desaparecer depois de usar toque/caneta, distinga os flags de `GetCursorInfo`: **0** é oculto, **1** é visível e **2** é `CURSOR_SUPPRESSED`, supressão pelo próprio Windows. Na retomada, um evento de movimento de mouse restabeleceu a seta, sem alterar o patch; apenas reposicionar com `SetCursorPos` não fez isso. Não normalize `ShowCursor` indefinidamente nem altere a configuração global de input como contorno. O bitmap pode reaparecer como fallback enquanto a seta não está visível. Houve também duas amostras ocultas transitórias no primeiro percurso após resize; limites e resultados completos em [UI-QUALITY.md](UI-QUALITY.md).

## 0.4: janela escalada e canvas menor

O build final foi operado em **OFFLINE no Windows 11, a 125% DPI**, com menus, resize, maximizar/restaurar, minimizar/restaurar e Alt+Tab. Passou em **648 verificações sintéticas, zero falhas**. Isso não garante estabilidade em outros drivers/DPI, Windows 10 ou áudio em produção; cobertura e pendências em [VALIDATION.md](VALIDATION.md).

Use **`dist-v0.4\`**, preservando `dist-v0.3\`. Não renomeie binários antigos para simular a revisão. Mantenha launcher/DLL/INI da mesma versão e confira o padrão:

```ini
[window]
Width=1024
Height=576

[compatibility]
ScaleRendering=1
NativeResize=0

[render]
Width=1088
Height=612
```

Esse padrão usa a moldura privada **`LakeWindowedFix.ScaledHost.v1`**, com resize/maximizar/minimizar e canvas virtual fixo. `[window]` controla a janela inicial 1024×576 lógica, ajustada à área útil, com mínimo interativo 320×180. `[render]` controla o canvas, padrão **1088×612**, cerca de 29% menor por dimensão que o candidato 1536×864 para ampliar os controles na mesma janela. O canvas não muda ao redimensionar/restaurar. O shim não encaminha esse resize ao rebuild de `ContourParent`; o presenter adapta o `Blt` da primária com fonte inteira, preservando proporção e letterbox preto.

Com outras instâncias fechadas, comece em OFFLINE e fora de produção:

```powershell
.\dist-v0.4\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

Mantenha o console aberto; `--wait` aguarda o aplicativo encerrar, consulta `GetExitCodeProcess` e imprime/grava decimal/hexadecimal em `dist-v0.4\logs\LakeWindowedFix-<PID>-exit.log`. Na espera normal de produção não há timeout que mate o Lake. Isso é separado dos timeouts de inicialização e dos testes sintéticos. Código zero sem abrir a UI não é sucesso.

Se continuar fechando, faça uma comparação **separada**, somente com as flags internas e sem DLL/host/hooks:

```powershell
.\dist-v0.4\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only --wait
```

Esse modo gera o relatório de saída pelo launcher, mas não o log interno da DLL. No ensaio anterior sem host, a imagem deixou de acompanhar a janela ao movê-la: somente as flags nativas não garantem uma janela móvel correta. Também executa os comportamentos reais de rede/configuração do Lake; não é um teste automatizado seguro para áudio ao vivo. Diferenças são pistas, não confirmação automática de causa. Não conecte hardware, altere adaptadores ou use Save/Recall para um diagnóstico gráfico.

## Selecionou a interface de rede e a tela principal não apareceu

Foi identificada uma falha da **0.1**: o cliente podia receber `WS_VISIBLE`, mas a moldura continuava oculta. A correção de visibilidade entrou na **0.2** e foi mantida nas versões seguintes. Ela não equivale a corrigir o encerramento posterior observado com a 0.2 nem a validar a escala 0.4.

Antes de tentar novamente, feche a instância antiga. Se não houver janela, confira `Lake Controller.exe` em **Gerenciador de Tarefas → Detalhes**. Finalizar um processo pode perder trabalho não salvo; faça isso somente na instância de teste sem dados a preservar. O launcher novo não encerra processos existentes e recusa criar outro enquanto encontrar um Lake aberto (exit 4).

Não execute o launcher repetidamente para tentar “destravar”: o software original também usa um mutex e uma segunda instância pode apenas sair. A mensagem “processo criado/DLL inicializada” não significa que o startup gráfico terminou.

Os logs internos ficam em `logs\LakeWindowedFix-<PID>.log` ao lado da DLL, com fallback em `%TEMP%\LakeWindowedFix\` quando disponível. O relatório `-exit.log` fica em `logs\` ao lado do launcher; tenta o fallback temporário também após erro de abertura/escrita local, mesmo se `logs` já existir. Procure:

- `Native ShowWindow ... host-visible=1`: pedido de visibilidade replicado.
- `Ready queued after native 0x8032 notification` ou `Ready queued by EXE GetMessage fallback`: setup agendado.
- `ScaleRendering=1: resizable scaled presentation; fixed virtual canvas; no native graphics rebuild`: política nova selecionada. A linha antiga `NativeResize=0: original framebuffer size retained...` indica o modo fixo **sem escala**, não o padrão novo.
- `Original client size=...` e `Scaled presentation active source=... viewport=[...]`: compare canvas/descrição real da fonte e destino; “active” é um retorno bem-sucedido observado, não valida toda a UI/input. O diagnóstico é de melhor esforço.
- `Scaled presentation bypassed: ...` ou `Scaled presentation first failure ... HRESULT=...`: houve bypass ou erro real; não o interprete como apresentação escalada bem-sucedida.
- `Virtual canvas layout seeded after native adapter/mode selection`: os dois dados de layout passaram pelas guardas e receberam o canvas. Não confirma saúde gráfica nem toda a UI. `Virtual canvas layout unavailable: writable build-007 dimension guard failed` indica recusa da escrita; não remova guardas como contorno. A falha COM dos startups intermediários ocorreu antes desse seed.
- `Primary COLORFILL unavailable: owned-host GDI letterbox fallback active; presentation HRESULT remains real`: o fill nativo foi recusado e as faixas usam GDI restrito ao host; isso não é falha da apresentação principal por si só.
- `Host close deferred until native startup dialogs complete`: fechamento pelo X adiado durante os modais nativos. Conclua os diálogos; não force a destruição gráfica durante o startup.
- `Initial placement complete WITHOUT WM_SIZE or framebuffer size change`: posicionamento sem rebuild inicial, tanto no modo escalado quanto no diagnóstico fixo.
- `Host setup complete (not a renderer health check)`: apenas setup da janela. A propriedade `LakeWindowedFix.HostSetupCompleted` tem esse mesmo limite.
- `Native resize begin`/`Native resize complete`: esperados somente com `NativeResize=1` no resize do host. Se aparecerem num reteste que deveria ser escalado, confira o INI e a versão da DLL. “Complete” significa retorno da chamada, não sucesso gráfico comprovado.

Para um diagnóstico passivo, sem injeção adicional nem alterações de rede:

```powershell
python .\scripts\Inspect-WindowState.py --output .\private\window-state.json
```

O script consulta nomes de processos e estado de janelas no desktop atual. Não abre memória de processo nem lê os nomes/IPs dos adaptadores.

## O processo fechou após abrir ou escolher a rede

No caso histórico **0.2, PID 14208**, o log `dist-v0.2/logs/LakeWindowedFix-14208.log` chegou a `Native ShowWindow cmd=1/5`, `Initial placement begin`, `Native resize begin/complete`, `Initial placement complete` e `Native windowed host ready`. O processo depois fechou. Não foi encontrado evento Aplicativo 1000/1001/1002 correspondente nem dump. **Não há causa confirmada e não sabemos se foi crash ou saída normal.**

A inspeção estática encontrou riscos no rebuild e um ramo nativo de `WM_DISPLAYCHANGE` que pode chamar `PostQuitMessage(0)` mesmo em windowed; não estabelece relação causal com esse encerramento. Veja [ANALYSIS.md, R2](ANALYSIS.md#r2--encerramento-após-o-setup-da-02-e-diagnóstico-na-03).

O ensaio real autorizado posterior da **0.3** teve três saídas normais com código 0, mas reproduziu problemas visuais; não explica o PID 14208 nem autoriza atribuir todos os fechamentos à GPU. O usuário também relatou continuação offline/rede funcionando, sem validação de hardware.

Na **0.4 intermediária**, três startups saíram com código 0 antes da UI. O probe reproduziu a incompatibilidade de trocar a vtable nativa: QI D3D7 passou de 0 para `0x80004002`, com cooperação `0x80004005`. O build final usa **proxies próprios completos**, sem alterar `lpVtbl` nativo, e o probe revisado confirmou QI D3D7/cooperação com HRESULT 0. O ensaio final abriu a UI e encerrou por Quit/Yes, exit 0. Não force HRESULT de sucesso para atravessar falhas.

Se houver nova falha, registre **as duas flags** e use `--wait`. Correlacione PID/horário e, no log interno, procure:

- `Host`/`Client WM_CLOSE`, `WM_DESTROY` e `WM_NCDESTROY`: sequência de fechamento/destruição, sem concluir por ela quem iniciou tudo.
- `Client WM_DISPLAYCHANGE ... (forwarded unchanged)`: mensagem entregue ao original; o shim **não a descarta**.
- `Application PostQuitMessage`, `Application ExitProcess`, `EXE`/`Module GetMessageA/W returned WM_QUIT` e `PeekMessageA/W removed WM_QUIT`: caminhos cobertos de saída. Código, endereço e RVA são registrados quando aplicável às imagens cacheadas do EXE/`Contour.dll`; outros caminhos podem não passar pelos hooks.
- `Host WM_DESTROY: preserving pending native UI quit code=...`: quit nativo observado da thread da interface preservado, sem postar zero por cima. Quits de workers não definem esse estado.
- `Host WM_DESTROY: no native UI quit pending; shim calls PostQuitMessage(0)`: pedido próprio do host, distinto de um pedido registrado do aplicativo.
- `First-chance exception (may be handled by application)`: observação, **não confirmação de crash**. O aplicativo pode tratar a exceção; o observador sempre retorna `EXCEPTION_CONTINUE_SEARCH`. Observa AV, instrução ilegal e divisão inteira por zero, sem engolir/tratar a falha.

O shim não cancela `ExitProcess`/`PostQuitMessage`. Os registros de lifecycle/saída/exceção são de melhor esforço: não esperam pelo lock do logger nem chamam `OutputDebugString`, e podem omitir linhas sob contenção. A escrita em arquivo ainda é síncrona. O código final é um dado adicional: zero não prova funcionamento correto, e não zero sozinho não prova crash (a fixture, por exemplo, sai intencionalmente com 23). Ausência de evento/dump ou de linha de exceção também não prova saída normal.

No ensaio 0.3, a primeira chance mapeada a `D3D9on12.dll+0x78890` foi seguida de milhares de apresentações; **não foi um crash demonstrado**. Não esconda a exceção nem force a continuação como suposta correção.

Guarde os logs localmente e informe versão, `ScaleRendering`, `NativeResize`, código decimal/hexadecimal, última etapa, GPU/driver, Windows, monitores/DPI, dimensões e operação realizada. Compartilhe somente trechos revisados; **não envie ao Git logs/dumps, relatórios privados integrais, originais ou capturas do seletor de rede com IPs**. Capture dump apenas com autorização. Os 648 PASS sintéticos finais da 0.4 não substituem a inspeção visual/input nem uma matriz de drivers.

## Build não reconhecido (exit 3)

Compare os dois hashes de `docs/ANALYSIS.md`:

```powershell
Get-FileHash "C:\Program Files (x86)\Lake\Lake Controller v5.8\Lake Controller.exe" -Algorithm SHA256
Get-FileHash "C:\Program Files (x86)\Lake\Lake Controller v5.8\Contour.dll" -Algorithm SHA256
```

Não substitua os hashes no código sem reinspecionar o novo build. Não existe `--force`/`--allow-unknown`.

## DLL não encontrada / Win32 126, 127, 193 ou 1114

- Mantenha DLL e launcher da mesma compilação, x86, juntos.
- Confira o export `LakeWindowedInitialize` com `dumpbin /exports`.
- 193 costuma indicar mistura de arquitetura. 126 também pode ser uma dependência ausente.
- O Zig 0.14.1 gera dependências UCRT do Windows 10/11; MSVC usa `/MT`.
- O patch não fornece os runtimes legados exigidos pelo Lake, como componentes DirectX antigos. Não obtenha DLLs de sites avulsos; use fontes oficiais/licenciadas.

## Debug/injeção bloqueados, timeout (1460) ou acesso negado (5)

O launcher precisa criar/debbugar apenas seu próprio filho e criar threads nesse filho. Antivírus, EDR ou políticas locais podem impedir isso. Não desative proteções automaticamente; peça revisão/allowlisting do código conforme a política local. Não use injeção em um Lake já aberto como contorno.

O estágio da falha e o código Win32 aparecem no console. Em falha de inicialização, o filho criado pelo launcher é encerrado; nenhuma instância já existente é morta. A DLL é inicializada fora do loader lock; fazer a injeção no primeiro breakpoint do loader pode causar deadlock e não é equivalente ao procedimento deste projeto.

## Iniciou sem bordas

- Abra `LakeWindowedLauncher.exe`, não `Lake Controller.exe`.
- Não use `--native-only` se espera a moldura do shim; ele serve apenas para comparação diagnóstica.
- Não copie a DLL como `ddraw.dll`, `user32.dll` ou `d3d9.dll`.
- Depois do build, confira o prefixo da versão executada (`[LakeWindowedFix 0.5]` na revisão atual), `Initialized`, `client hosted`, política `ScaleRendering=1` e `Host setup complete`. Setup concluído não significa renderer saudável. `OutputDebugString` não contém todos os registros críticos de melhor esforço.
- O patch não corrige uma janela criada por outro módulo/classe desconhecido. Informe hash e contexto, não envie o EXE.

## Sem resize/maximizar ou configuração aparentemente ignorada

`NativeResize=0` **não significa mais janela fixa**: com `ScaleRendering=1`, o host 0.4 deve ser redimensionável. Verifique a versão da DLL e o INI realmente ao lado dela; build/instalação preservam INIs existentes.

- `ScaleRendering=0`, `NativeResize=0`: modo fixo diagnóstico da 0.3, sem resize/maximizar; `[window]` não altera o framebuffer e recorte pode reaparecer.
- `ScaleRendering=1`, `NativeResize=0`: `[window]` controla a janela; `[render]` controla o canvas fixo, não o tamanho externo.
- `NativeResize=1`: tem precedência e **desabilita escala**. É o rebuild legado EXPERIMENTAL, não uma exigência para resize da moldura nova. Seu mínimo é 800×600; a altura 576 do INI novo volta ao fallback legado de 768.

Feche/reabra o aplicativo após mudar a configuração e compare os modos em execuções separadas, fora de produção.

## Letterbox, barra inferior cortada ou mouse desalinhado

Barras pretas são esperadas quando a proporção da janela difere da fonte: preservam a imagem sem esticar desigualmente. Uma janela menor torna os elementos menores, não reorganiza os widgets. Cliques nas barras não devem acionar pontos válidos do canvas.

O canvas padrão **1088×612** foi reduzido para melhorar o tamanho dos controles. A largura aceita é **1024–4096** e a altura **600–4096**, com fallback para 1088/612 fora da faixa; nem toda combinação dessa faixa teve a UI validada. Para caber no monitor, ajuste `[window]` ou a moldura; para mudar o tamanho relativo dos controles, altere `[render]` com o Lake fechado. Compare a fonte real indicada pelo presenter, não um cache de inicialização.

O ensaio 0.3 a 125% DPI reproduziu corte no modo fixo e comandos da direita ainda cortados após reduzir o renderer para 1024×768. Na 0.4 final, fonte/layout 1088×612 e barra completa foram verificados a 125% DPI, inclusive maximizado e com letterbox. O processo continua DPI-unaware; dimensões lógicas/físicas diferem. Outros DPI e DPI misto permanecem pendentes.

Se a imagem parar ao mudar a proporção, confira o fallback de letterbox e a versão da DLL: o backend observado recusava `COLORFILL` da primária com **`DDERR_EXCEPTION = 0x88760037`**. A correção preenche as faixas com GDI somente dentro do host e continua retornando o HRESULT real do `Blt` da imagem. Se GDI também falhar, preserva o erro do fill; não converta erros em sucesso nem pinte fora do host.

Se o host maximizado ultrapassar o tamanho do canvas, verifique também o clipper: o `SetClipper` da primária pode redirecionar o `SetHWnd` real do cliente para o host visível, por callback opcional. Isso não muda as flags originais de cooperação e não deve ser substituído por sucesso fictício de `SetCooperativeLevel`.

Para mouse, Get/Peek A/W convertem **`MSG.pt`**, inclusive `PM_NOREMOVE`, e fazem hit-test virtual nos HWNDs filhos. A posição física pode atingir `WorkWindow` enquanto a imagem mostra `ButtonBar`: não basta encaminhar tudo ao main. `ClientProc` converte `lParam` do main só no dispatch; filhos nativos recebem coordenadas locais virtuais já convertidas uma vez. Wheel passa pelo main, em coordenadas de tela, com guarda contra propagação recursiva. Barras/captura fora da imagem usam a sentinela de canvas `(-1,-1)`; captura de filhos também é coberta. O único setter com inversão é o par **Get `0x36fc43` → Set `0x36fc55`**, armado por thread e calculado por deltas; outros `SetCursorPos` permanecem originais.

Registre modo, tamanho lógico/físico, source/viewport, posição, DPI e qual controle/caminho falhou. Isso não comprova touch, IME, controles filhos nativos distintos ou todo comportamento de captura. Fontes parciais, `Flip`/`BltFast` e caminhos não observados não recebem escala universal. Não mude automaticamente para `NativeResize=1` como suposta correção.

## Canvas e seleção de adaptador/modo

Não virtualize `GetDeviceCaps`: suas dimensões alimentam o seletor nativo de modo antes do layout, e um canvas virtual maior pode impedir a seleção. A integração mantém essa consulta real e faz **seed de apenas dois `LONG` graváveis**, em RVAs `0xc8aa34`/`0xc8aa38`, quando o `GetClientRect` real retorna em **`0x369005`**, após a seleção e antes da UI. É específico do Build 007, com limites de imagem, região `PAGE_READWRITE` e valores anteriores em 1–32768; ocorre uma vez por cliente, sem patch de código, `WM_SIZE` ou rebuild.

**O seed foi observado no ensaio final**, com fonte/layout 1088×612. A UI foi construída com esse canvas, não encolhida durante resize. Isso não valida outros builds ou testes de corrupção das guardas. Não altere esses globais manualmente nem remova as guardas como contorno. Os arquivos originais no disco continuam intactos, mas a arquitetura não é “apenas IAT + COM”: inclui esses dois dados em memória.

## Aviso de 32 bits

O aviso observado usa a flag em RVA `0xc8a892`, derivada de **`DDCKEYCAPS_SRCBLTCLRSPACE` (`0x400`)**: suporte a intervalo de color key de origem em `Blt`, não uma capacidade genérica de 32 bpp. Na 0.3 a flag era zero nas três execuções, com/sem shim, e foi possível continuar offline.

A 0.4 não esconde o aviso, falsifica `GetCaps` ou transforma HRESULT de erro em sucesso. Não substitui o backend Windows nem porta D3D7 para D3D11; também não fornece um `ddraw.dll` para autoload. Não obtenha DLLs avulsas para silenciar a mensagem.

## Rebuild legado lento, tela preta, RDP/hotplug

O renderer e a recuperação de superfícies continuam sendo nativos. Com `NativeResize=1`, resize usa rebuild após o arraste, não a cada pixel; no ensaio 0.3, inicialização/restauração demoraram **4.812/4.938 ms** e não adaptaram toda a barra de comandos. Isso não é o caminho padrão escalado da 0.4.

No modo escalado, registre `Scaled presentation first failure ... HRESULT=...`/bypass e o comportamento real; não trate `Blt` bem-sucedido como imagem correta. Se houver tela preta ou erro, feche normalmente e documente o estado antes de comparar outro modo ou o EXE original. Não há garantia de recuperação universal de superfície, mudança de profundidade de cor, sessão remota ou reconexão de monitor, em nenhum modo.

Teste primeiro sem processadores de áudio de produção. Se o Lake já estiver travado, o launcher não é ferramenta de recuperação nem interfere nesse processo.

## Permissões

O uso portátil com `--app-dir` não grava o patch em `Program Files`. A instalação opcional nessa pasta normalmente requer elevação. Os comportamentos originais de escrita de configurações e alteração de rede continuam sujeitos às permissões do próprio Lake.
