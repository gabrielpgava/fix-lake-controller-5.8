# Análise estática e decisões técnicas

**Revisão 0.5:** tratamento de fonte/cursor e ensaio de RGB565 estão em [UI-QUALITY.md](UI-QUALITY.md). Esta página conserva a análise e os resultados históricos que fundamentam a base 0.4.

Data: 2026-10-24. **0.4 experimental, compilada e operada em OFFLINE no Windows 11 a 125% DPI:** 648 verificações sintéticas aprovadas e ensaio real de menus/resize/foco, com canvas reduzido a 1088×612. R4 registra os candidatos reprovados e as correções finais; isso não valida outros ambientes nem áudio em produção. R1 registra observação passiva da 0.1; R2, o encerramento da 0.2; R3, o ensaio real da 0.3. Endereços VA usam a base preferida do PE, não endereços garantidos em runtime. `Contour.dll`: base `0x10000000`, máquina `0x014c` (x86).

## Identidade

| Arquivo | SHA-256 |
| --- | --- |
| `Lake Controller.exe` | `e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713` |
| `Contour.dll` | `bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283` |
| `StdGraphics.dll` | `d20c8500eeddf3038e01a1b25789ec5afca10430acb8b4a4219c1092a143e6e4` |
| `DaylightGraphics.dll` | `ffa34b4c6ee0896a7cec0fe85310e5f2a5ed31ffbad6c4da1d40aa5f4e74cb7e` |

`Contour.dll` contém a string `Lake Controller 5.8 Build 007`. O launcher valida os dois primeiros hashes. IATs/entry point derivam dos cabeçalhos PE, mas **a 0.4 não é mais independente de RVAs do build**: usa pontos de retorno específicos e semeia dois `LONG` graváveis em `0xc8aa34`/`0xc8aa38`. Não altera os arquivos originais no disco nem instruções executáveis para esse seed. A seleção de build por hash permanece obrigatória.

## Ferramentas e proveniência

- Na inspeção estática inicial, REA estava conectado, mas os providers de análise nativa estavam indisponíveis: Ghidra sem `GHIDRA_INSTALL_DIR`/runtime configurados; IDA e Kuna não configurados; Hopper não suportado no host Windows. Não houve descompilação nem alegação de CFG completo. Não foi instalado/reconfigurado um provider para contornar isso.
- REA `inspect_pe_resources` confirmou PE x86 e identidade do EXE; Evidence ID: `ev_8ccab34d400a5af87127ee0e605ac6182c23455895ccc93268cc42e46a6a29c5`. Cobertura: 16 entradas, 6 recursos. Limites desse Evidence: inventário estático de recursos; payloads exceto grupos de ícones opacos; nenhuma execução, fallback de idioma ou renderização observados. **Esse Evidence não fundamenta sozinho a identificação da API gráfica.**
- As observações de imports, exports, strings, referências candidatas e trechos assembly vêm de inspeção **local** com Python/pefile 2024.8.26 e Capstone 5.0.7. O script reprodutível é `scripts/Inspect-Target.py`; cada registro tem SHA-256 e `record_id` local, sem fingir ser Evidence REA.
- Os achados adicionais de R2 são inspeção estática local pefile/Capstone e leitura de log local ignorado, não saída de REA. Não há trace, pilha de falha ou dump que ligue esses caminhos ao encerramento real da 0.2.
- R3 resume `private/runtime-20261024-02/REPORT.md`, relatório privado de operação autorizada da **0.3**, com observação REA/Frida de campos e retornos de `Blt`. A instrumentação pode afetar timings; os limites de correlação e cobertura constam em R3.
- R4 registra probes locais do DirectDraw nativo, a substituição por proxies e o ensaio real final. A amostra Frida final é Evidence `ev_83a3b7397232f1bcdb19a83cfca29ad385a8adb457343f685d9c490cc32d65df`, export privado `private/runtime-20261024-04/final-evidence.json`; não são resultados de GPU inferidos de COM falso.
- Os relatórios privados e ferramentas baixadas ficam em diretórios ignorados. Nenhum binário original foi copiado para o repositório.

## Livro de achados

