# Validação e limites

## Revisão 0.5

A validação das opções de fonte/cursor, as contagens atuais e a comparação isolada de 16/32 bits de cor estão em [UI-QUALITY.md](UI-QUALITY.md). Os resultados abaixo registram a **base 0.4 e seu histórico**, não substituem o reteste das opções novas. A 0.5 preserva as seis suítes anteriores e acrescenta `ui_quality_policy` / `LakeFixUiQualityTest.exe`.

**0.4 experimental, compilada e operada em OFFLINE.** O build final Zig passou em **648 verificações sintéticas, zero falhas**. A sessão final abriu a interface, executou menus/resize/foco e encerrou por Quit/Yes com código 0. Padrão: `ScaleRendering=1`, `NativeResize=0`, canvas **1088×612**, janela inicial **1024×576**. Isso não é validação universal ou de áudio em produção.

## Ambiente desta implementação

- Host: Windows 11 Home Single Language, `10.0.26300`, 64-bit.
- Toolchain disponível usada: Zig 0.14.1, target `x86-windows-gnu`, C++17.
- Inspeção: Python 3.12.8, pefile 2024.8.26, Capstone 5.0.7.
- MSVC/MinGW e Windows 10: instruções fornecidas; execução não observada neste ambiente.
- Na 0.1 houve consulta passiva; na 0.2, encerramento relatado pelo usuário sem causa determinada; na 0.3, ensaio real autorizado com limites explícitos. Na 0.4, candidatos intermediários foram reprovados e corrigidos antes do ensaio final. Áudio/hardware não foram operados.

## Testes automáticos

### 0.4 — seis suítes aprovadas

Log privado `private/build-validation-v04-reduced-02.txt`: **quatro cenários Win32** e **dois testes standalone**, sem warnings/falhas no build final. As contagens incluem verificações do launcher quando aplicável:

Os testes COM falsos antigos não detectaram a rejeição de vtables nativas pelo Windows. Foram substituídos por testes de proxies próprios, identidade de `this`, todos os slots SDK, aliases/lifetime/concorrência e HRESULTs. As fixtures de mouse incluem HWND filho, hit-test virtual independente do alvo físico, Get/Peek A/W, `PM_NOREMOVE`, captura e wheel não consumido, sem mover o cursor global.

| Cenário/teste | Configuração/objetivo | Estado 0.4 |
| --- | --- | --- |
| `windows_safe_no_rebuild` | Modo fixo diagnóstico | 65 PASS, 0 falhas |
| `windows_experimental_resize` | Rebuild legado simulado | 63 PASS, 0 falhas |
| `windows_exit_observer` | Saída intencional 23 e relatório/fallback | 5 PASS, 0 falhas |
| `windows_scaled_window` / ScaledWindow | Host/canvas/mouse/lifecycle | 317 verificações + 4 do launcher; 0 falhas |
| `scale_geometry` / ScaleGeometry | Proporção, coordenadas, deltas e limites aritméticos | 119 PASS, 0 falhas |
| `scaled_presenter_objects` / Presenter | Proxies COM falsos completos, clipping/fallback e erros | 75 PASS, 0 falhas |

O presenter verificou **30/49 slots SDK**, `this`/tabelas nativas intactos, QI cobertas, aliases nativos, offscreen real, unwrap, referências próprias, letterbox/clipper e propagação de erros. O fallback de letterbox é opcional: preenchido o host, retorna-se o HRESULT da apresentação real; se ambos os preenchimentos falharem, preserva-se a falha. COM falso **não exercita a GPU**.

O probe DirectDraw real reproduziu a falha do clone e, com o proxy, registrou **QI D3D7 = 0, cooperação = 0, tabela nativa intacta e proxy distinto**. Um probe separado conferiu pixels da apresentação reduzida em três tamanhos de janela próprios. No Lake final, o seed foi alcançado e fonte/layout **1088×612** foram observados; veja R4 em `ANALYSIS.md`.

A saída é **`dist-v0.4/`**, preservando `dist-v0.3/`. Para repetir os testes sintéticos:

```powershell
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 0
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 1
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 0 -ExpectedExit 23
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 0 -ScaleRendering
.\build-zig\LakeFixScaleGeometryTest.exe
.\build-zig\LakeFixPresenterTest.exe
```

As três primeiras suítes optam explicitamente por `ScaleRendering=0`; a quarta usa canvas 1536×864 e janela inicial 1024×576. Cada execução Win32 usa sua própria cópia sintética/INI em `test-runs/<GUID>/` sob o build. O limite de tempo da fixture/CTest é separado do `--wait` de produção. O roteiro não substitui o probe nativo nem a matriz do Lake.

### Histórico 0.3 — 133 PASS preservados

Build Release x86 Zig concluído após as correções de diagnóstico; binários em **`dist-v0.3/`**, sem sobrescrever `dist-v0.2/`. O log local ignorado `private/build-validation-v03.txt` contém **133 verificações PASS, zero falhas**:

| Suíte | PASS | Falhas | O que comprova no alvo sintético |
| --- | --- | --- | --- |
| `NativeResize=0` | 65 | 0 | Tamanho original preservado, sem rebuild inicial/por arraste e sem controles de resize/maximizar; visibilidade e diagnósticos |
| `NativeResize=1` | 63 | 0 | Opt-in de resize/rebuild simulado, coalescência e regressões de visibilidade |
| Saída intencional `23` com `--wait` | 5 | 0 | Launcher observa a saída não zero e verifica o arquivo com código decimal/hexadecimal |

As contagens incluem verificações do launcher sintético. A saída 23 da fixture é **intencional**, não falha nem crash; o teste passa ao observá-la corretamente. As duas suítes de janela também verificam que `PostQuitMessage(7)` chega intacto a `WM_QUIT`, sem o shim substituir o código.

Regressões finais: quit 23 postado no `WM_DESTROY` nativo permanece 23 após o cleanup do host; depois de consumido, não deixa estado obsoleto, e quit 99 de uma worker não impede o host da interface de postar zero. Um SRWLOCK próprio mantido por outra thread verifica que o helper de logging best-effort rejeita contenção em menos de 1000 ms; não força o lock privado da DLL nem testa toda espera possível de I/O. O launcher mantém um log local próprio aberto sem compartilhamento de escrita e confirma fallback em `%TEMP%`, código decimal/hex e conteúdo local preservados, sem alterar ACLs.

No histórico 0.3, os logs das fixtures confirmaram **zero rebuilds** no modo fixo e **cinco pares begin/complete** no experimental. Esses pares só medem o procedimento sintético, não reconstruções gráficas reais.

No registro 0.3, `tests/Run-Fixture.ps1` criou uma pasta por execução sob o build (`build-zig/test-runs/<GUID>/`), copiando **apenas artefatos sintéticos** e escrevendo um INI próprio. As três suítes não sobrescreveram a configuração/DLL umas das outras nem usaram a instalação real. Seus nomes CTest são `windows_safe_no_rebuild`, `windows_experimental_resize` e `windows_exit_observer`; foram reexecutadas como regressões na 0.4.

O teste do observador de exceções chama **`RaiseException(EXCEPTION_ACCESS_VIOLATION, ...)` por software e trata explicitamente essa exceção** na fixture. Não faz um acesso inválido real à memória, não simula uma GPU com falha e não comprova recuperação de crash. Verifica que o diagnóstico de primeira chance é registrado e que o observador retorna `EXCEPTION_CONTINUE_SEARCH`, permitindo o tratamento original.

O `--wait` de produção aguarda o término do processo sem timeout nessa observação; o launcher sintético e o CTest mantêm limites próprios para testes. Aprovar a fixture não valida todas as possíveis rotas de saída do programa proprietário.

### Histórico 0.2