| ID local | Observação / inferência | Localização e limite |
| --- | --- | --- |
| A1 | EXE carrega `Contour.dll` com `LoadLibraryA`, resolve `MkCreatePlugin` e chama sua inicialização recebendo a linha de comando | EXE VA `0x004056dd`–`0x00405779`; sequência de chamadas observada em assembly, não execução |
| A2 | `Contour.dll` importa `DirectDrawCreateEx`, `DirectDrawCreate`, `DirectDrawEnumerateExA` | IAT RVAs `0x43c040`, `0x43c048`, `0x43c044` |
| A3 | Uso de interfaces DD7/D3D7 | exports decorados contêm `IDirectDrawSurface7` e `IDirect3DDevice7`; o IID em RVA `0x43efcc`, passado à factory, é `15e65ec0-3b9c-11d2-b92f-00609797ea5b` (`IID_IDirectDraw7`). Não é prova de cada caminho executado |
| A4 | `CreateWindowExA`, `SetWindowPos`, `ShowWindow`, `MoveWindow` são imports gráficos relevantes | IAT RVAs `0x43c484`, `0x43c494`, `0x43c49c`, `0x43c4c8` |
| A5 | Não há imports normais D3D9/OpenGL/`ChangeDisplaySettings` nesses quatro artefatos | Busca limitada às tabelas importadas; carregamentos dinâmicos e dependências transitivas não foram exauridos |
| A6 | Parser reconhece `nonfullscreen`, evita testar `fullscreen` nesse ramo e grava seleção de modo | código RVA `0x2e69e`–`0x2e6e8`; string RVA `0x464ec0`; globais RVAs `0xc8aa04 = 1`, `0x6bb258 = 0` |
| A7 | Parser reconhece `windowed` e ativa flag separada | código RVA `0x2e835`–`0x2e84d`; string RVA `0x464f50`; global RVA `0xc8a848 = 1` |
| A8 | Main class `Lake Controller` usa WndProc RVA `0x209e0` | registro em RVA `0x2ed37`–`0x2eda7`; nome na RVA `0x6bb328` |
| A9 | Sem parent explícito, main recebe `0x80080000 = WS_POPUP \| WS_SYSMENU` | criação em RVA `0x302a1`–`0x30319`; com retângulo/parent fornecidos, há outro ramo `WS_CHILD` |
| A10 | Classe `ContourParent` usa WndProc RVA `0x20120`, com rebuild nativo em resize | registro em RVA `0x301ed`; nome RVA `0x4650ac`; handler `0x20120`–`0x2027a`: ignora `SIZE_MINIMIZED`/`SIZE_MAXHIDE` e estado não pronto, chama `MoveWindow(main,0,0,w,h)`, atualiza dimensões e refaz recursos sob sincronização nativa. A proteção adicional contra 0×0 é do shim; as guardas não demonstram segurança do rebuild (R2) |
| A11 | O handler de foco chama `SetFocus(main)` para as mensagens 7 e 8 | tabela de dispatch em RVAs `0x24c8c`/`0x24c60`; ramo RVA `0x20bc6`–`0x20bd7`. Inadequação para troca externa de foco é inferência de compatibilidade |
| A12 | O ramo de ativação **não cliente**, `WM_NCACTIVATE` (`0x86`), é pulado com `windowed != 0` | tabela de dispatch e RVA `0x20c5d`–`0x20ca1`; evita chamada para helper legado `0x20990`. Não confundir com `WM_ACTIVATE`/`WM_ACTIVATEAPP`. A causa exata do travamento não foi demonstrada |
| A13 | Exclusividade real está também em COM, não só User32 | factory em RVA `0x368d60`; chamada por vtable `+0x50` em `0x368dc7` (`IDirectDraw7::SetCooperativeLevel`); chamada `+0x54` em `0x368e95` (`SetDisplayMode`) |
| A14 | O framework tem ramos normais/exclusivos | `esi+4` seleciona flags `0x1408` (NORMAL, MULTITHREADED, FPUPRESERVE) ou `0x1013` (FULLSCREEN, ALLOWREBOOT, EXCLUSIVE, FPUPRESERVE); escolha em `0x368da6`–`0x368dc7`. A flag do framework deriva do bit 0 em `0x3695d4`–`0x3695e2`; seleção de modo do aplicativo em `0x48b32`–`0x48bae` |
| A15 | O aviso de 32 bits usa uma capacidade específica de color key | RVA `0x292830` consulta `IDirectDraw7::GetCaps`; `DDCKEYCAPS_SRCBLTCLRSPACE` (`0x400`) define a flag `0xc8a892`. O aviso em `0x1f0b6`–`0x1f0d4` combina flag zero com display 32 bpp. Não é prova genérica de incapacidade para 32 bpp; a flag zero foi observada em R3 |

As opções são procuradas como substrings, em minúsculas, não como um parser moderno de argumentos. O launcher não repassa argumentos arbitrários: usa apenas as duas opções verificadas. `nonfullscreen` contém `fullscreen`, mas o código usa `else`/salto após reconhecer `nonfullscreen`; não aciona os dois ramos.

A mesma tabela encaminha `WM_SHOWWINDOW` (`0x18`) ao ramo `0x20a41`, que atualiza a flag usada para habilitar resize. Não se infere esse comportamento a partir de `WM_ACTIVATEAPP`: essa mensagem cai no procedimento padrão no switch observado.

GDI, GDI+ e `d3dxof.dll` também estão presentes. Isso não transforma o renderer em D3D9. A ausência de imports D3D9 não exclui absolutamente todos os caminhos possíveis de plugins.

## Implementação escolhida

### 1. Seleção nativa de modo

Usar as flags desde a criação do processo é menos frágil que alterar globais por RVA ou interceptar só `SetDisplayMode`. Não são suprimidas chamadas COM mantendo o aplicativo convencido de que tem superfícies exclusivas. A eficácia sobre todas as GPUs continua dependente de runtime.

### 2. Host escalado e canvas fixo na 0.4

O padrão passa a **`[compatibility] ScaleRendering=1`, `NativeResize=0`**. O host usa a classe privada **`LakeWindowedFix.ScaledHost.v1`**, baseada em `DefWindowProcA`, com título/bordas e resize/maximizar/minimizar. Não usa a WndProc destrutiva de `ContourParent`, inclusive nas mensagens de criação anteriores à subclass. O cliente `Lake Controller` mantém o procedimento original sob subclass e vira `WS_CHILD`; não se substitui o render loop.

`[render] Width=1088`, `Height=612` define o canvas virtual fixo, cerca de **29% menor em cada dimensão** que o candidato 1536×864, para ampliar os controles na mesma janela. São aceitas larguras 1024–4096 e alturas 600–4096; valores fora da faixa voltam a 1088/612. O canvas é definido antes de construir a UI; mudar o INI exige reiniciar o Lake. O cliente não muda de tamanho ao arrastar/restaurar o host, e esse caminho do shim **não envia `WM_SIZE` a `ContourParent` nem recria objetos/superfícies DD7**. Isso não impede perda/recriação iniciada pelo próprio backend ou por outros caminhos.

O tamanho do HWND, sozinho, não semeia corretamente todo o layout. Os globais `0xc8aa34`/`0xc8aa38` recebem dimensões de `GetDeviceCaps` **antes** da escolha de adaptador/modo; o seletor também consome a lista de modos e altura ajustada em um pixel. Virtualizar essa consulta pode exigir um canvas maior que o modo e fazer o seletor falhar. **`GetDeviceCaps` e a enumeração/capacidades permanecem reais.**

O hook de **`GetClientRect` chama a API real** e, somente no retorno RVA **`0x369005`** do helper de superfícies windowed, após a seleção de modo e antes do layout/UI, semeia `renderSize` nesses **dois `LONG` de dados**. Exige modo escalado, HWND do cliente, retângulo real igual ao canvas, intervalo dentro da imagem de `Contour.dll`, região `MEM_COMMIT`/`PAGE_READWRITE` contendo os oito bytes e valores anteriores em **1–32768**. O seed bem-sucedido ocorre uma vez por criação do cliente; não há patch de instruções, retorno fictício da API, escrita periódica de resize ou rebuild. **No ensaio final, o log confirmou o seed e a observação Frida registrou layout e superfície fonte em 1088×612.** Não foi provocada corrupção no proprietário para testar suas guardas.

`[window] Width=1024`, `Height=576` define a área cliente inicial em pixels lógicos, ajustada à área útil com as bordas. O mínimo interativo é 320×180; largura/altura de configuração fora de 320–8192/180–8192 voltam a 1024/576. Há correção inicial por medições da janela, mas isso **não é validação de todo cenário DPI**. A aplicação legada observada era DPI-unaware; não se deve interpretar pixels lógicos como físicos garantidos.

**Modos legados:** `ScaleRendering=0`, `NativeResize=0` conserva o tamanho original, ignora as dimensões de `[window]` e desabilita resize/maximizar, como na 0.3; pode ter recorte (R3). `NativeResize=1` tem precedência e desabilita a escala, voltando a `ContourParent`, mínimo 800×600 e rebuild **EXPERIMENTAL**. Reduzir o backbuffer não reorganizou toda a UI no ensaio real.

Na 0.1/0.2, o hook de `GetMessageA/W` era aplicado somente à IAT do EXE. Desde a 0.3, a observação de `GetMessageA/W` e `PeekMessageA/W` alcança os imports do EXE e de `Contour.dll`, além do lookup dinâmico coberto. **Somente chamadas de `GetMessage` cuja origem esteja na imagem do EXE podem agendar o fallback de setup**; `PeekMessage` não o agenda. No EXE inspecionado, `GetMessageA` é chamado em VA `0x004057d0`/`0x004057ff`, depois do retorno da inicialização do plugin e do método seguinte. Na 0.1, esse era o único marco que podia revelar o host: essa dependência foi insuficiente no uso real (R1).

Na 0.2, o host acompanha imediatamente o pedido nativo de visibilidade do cliente, mantendo o rebuild desabilitado nesse momento. O posicionamento inicial é enfileirado após o tratamento original da mensagem nativa `0x8032`, com o `GetMessage` externo apenas como fallback. A mensagem privada é postada ao **cliente**, que a encaminha à moldura: isso também atende loops que filtram por HWND e não retirariam mensagens postadas apenas ao novo parent. Bombear `PeekMessage` antes desse pedido/notificação não habilita resize. Não é usado um timer arbitrário nem se presume que apenas criar a janela signifique que o startup terminou.

As versões 0.3/0.4 mantêm esse hook de visibilidade e o agendamento, mas só permitem o rebuild do host com **`NativeResize=1`, EXPERIMENTAL**. A propriedade `LakeWindowedFix.HostSetupCompleted` e a linha `Host setup complete (not a renderer health check)` indicam apenas conclusão do setup da janela. Não verificam HRESULTs nativos, superfícies ou saúde da GPU; tampouco o antigo `Native windowed host ready` da 0.2 fazia essa verificação.

O fechamento da moldura envia `WM_CLOSE` ao main original e respeita eventual veto. Durante os modais de startup, o modo escalado adia esse pedido até o setup estar pronto: um candidato anterior ficou sem janelas e não encerrou após fechar durante o startup. A guarda e o veto foram testados sinteticamente. A moldura é removida após a destruição do filho, sem substituir a confirmação/rotina de cleanup nativa por `DestroyWindow` direto.

Nos modos legados que ainda usam `ContourParent`, mensagens de criação podem alcançar o procedimento nativo antes da guarda da subclass. A política fixa legada não garante ausência de rebuild nessa fase. O host privado da 0.4 evita esse caminho no modo escalado; nenhum rebuild nativo foi registrado na sessão final, sem cobrir todos os caminhos do aplicativo.

No opt-in legado `NativeResize=1`, `[window] Width`/`Height` definem as dimensões iniciais enviadas ao resize nativo, não apenas à apresentação. O resize é coalescido durante o arraste até `WM_EXITSIZEMOVE`, filtrando tamanhos zero/minimizados. A reconstrução original pode consumir tempo ou falhar; não há promessa de latência zero. Uma trava existente dentro do renderer não é corrigida pelo shim.

### 3. Bridge de apresentação DirectDraw, não port gráfico

`src/fix.cpp` intercepta `DirectDrawCreateEx` pela IAT apenas do EXE/`Contour.dll` e pelo lookup dinâmico coberto. A abordagem intermediária de copiar/trocar o `lpVtbl` **do objeto nativo foi rejeitada pelo DirectDraw do Windows** (R4). Foi descartada para a revisão, mesmo usando uma cópia completa da tabela.