- Regressão nova reproduz o pedido nativo `PostMessage(0x8032)` → `ShowWindow(main, 1)` antes do loop externo: **duas falhas confirmadas na DLL 0.1**.
- Build 0.2 Release x86 Zig: **46 verificações PASS, 0 falhas**, incluindo as duas regressões, visibilidade/hide/minimizar/restaurar, fallback externo, loop interno filtrado pelo HWND do cliente e descoberta de instância existente sem acesso à memória.
- Os originais e a pasta `dist/` da 0.1 não foram substituídos. A saída nova está em `dist-v0.2/` porque a DLL antiga continuava em uso.
- A consulta às janelas do PID histórico 5836 confirmou host oculto e cliente com `WS_VISIBLE` mas invisível por ancestral oculto. **Não houve lançamento/reteste automático do Lake real.**
- Logs em `private/regression-v01.txt`, `private/build-validation-v02.txt` e `private/window-state-v01-failure.json` (todos ignorados).

No reteste posterior feito **pelo usuário**, PID histórico `14208`, `dist-v0.2/logs/LakeWindowedFix-14208.log` chegou a `Native ShowWindow cmd=1/5`, `Native resize begin/complete`, `Initial placement complete` e `Native windowed host ready`. O processo depois fechou. Não foi encontrado evento Aplicativo 1000/1001/1002 correspondente nem dump; **crash versus saída normal permanece indeterminado**. Esses marcos de janela não medem saúde da GPU. Riscos estáticos adicionais e limites de inferência estão em [ANALYSIS.md, R2](ANALYSIS.md#r2--encerramento-após-o-setup-da-02-e-diagnóstico-na-03).

### Registro anterior, 0.1

| Verificação | Resultado |
| --- | --- |
| Build Release x86, Zig 0.14.1 | Aprovado, sem warnings na execução final |
| Fixture Win32/injeção + autoteste SHA-256 | **35 verificações PASS, 0 falhas** |
| Launcher de produção com pasta sem hashes originais | Recusou com exit 3, sem iniciar alvo |
| Parser PowerShell (quatro scripts) | Sem erros de sintaxe |
| Compilação sintática do script Python | Aprovada |
| Inspeção PE de `dist/` | Launcher/DLL x86; export `LakeWindowedInitialize` presente |
| Reinspeção dos quatro arquivos originais | SHA-256 idênticos aos iniciais |
| MSVC, MinGW GCC, Windows 10 e renderer Lake real | **Não executados** |

O log local da 0.1 está em `private/build-validation.txt` (ignorado); seus binários ficam em `dist/`. As pastas `dist*`, builds e relatórios locais não são conteúdo para publicação. Versione somente os arquivos públicos do projeto; não envie logs/dumps, relatórios privados integrais ou binários originais.

### Cobertura histórica e limites

O build padrão compila uma aplicação Win32 inteiramente própria (`LakeFixFixture.exe`), uma **`Contour.dll` sintética** de teste e um launcher de teste (`LakeFixInjectionTest.exe`). O launcher de teste reutiliza o fonte de injeção de produção, mas só inicia o nome fixo da fixture. O launcher de produção mantém sua allowlist SHA-256 e não contém esse modo de teste. A DLL sintética não contém código Lake, nunca é instalada por este projeto e **não deve ser copiada para a instalação real**.

Cobertura sintética herdada da 0.3 e reexecutada na revisão final 0.4, respeitando a política selecionada:

- ABI x86, injeção antes do entry point, export WINAPI sem decoração externa.
- SHA-256 de vetor conhecido (`abc`) aceito e hash incorreto recusado.
- Criação ANSI e Unicode, popup convertido para renderer filho + host padrão.
- Janela não relacionada permanece intacta.
- Setup inicial enfileirado; título/bordas e tamanho mínimo do host. `HostSetupCompleted` representa somente setup da janela, não saúde gráfica.
- Modo 0 preserva dimensões do framebuffer em chamadas cobertas e desabilita resize/maximizar; modo 1 permite o rebuild simulado.
- Bombeamento reentrante de `PeekMessage` antes do primeiro `GetMessage` não habilita resize prematuro.
- No modo 1, coalescência de `WM_SIZE` durante arraste, sem rebuild com tamanho zero/minimizado; no modo 0, sem encaminhamento ao rebuild nativo do host.
- `WM_KILLFOCUS` externo não alcança o bounce legado simulado; ativação continua entregue.
- Guardas de `SetWindowLong`, `SetWindowPos` e chamadas obtidas com `GetProcAddress`.
- `ChangeDisplaySettingsExW` neutralizado, mas `CDS_TEST` encaminhado. O dispositivo de teste é propositalmente inexistente para não alterar resolução mesmo se um teste regredir.
- Destruição e recriação de janelas.
- `WM_CLOSE` da moldura respeita veto e cleanup do main.
- Primeira chance tratada pela fixture continua tratável; quits 7 e 23 preservados, quit de worker isolado e código final registrado, inclusive saída intencional 23.
- Helper de try-lock sob contenção e fallback do relatório após falha de abertura local, sem alterar ACLs nem truncar logs existentes.
- Carregamento tardio da DLL gráfica sintética, com imports interceptados antes de sua factory ser usada; erros de `LoadLibrary` preservados.

Esses testes **não** criam superfícies DirectDraw/Direct3D7 reais nem exercitam GPU/driver ou o teardown/renderer proprietário identificado estaticamente. Não validam mouse/touch no Lake, dependências legadas, perda de dispositivo, latência de áudio ou correção do travamento/encerramento real. Preserve logs e estado final **localmente**, sem versioná-los; não tratar “133 PASS” da 0.3 como aprovação da 0.4 ou da matriz real.

O registro histórico da 0.1 inclui análise sintática dos scripts anteriores. Os instaladores não foram executados contra a pasta real protegida do Lake. MSVC, MinGW GCC e Windows 10 continuam sem execução observada neste ambiente.

## Histórico real 0.3 — ensaio autorizado, não validação da 0.4

Fonte local privada: `private/runtime-20261024-02/REPORT.md`. Monitor 1920×1080, área útil 1920×1020, DPI 120/125%, processo DPI-unaware (0). A observação REA/Frida pode afetar timings; contagens de `Blt` cobrem somente o intervalo observado. O adaptador físico selecionado pelo renderer não foi estabelecido.

| Execução | Resultado limitado registrado |
| --- | --- |
| PID 6632, 0.3 fixa | Fonte 1536×863, cliente físico 1920×1078 e moldura 1928×1105: barra inferior cortada. Menus, um movimento, um minimizar/restaurar e um Alt+Tab/retorno funcionaram. 4.655 `Blt` primários com HRESULT 0; Quit/Yes, exit 0 |
| PID 12380, nativo sem host | Após mover a (100,40) físicos, destino continuou em (0,0), com imagem deslocada. 1.687 `Blt` com HRESULT 0; Quit/Yes, exit 0 |
| PID 13272, cópia 0.3 com resize legado | Renderer 1024×768/cliente físico 1280×960 cabível, mas comandos da direita cortados. Rebuild inicial 4.812 ms, ao restaurar 4.938 ms; X da moldura, exit 0 |

As três saídas normais não corrigem o recorte nem demonstram estabilidade prolongada. A primeira tentativa interrompida por timeout da ferramenta foi excluída das conclusões. Não houve arraste contínuo, maximização ou matriz de tamanhos. O cache de startup não foi tratado como descrição atual da superfície.

Uma AV de primeira chance em `D3D9on12.dll+0x78890` foi seguida de continuação do aplicativo, não classificada como crash. O aviso de 32 bits apareceu com/sem shim; a flag `0xc8a892=0` corresponde ao bit `DDCKEYCAPS_SRCBLTCLRSPACE` (`0x400`), não a capacidade genérica de 32 bpp. O usuário relatou rede/offline funcionando; o ensaio foi offline, sem conectar hardware, alterar adaptadores ou usar Save/Recall. Não é validação de áudio/rede.

Hashes do EXE/`Contour.dll` originais e launcher/DLL/INI 0.3 permaneceram iguais. Logs/capturas/relatórios integrais ficam privados; imagens do seletor podem expor IPs e **não devem ser publicadas**. Mais detalhes e limites em [ANALYSIS.md, R3](ANALYSIS.md#r3--ensaio-real-autorizado-da-03-e-motivação-da-04).

## Ensaio gráfico 0.4 operado pelo agente

A sessão final, PID histórico **10148**, selecionou OFFLINE, confirmou o aviso de vídeo e escolheu **No**, sem recuperar/salvar presets ou configurar adaptadores/hardware. Encerrou por **Quit → Yes, exit 0**, sem instância restante. Monitor principal 1920×1080, área útil 1920×1020, DPI 120/125%, processo DPI-unaware. O adaptador gráfico físico não foi estabelecido.

Foram analisadas capturas intermediárias: Modules, Home, cliente físico 800×600 e 600×450 com letterbox, maximizado 1920×991, arraste de borda de 600×450 para 520×410 e mínimo 400×225 físico (320×180 lógico), depois retorno a 1280×720. A interface completa foi preservada; no mínimo, o texto naturalmente fica muito pequeno. Wheel em área vazia, um ciclo minimizar/restaurar e um Alt+Tab/retorno foram operados. Houve ativação explícita da janela após Alt+Tab.

A amostra REA/Frida final observou **9.315 Blts da primária com HRESULT 0**, **zero CreateSurface do DD7 acompanhado** e fonte/layout fixos **1088×612** durante movimento/resize/foco. Não cobre todas as chamadas gráficas. Resize mais resposta `WM_NULL` levou **0–46 ms** nos tamanhos registrados; não é benchmark de GPU/áudio. Nenhum rebuild `ContourParent` foi registrado na sessão final.

Um candidato anterior mostrou `DDERR_EXCEPTION` no COLORFILL da primária: devolver essa falha sem apresentar a imagem congelava/cortava o conteúdo em proporções diferentes. O final usa fallback GDI restrito ao host; depois de ativado, a amostra não precisou de fills DirectDraw. Capturas/ações/logs e Evidence `ev_83a3b7397232f1bcdb19a83cfca29ad385a8adb457343f685d9c490cc32d65df` permanecem privados em `private/runtime-20261024-04/`.

Um candidato foi encerrado forçadamente após perder a saída ao fechar um modal de startup; **não foi contado como saída normal**. O host final adia esse pedido de fechamento até concluir os diálogos; guarda e veto nativo foram testados sinteticamente. O Windows chegou a bloquear a sessão: cliques foram recusados e só continuaram após desbloqueio manual pelo usuário, sem contornar autenticação.

### Padrão 0.4: `ScaleRendering=1`, `NativeResize=0`

Os casos abaixo distinguem operações reais de ensaios prolongados/ambientes ainda pendentes. Código final e HRESULTs não substituem inspeção visual/input.

| Caso | Resultado esperado | Estado |
| --- | --- | --- |
| Interoperabilidade DirectDraw/D3D7 | QI/cooperação nativas preservadas | Probe nativo aprovado; clone descartado |
| Abrir pelo launcher em sessão de teste | UI aberta e utilizável | Operado no ambiente acima |
| Seed de layout após seleção nativa | Dimensões antes de construir a UI | Observado em 1088×612; corrupção de dados não forçada no proprietário |
| Interface completa | Barra e comandos da direita visíveis | Verificado nas capturas citadas |
| Mover entre posições do desktop | Destino/letterbox acompanham o host | Operado no monitor principal |
| Resize até 320×180 lógico, 4:3/16:9 e maximizar/restaurar | Fonte fixa e conteúdo inteiro | Operado nos tamanhos citados |
| Host maximizado maior que o canvas | Sem corte pelo HWND fixo | Verificado em 1920×991 físico versus canvas-HWND 1360×765 |
| Arraste contínuo | Sem rebuild por pixel ou ao soltar | Um arraste real de borda, oito passos; não ensaio prolongado |
| Mouse via Get/Peek A/W e dispatch | Hit-test virtual e coordenadas coerentes | Fixtures A/W aprovadas; menus/avisos/Home/Quit e wheel operados no Lake |
| Par getter/setter do cursor | Conversão inversa apenas no par de retornos `0x36fc43`/`0x36fc55` armado por thread; demais setters inalterados | Pendente no Lake |
| Controles filhos nativos de classes distintas | Verificar hit-testing, texto e foco separadamente, sem supor remapeamento universal | Menus/diálogos citados e fixture de filho operados; demais controles/texto pendentes |
| Alt+Tab 100 vezes | Outra janela recebe foco; volta sem travar | 1 ciclo operado; 100 pendentes |
| Minimizar/restaurar 50 vezes | Repaint/input normais e fonte fixa | 1 ciclo operado; 50 pendentes |
| Botão interno “minimize” | Minimiza o host, não apenas o renderer filho | Pendente |
| Diálogos de arquivos, avisos e texto | Modalidade, teclado e foco funcionam | Aviso gráfico/No e confirmação de saída operados; arquivos/texto/teclado pendentes |
| Touch, IME e acessibilidade | Sem regressões de input | Pendente |
| 100%, 125%, 150%, 200% DPI | Medir lógicos/físicos e input | 125% operado; demais pendentes |
| Vários monitores / DPI misto | Mover e restaurar; testar clipping | Pendente |
| RDP, hotplug, bloqueio de sessão | Registrar perda/recuperação de superfícies | Pendente, sem garantia |
| Fechar por X e pela UI interna | Sem processo órfão | Candidato reduzido fechou por X com exit 0; final por Quit/Yes, exit 0 |
| Encerramento observado com `--wait` | Código final registrado | Confirmado na sessão final |
| `WM_DISPLAYCHANGE`/mudança de ambiente | Mensagem segue para o original; registrar eventual saída nativa, sem bloqueá-la | Pendente, sem garantia |
| Falha DirectDraw/perda de superfície | Registrar HRESULT real e recuperação original, sem convertê-lo em sucesso ou esconder aviso | Pendente, sem garantia |
| Reabrir pelo EXE original | Comportamento original, hashes idênticos | Reabertura não operada nesta rodada; hashes conferidos |
| Rede/áudio com hardware | Fora deste roteiro offline; exige autorização e validação dedicadas, não inferidas do teste gráfico | Pendente, sem operação de hardware neste ensaio |

### Comparações legadas separadas

`ScaleRendering=0`, `NativeResize=0` mantém o diagnóstico fixo da 0.3, sem resize/maximizar; recorte pode reaparecer. Não é o padrão 0.4 nem uma alternativa comprovada ao objetivo de janela livre.

**`NativeResize=1` desabilita escala e permanece EXPERIMENTAL.** Não é necessário para resize do host escalado. Feche o aplicativo antes de mudar o INI; use altura inicial de pelo menos 600 (por exemplo 768), pois os 576 do padrão novo caem no fallback legado.

Se esse opt-in for comparado, registre separadamente:

| Caso | Resultado a verificar | Estado |
| --- | --- | --- |
| `Width`/`Height` iniciais | Dimensões aplicadas, respeitando o mínimo 800×600; renderização real correta | Pendente |
| Resize até mínimo e maximizar/restaurar | UI/superfícies reconstruídas; sem tela preta, trava ou saída inesperada | Pendente |
| Arraste contínuo de borda | Sem rebuild por pixel; redesenho após soltar e input alinhado | Pendente |

O fato de `Native resize complete` aparecer não prova que HRESULTs e recursos nativos estejam válidos nem que a UI inteira caiba. R3 já reproduziu corte e rebuild demorado nesse modo. Não use o opt-in em áudio de produção.

Se o processo travar, capture evidência **com autorização** e guarde em `dumps/` ignorado. Não publique logs integrais, presets, dados de rede, dumps ou originais. Uma primeira chance pode ser tratada, e ausência de evento/dump não prova saída normal. Não faça firmware update nem altere adaptadores como parte de um teste gráfico.

## Critério para chamar a correção de validada

Uma alegação de suporte exige build/testes, interoperabilidade nativa e matriz essencial em **cada** SO/GPU/driver alegado: UI completa, seed no momento correto, clipper, movimento, resize/maximizar, input sem dupla escala, Alt+Tab, minimizar/restaurar, diálogos e saída. O objetivo 0.4 usa **escala ligada e rebuild nativo desligado**, sem depender de aprovar o opt-in legado. Exit 0 antes da UI é reprovação. COM falso/geometria, números intermediários ou os 133 PASS históricos não aprovam GPU, Windows 10, touch/IME, controles filhos ou áudio/rede ainda não testados. A 0.4 permanece **experimental, com validação limitada às operações e ao ambiente observados**, sem aprovação universal ou para áudio em produção.