A substituição expõe **`WrapDirectDraw7(void** object)`**: retorna um **proxy próprio completo**, com as 30 entradas SDK de `IDirectDraw7`, e envolve a primária com um proxy de 49 entradas de `IDirectDrawSurface7`. Cada forwarder conserva a assinatura SDK e chama o método usando **o objeto nativo real como `this`**, nunca o endereço do wrapper. **Nenhum `lpVtbl` nativo ou tabela compartilhada é modificado.** Não se trata de apenas copiar ponteiros nativos para uma tabela de proxy e chamá-los com o `this` errado.

As `QueryInterface` cobertas de `IUnknown`/`IDirectDraw7` ou `IDirectDrawSurface7` preservam a identidade do respectivo proxy; outras consultas seguem para o nativo, incluindo `IDirect3D7`. Superfícies **offscreen permanecem reais, sem proxy**, para que o dispositivo D3D7 receba superfícies nativas. Quando a fonte de um `Blt` for um proxy, ela deve ser desembrulhada antes da chamada real.

O lifetime do proxy usa referências próprias e não depende de o `ULONG` retornado pelo `Release` nativo chegar a zero: aliases nativos podem continuar vivos sem deixar o wrapper pendurado. As estatísticas representam alocações/contextos do bridge, não referências GPU nem prova de compatibilidade nativa. Identidade, aliases, lifetime, concorrência e forwarders foram exercitados nos testes COM falsos finais; o probe nativo confirmou separadamente QI de D3D7 e cooperação, sem provar toda combinação de interfaces.

O caso escalado é o `Blt` da primária **com superfície fonte e `sourceRect == nullptr`**, isto é, a fonte inteira. `GetSurfaceDesc` da fonte fornece suas dimensões atuais; nem o INI nem o cache de startup substituem essa consulta. O viewport proporcional é calculado para a área cliente atual do host, com origem atual de apresentação; as faixas excedentes recebem letterbox preto dentro dessa área. Chamadas parciais, sem fonte, reentrantes ou sem viewport válido são encaminhadas sem adaptação. Não há cobertura universal de `Flip`, `BltFast`, interfaces antigas ou todos os caminhos gráficos.

O `SetClipper` da primária (slot **28**) pode usar um callback opcional para redirecionar o **`SetHWnd` real do clipper** do cliente fixo ao host visível. Isso evita o corte direito/inferior quando a apresentação maximizada ultrapassa o HWND do canvas. Não troca as flags originais de `SetCooperativeLevel` nem inventa sucesso de cooperação. O ensaio real verificou a interface completa em cliente físico 1920×991, maior que o HWND-canvas de 1360×765 a 125% DPI.

Um probe nativo com janela/filho próprios confirmou outra exigência: `GetClipList` do host com `WS_CLIPCHILDREN` retornava sucesso, mas **zero retângulos**, pois o filho cobria sua área. Sem esse estilo, retornava a região cliente visível. Por isso o host escalado remove `WS_CLIPCHILDREN` e não apaga seus pixels em `WM_ERASEBKGND`. A imagem usa DirectDraw; o letterbox usa DirectDraw ou o fallback GDI abaixo. Os modos legados não mudam essa política.

O backend observado recusou `COLORFILL` na primária com **`DDERR_EXCEPTION = 0x88760037`**. Retornar esse erro antes do `Blt` da imagem congelava/cortava a apresentação em janelas de outra proporção. O callback opcional `PaintLetterbox` preenche as faixas com GDI, validando que os retângulos ficam **dentro do host próprio**. Após esse fallback funcionar, mudanças posteriores de geometria tentam GDI primeiro, sem insistir no fill nativo incompatível. Se nenhum preenchimento funcionar, retorna-se a falha nativa; se o fallback funcionar, o resultado continua sendo o **HRESULT real do Blt da imagem**, inclusive erros. Não há sucesso gráfico inventado.

`CreateSurface` e `SetCooperativeLevel` também mantêm seus resultados reais. Se `GetSurfaceDesc` falhar, a apresentação é encaminhada ao método original, com diagnóstico de bypass. O código **não falsifica `GetCaps` nem oculta o aviso de 32 bits**. Continua usando o backend DD7/D3D7/Windows original: não porta D3D7 para D3D11 nem baixa/substitui `ddraw.dll`.

### 4. Foco, mouse e mensagens

Não se retorna “ativo” permanentemente nem se descarta `WM_ACTIVATEAPP`. Apenas a perda de foco do main para fora do host desvia da recuperação de foco legada. A saída passa pelo procedimento padrão; controles dentro do host continuam usando o tratamento original. Sistemas com touch, IME, acessibilidade e diálogos modais precisam de testes adicionais.

Na escala, os hooks **`GetMessageA/W` e `PeekMessageA/W`** convertem `MSG.pt` dos caminhos cobertos, inclusive em `PM_NOREMOVE`. Para mensagens de mouse, fazem hit-test virtual nos HWNDs filhos: o alvo físico pode ser `WorkWindow` enquanto a imagem mostra `ButtonBar`. Encaminhar tudo ao main não bastou, pois seu router descarta certos cliques destinados a outro filho. O main recebe `lParam` para conversão única no dispatch; um filho nativo recebe **coordenadas locais virtuais já convertidas**, sem subclass de sua WndProc. A mensagem original na fila não é reescrita.

Wheel é encaminhado ao main e conserva a convenção de tela após a conversão. `GetCursorPos` cobre cliente, filhos, área cliente do host e captura pelo cliente **ou seus filhos**; fora da imagem, a sentinela no canvas é `(-1,-1)`, não uma borda válida. A geometria usa a fonte efetivamente apresentada, quando disponível. Menus/diálogos/Home/Quit foram operados no ensaio final; não é cobertura de todos os controles.

Somente o par legado **retorno de `GetCursorPos` RVA `0x36fc43` → retorno de `SetCursorPos` RVA `0x36fc55`** arma/consome uma conversão inversa **thread-local**. Outros setters mantêm sua semântica original de tela; não se escala indiscriminadamente `SetCursorPos`, inclusive chamadas como `(0,0)` do rebuild. Não há promessa de suporte universal a controles filhos nativos distintos, touch/IME, acessibilidade ou DPI misto.

Esse par usa deltas a partir da posição real, inclusive quando o getter devolve a sentinela externa: delta zero não deve deslocar fisicamente o cursor por arredondamento. Uma guarda de entrega também impede que wheel não consumido, propagado pelo `DefWindowProc` ao pai, seja reenviado recursivamente ao cliente.

Desde a 0.3, `WM_CLOSE`, `WM_DESTROY`, `WM_NCDESTROY` e `WM_DISPLAYCHANGE` têm registros diagnósticos nos caminhos acompanhados. **`WM_DISPLAYCHANGE` continua entregue sem alteração**, inclusive ao ramo nativo que pode solicitar saída. Os hooks cobertos de `PostQuitMessage` e `ExitProcess` registram código e endereço do chamador, depois chamam a API real; a retirada de `WM_QUIT` por `GetMessage` ou `PeekMessage(PM_REMOVE)` também é observada.

O host não posta zero por cima de um quit nativo pendente observado na thread da interface. Esse estado é limpo quando os hooks de `GetMessage`/`PeekMessage(PM_REMOVE)` retiram o quit; quits de workers não o definem. Sem quit nativo pendente observado, o host posta seu próprio `PostQuitMessage(0)`, com registro separado. A cobertura não inclui solicitações/retiradas feitas por caminhos fora dos hooks.

Esses registros de lifecycle/saída e o observador de exceções usam try-lock de melhor esforço e não chamam `OutputDebugString`: podem omitir linhas se o logger estiver ocupado, em vez de esperar pelo lock. Isso não torna a escrita em arquivo assíncrona nem garante ausência de espera por I/O. Não há interceptação para cancelar a saída.

### 5. IAT e carregamento

A DLL usa sua própria IAT original ao chamar APIs reais, evitando recursão. As alterações de IAT ficam no EXE/`Contour.dll`; **a integração também substitui o ponteiro retornado por um proxy COM e escreve os dois dados de layout guardados**, portanto não é correto dizer que altera somente IATs. Os exports originais são pré-resolvidos fora do lock. Um lock curto serializa as trocas de IAT; proteções são restauradas. Não há inline detour de código para o seed, driver ou alteração de vtables nativas; a pausa transitória do launcher é descrita abaixo.

O hook recusa sobrescrever um slot que não aponta nem para a API original esperada nem para o próprio hook, com `InterlockedCompareExchangePointer` para não fazer uma troca incondicional após uma verificação sujeita a corrida. `SetWindowLongPtr` em x86 é alias de header de `SetWindowLong`; não se tenta importar um export inexistente com esse nome.

Limites explícitos: sem IAT de outros plugins, ordinais, delay imports, APIs equivalentes desconhecidas ou `LoadLibraryEx`. O carregamento observado é normal/`LoadLibraryA`.

Os registros de chamadas/exceções atribuem endereço/RVA ao EXE ou a `Contour.dll` apenas dentro dos intervalos de imagem cacheados. Endereços externos são marcados `outside-cached-target-images`; não há enumeração completa de módulos, stack trace ou cobertura universal de saídas. A ausência de uma linha não exclui chamada por outro caminho.

`AddVectoredExceptionHandler` observa primeira chance de violação de acesso, instrução ilegal e divisão inteira por zero. **Sempre retorna `EXCEPTION_CONTINUE_SEARCH`**, sem engolir exceções nem instalar recuperação gráfica. Uma primeira chance pode ser tratada normalmente pelo aplicativo e não prova crash. O log é de melhor esforço e não observa toda classe de falha. A fixture usa uma exceção levantada por software e explicitamente tratada, não uma violação real de acesso à memória.

### 6. Inicialização do processo

O launcher é x86. Inicia um filho próprio sob a API de debug e pausa no entry point PE com um `INT3` temporário de **um byte somente em memória**, restaurando byte original, proteção de página, instruction cache e EIP antes de prosseguir. O endereço deriva do PE/base ASLR observados, não de um offset fixo. A pausa deve ocorrer **depois** de liberar o loader lock, não no primeiro breakpoint do loader. Em seguida desanexa o debugger, carrega a DLL por caminho absoluto, chama o export de inicialização em uma thread remota e só então libera a thread principal. Não resta patch funcional em instruções do EXE.

O endereço remoto de `LoadLibraryW` é calculado a partir do módulo que realmente contém o export resolvido, incluindo forwarders KernelBase, e de seu endereço base observado no filho. Não se presume que o endereço local absoluto seja o endereço remoto. O export próprio é localizado por RVA em uma imagem mapeada sem executar `DllMain` local.

Falhas de inicialização têm timeout; nesse caso somente o novo filho pertencente ao launcher é encerrado. Um buffer de carregamento não é liberado enquanto uma thread remota com timeout ainda poderia acessá-lo. O EXE original no disco permanece intacto. Nenhum processo Lake existente é anexado, modificado ou encerrado.

Na execução normal de produção, **`--wait` espera sem timeout pelo encerramento**, consulta `GetExitCodeProcess`, imprime o resultado e grava `logs/LakeWindowedFix-<PID>-exit.log` com decimal/hexadecimal. O fallback temporário é tentado também após falha de abertura/escrita local, mesmo com o diretório já existente; ambos os destinos usam append, sem truncar registros anteriores. Não mata o Lake por demorar nessa espera. Funciona também com `--native-only`, que não injeta a DLL e, portanto, não gera o log interno do shim. O launcher de fixture continua tendo limite de tempo próprio, separado do comportamento de produção. Código zero não comprova saúde gráfica, e código não zero sozinho não identifica a causa.

## Checklist e lacunas

| Pergunta | Estado |
| --- | --- |
| API gráfica e arquitetura? | Respondida estaticamente no escopo observado: x86, DD7/D3D7, GDI/GDI+ auxiliares |
| Criação de janela e estilos? | Respondida com IAT, registros de classe e assembly |
| Mudança de resolução/exclusividade? | Métodos COM/flags identificados; R3 observa apresentação DD7, não toda a árvore gráfica ou toda chamada de modo |
| Flags internas e resize nativo? | Riscos em R2; R3 mostra rebuild lento e UI ainda cortada. Opt-in legado continua experimental |
| Resize escalado da 0.4? | Operado em OFFLINE a 125% DPI, inclusive arraste/maximização, fonte fixa e 9.315 apresentações bem-sucedidas na amostra; outros ambientes/prolongamento pendentes |
| Seed das dimensões do canvas? | Observado após seleção de modo, com layout/fonte 1088×612; específico do Build 007, sem teste de corrupção no proprietário |
| Causa do encerramento real da 0.2? | **Indeterminada**; log termina após setup, sem exit code, evento correspondente ou dump encontrado |
| Diagnóstico de saída da 0.3? | 133 PASS sintéticos históricos e três saídas normais com código 0 em R3; não explicam o encerramento da 0.2 |
| Código aberto, licença, scripts e reversibilidade? | Implementados no projeto; sem redistribuição proprietária |
| Elimina o travamento de Alt+Tab em Windows 10 **e** 11? | **Não demonstrado**; requer matriz gráfica/manual, inclusive dependências legadas |
| Hardware áudio/rede funciona após a mudança? | Sem validação de hardware; há relato do usuário e continuação offline em R3, não teste de adaptadores/processadores |

## R1 — regressão de visibilidade após seleção de rede (versões 0.1 → 0.2)

O usuário executou a 0.1, viu a seleção de interface de rede, confirmou e não viu a tela principal. Uma consulta **somente-leitura** ao desktop encontrou, no PID histórico `5836`:

- `ContourParent`, título `Lake Controller - Windowed`: `WS_VISIBLE` ausente e `IsWindowVisible == false`.
- Filho `Lake Controller`: `WS_CHILD | WS_VISIBLE` (`0x50000000`), mas `IsWindowVisible == false` por causa do ancestral oculto.
- Portanto, houve criação da janela principal; a moldura do próprio shim impedia sua exibição. Isso não demonstra que o renderer esteja saudável ou que não exista outro problema gráfico.

Relatório local ignorado: `private/window-state-v01-failure.json`, gerado por `scripts/Inspect-WindowState.py`. Não é REA Evidence. Não foram lidos textos de controles de interfaces/dispositivos. A tentativa anterior de consultar memória com acesso somente-leitura foi negada (Win32 5); não houve escalada, suspensão, debugger, alteração de janela/rede ou encerramento dessa instância. A pilha e o ponto exato do loop legado continuam desconhecidos.

Inspeção estática adicional, no mesmo hash de `Contour.dll`:

1. `MkCreatePlugin` (RVA `0x75290`) chama o construtor em `0x75340`.
2. Esse construtor grava a vtable em VA preferida `0x104696c8`; o slot `+0x24` aponta para RVA `0x75860`.
3. O método em `0x75860` posta `0x8032` para o main, depois chama `ShowWindow(main, 1)` em `0x75883`.
4. A tabela de dispatch em `0x24f58`/`0x24e2c` encaminha `0x8032` ao ramo `0x20d1a`, que retorna zero. A subclass conserva esse tratamento antes de agendar o posicionamento.

A fixture passou a reproduzir essa ordem **antes do primeiro GetMessage do EXE**, usando um loop interno de `PeekMessage`. Na DLL 0.1, duas verificações novas falharam (moldura não exibida e inicialização não agendada pelo caminho nativo). Na 0.2, ambas passaram. Logs: `private/regression-v01.txt` e `private/build-validation-v02.txt`.

A 0.2 também passou a registrar etapas em arquivo, recusar outra instância com o mesmo nome de imagem e deixar explícito no console que injeção bem-sucedida não prova abertura da janela. Seus binários foram gravados em `dist-v0.2/`, sem sobrescrever a DLL antiga em uso. O reteste posterior executado pelo usuário está registrado em R2; não validou estabilidade gráfica.

## R2 — encerramento após o setup da 0.2 e diagnóstico na 0.3

O usuário executou a **0.2**, PID histórico `14208`. O log local ignorado `dist-v0.2/logs/LakeWindowedFix-14208.log` registra, em ordem:

1. `Native ShowWindow cmd=1`, com cliente e host visíveis, seguido do agendamento pelo fallback `EXE GetMessage`.
2. `Native ShowWindow cmd=5`.
3. `Initial placement begin` → `Native resize begin` → `Native resize complete`.
4. `Initial placement complete` → `Native windowed host ready`.

Depois, o processo fechou, conforme relato. A investigação não encontrou evento correspondente de Aplicativo 1000/1001/1002 nem arquivo dump. A 0.2 não registrou o código final nem o chamador de saída. **Não é possível distinguir crash de saída normal com essa evidência.** O retorno do rebuild não comprova sucesso do renderer, mas também não demonstra que o rebuild causou o encerramento.

Inspeção estática adicional **local, pefile/Capstone**, no mesmo hash de `Contour.dll`; todos os endereços seguintes são RVAs, não trace de execução nem Evidence REA:

| Caminho | Achado estático e limite |
| --- | --- |
| `WM_SIZE`, handler `0x20120` | Exige estado em `0xc8a834`/`0xc8ac60`, mas alcança teardown global em `0x466f0`, com extensa sequência de `Release` de superfícies. As flags não provam validade de todos os recursos |
| Recriação em `0x433d0` | Limpa/recria o renderer por `0x48a80` e descarta HRESULT negativo; retornar ao shim não equivale a reconstrução bem-sucedida |
| Helper `0x16de0` | Desreferencia o dispositivo global `0xc369b0` sem null check no trecho inspecionado. É risco condicionado ao estado real, que não foi capturado |
| `WM_DISPLAYCHANGE`, ramo `0x20b65` | Pode chamar `0x303b0`, que executa `PostQuitMessage(0)` em `0x303c7`, inclusive em windowed. Não há registro de que esse caminho tenha ocorrido no PID 14208 |
| `WM_DESTROY`, ramo windowed `0x23115` | Não chama `PostQuitMessage` nesse ramo; separadamente, o host do shim chama seu próprio `PostQuitMessage(0)` quando não há quit nativo pendente observado na thread da interface |

Esses achados demonstram **riscos e rotas possíveis**, não a causa do encerramento observado. Não justificam descartar `WM_DISPLAYCHANGE`, impedir `ExitProcess`/`PostQuitMessage` ou engolir exceções como suposta correção.

A 0.3 foi compilada em `dist-v0.3/`, preservando `dist-v0.2/`, com tamanho fixo por padrão e observadores de saída. O log local ignorado `private/build-validation-v03.txt` registra **133 PASS, zero falhas** em três suítes sintéticas após corrigir preservação de quit, contenção do logger e fallback do relatório; não é teste GPU/Lake. Essas correções não identificam a causa do encerramento da 0.2. O ensaio real posterior da 0.3 está em R3, sem validar resize livre nem estabilidade universal.

## R3 — ensaio real autorizado da 0.3 e motivação da 0.4

Fonte: relatório local ignorado **`private/runtime-20261024-02/REPORT.md`**, de 2026-10-24, patch 0.3/commit histórico `e1227e2`. O agente foi autorizado a operar essas instâncias de teste: modo **OFFLINE**, aviso gráfico e sessão vazia. Não conectou/controlou processadores, alterou adaptadores ou usou Save/Recall de presets. A execução não era uma sandbox; o aplicativo podia gravar seu estado habitual. O relato do usuário sobre rede/offline não foi convertido em teste de hardware ou causa universal dos fechamentos.

Ambiente medido: monitor 1920×1080, área útil 1920×1020, DPI efetivo **120 (125%)**, processo **DPI-unaware (`GetProcessDpiAwareness == 0`)**, `GetDpiForWindow == 96`. Havia outro monitor de 800×600; o ensaio não valida DPI misto nem todos os monitores. Não foi estabelecido qual adaptador físico o renderer selecionou.

| Execução histórica | Geometria/apresentação observada | Operações e limite |
| --- | --- | --- |
| PID `6632`, 0.3 fixa | Fonte lógica **1536×863**, cliente físico **1920×1078**, moldura **1928×1105**; barra inferior cortada. **4.655 `Blt` da primária, todos HRESULT 0** | Menus desenharam/responderam; um movimento, um ciclo minimizar/restaurar e um Alt+Tab/retorno sem trava. Quit/Yes, exit 0 |
| PID `12380`, `--native-only --wait` | Sem host/parent; movida a (100,40) físicos, a janela mudou mas o destino continuou **(0,0)–(1536,863)**. **1.687 `Blt`, todos HRESULT 0**, com deslocamento visível | Apenas flags nativas não deram apresentação móvel correta. Quit/Yes, exit 0 |
| PID `13272`, cópia 0.3 com `NativeResize=1` | Renderer/layout **1024×768**, cliente físico **1280×960**, moldura **1300×1009** cabível; comandos da direita ainda cortados | Rebuild inicial **4.812 ms**, ao restaurar **4.938 ms**. Desenho voltou após o rebuild; fechamento pelo X, exit 0 |

As três execuções acima terminaram normalmente com código 0; isso não elimina os defeitos visuais. Uma tentativa inicial interrompida pelo timeout de 120 s da ferramenta foi excluída das conclusões de estabilidade. Não houve ensaio prolongado, arraste contínuo, maximização ou matriz de dimensões/drivers. Uma imagem quase preta durante o rebuild não foi classificada como crash.

O cache em RVA `0xc8b724` ainda dizia 1536×863 após o resize: é snapshot de inicialização, **não descrição atual da superfície**, e não prova sozinho a causa do recorte. Da mesma forma, `Blt` com HRESULT 0 comprova o retorno da API observado, não alinhamento/input corretos nem todos os pixels.

### Aviso de 32 bits e primeira chance

A flag `0xc8a892` foi observada em **0**, com display 32 bpp, nas três execuções. O bit consultado é `DDCKEYCAPS_SRCBLTCLRSPACE` (`0x400`), suporte legado a intervalo de color key de origem em `Blt`, **não capacidade genérica de usar 32 bpp**. O aviso apareceu com/sem shim e permitiu continuar offline. Nem a checagem nem seu resultado foram falsificados.

No PID 6632, uma AV de primeira chance foi mapeada a **`D3D9on12.dll+0x78890`**, e o aplicativo continuou apresentando milhares de frames. Não é prova de crash, handler identificado ou defeito de driver. O módulo observado também não transforma a análise estática do renderer DD7/D3D7 em uma alegação de port D3D11.

### Proveniência, integridade e privacidade

O relatório guarda os IDs/exports REA da observação Frida. Seus envelopes têm `subject: null`; a correlação com o alvo usa separadamente allowlist do launcher, PID/caminho/horário de criação e manifesto de hashes. Leituras sem suspensão podem conter transientes; as contagens de `Blt` cobrem somente o intervalo conectado, não startup completo nem toda a árvore gráfica. A instrumentação não substituiu argumentos, retornos ou variáveis e foi desanexada antes do fechamento.

Foram preservados os hashes do EXE original, `Contour.dll` e launcher/DLL/INI 0.3. Capturas, logs, dumps e relatórios integrais continuam **privados e ignorados**; especialmente o seletor de rede pode expor IPs e não deve ser publicado. Esta documentação resume os resultados, sem embutir esses artefatos ou originais.

R3 fundamenta a separação **canvas/layout fixo → apresentação escalada**, não sua aprovação. A saída 0.4 fica separada de `dist-v0.3/`; o ensaio final está em R4 e a matriz de ambientes ainda não cobertos em `VALIDATION.md`.

## R4 — bloqueio nativo da 0.4 e substituição por proxies

O probe local **`private/real_dd_probe.cpp`** registrou `QueryInterface(IDirect3D7)` com **HRESULT 0 antes** da troca de vtable nativa, **`0x80004002` (`E_NOINTERFACE`) depois**, e **`SetCooperativeLevel = 0x80004005` (`E_FAIL`)**. A estratégia de clone/troca de `lpVtbl` não preservou o comportamento do DirectDraw nativo, embora passasse nos objetos COM falsos.

Três startups intermediários do Lake 0.4 terminaram com **código 0 antes da interface**, reprovando o critério de abertura. Isso não é crash de GPU demonstrado nem esclarece o encerramento histórico da 0.2. Esses candidatos falharam antes do seed de layout, que foi observado posteriormente no ensaio final.

A correção é o proxy completo descrito acima: `this` nativo em todos os forwarders, identidade nas QI cobertas, D3D7/offscreen nativos, referências próprias do wrapper e nenhum `lpVtbl` nativo alterado. O probe real revisado confirmou **QI D3D7 = 0, cooperação = 0, proxy distinto e tabela nativa intacta**. Probes separados exercitaram dispositivo HAL/superfícies em 1536×864 e 1920×1080 e pixels de apresentação reduzida; não substituem a UI real.

O build x86 Zig final passou em **648 verificações, zero falhas**, distribuídas em seis suítes (contagens em `VALIDATION.md`). A pedido do usuário, o canvas foi reduzido de 1536×864 para **1088×612**, cerca de 29% por dimensão, sem alterar a janela inicial 1024×576 lógica. O ensaio real encontrou e corrigiu o fill da primária e o hit-test dos filhos antes da validação final.

A sessão final histórica **PID 10148**, Windows 11 Home Single Language 10.0.26300 x64, DPI 120/125%, processo DPI-unaware, abriu em OFFLINE, confirmou o aviso gráfico e escolheu No para sessão vazia. Foram operados Modules/Home, wheel, movimento, arraste de borda, maximizar/restaurar, minimizar/restaurar e um Alt+Tab/retorno. Capturas intermediárias mostraram a interface inteira em clientes físicos 1280×720, 800×600, 600×450, 520×410, 400×225 e maximizado 1920×991. O texto do tamanho mínimo continua pequeno, embora sem recorte.

A amostra Frida final registrou **9.315 apresentações, todas HRESULT 0, zero CreateSurface do DD7 acompanhado e fonte/layout 1088×612**. Cobre somente chamadas pós-attach dos objetos instrumentados, não toda a árvore gráfica; não houve substituição de argumentos/retornos na observação. Resize mais resposta `WM_NULL` levou 0–46 ms nos registros, não uma medição de GPU/áudio. O logger não registrou rebuild nativo nessa sessão. Após desanexar Frida, o Lake encerrou por **Quit → Yes, exit 0**, sem processo restante.

Um candidato anterior, fechado durante modal de startup, ficou sem janelas e exigiu limpeza forçada **0xEC04**: não conta como saída normal. O host final adia esse fechamento até o setup, com teste sintético da guarda/veto. O candidato reduzido seguinte encerrou pelo X, exit 0; a sessão final encerrou pela UI interna. Durante bloqueio do Windows, o operador recusou input e só continuou após o usuário desbloquear manualmente.

O adaptador gráfico físico não foi determinado. Windows 10, outros drivers/DPI, touch/IME/texto, todos os controles, perda de dispositivo, RDP/hotplug, áudio/rede e estabilidade prolongada permanecem sem validação. Manifestos correlacionam os hashes do build final e a integridade dos originais/0.3; probes, capturas, logs e Evidence integrais permanecem privados e ignorados, sem publicação no repositório.
