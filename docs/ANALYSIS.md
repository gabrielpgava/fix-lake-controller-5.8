# Análise estática e decisões técnicas

Data: 2026-10-24. Versão atual: **0.3, experimental, tamanho fixo diagnóstico por padrão**; resize livre continua pendente. Escopo estático: os quatro artefatos abaixo da instalação fornecida, sem executá-los para a inspeção. R1 registra observação passiva da 0.1 aberta pelo usuário; R2 registra o encerramento posterior da 0.2 também executada pelo usuário e a inspeção estática adicional. O agente não iniciou o Lake real. Endereços VA usam a base preferida do PE, não endereços garantidos em runtime. `Contour.dll`: base `0x10000000`, máquina `0x014c` (x86).

## Identidade

| Arquivo | SHA-256 |
| --- | --- |
| `Lake Controller.exe` | `e9c9d291f9c20732904dd6b943737537c242508e546cda2afb1f72dfc2374713` |
| `Contour.dll` | `bb7d4126a83b775968d49ce52f885b588561838d97ffe062ec46a8208b94c283` |
| `StdGraphics.dll` | `d20c8500eeddf3038e01a1b25789ec5afca10430acb8b4a4219c1092a143e6e4` |
| `DaylightGraphics.dll` | `ffa34b4c6ee0896a7cec0fe85310e5f2a5ed31ffbad6c4da1d40aa5f4e74cb7e` |

`Contour.dll` contém a string `Lake Controller 5.8 Build 007`. O launcher valida os dois primeiros hashes. Não há patch por offsets fixos no produto: os RVAs abaixo servem para auditoria estática; IATs/entry point são encontrados nos cabeçalhos PE em memória.

## Ferramentas e proveniência

- REA estava conectado. Os providers nativos estavam indisponíveis: Ghidra sem `GHIDRA_INSTALL_DIR`/runtime configurados; IDA e Kuna não configurados; Hopper não suportado no host Windows. Não houve descompilação nem alegação de CFG completo. Não foi instalado/reconfigurado um provider para contornar isso.
- REA `inspect_pe_resources` confirmou PE x86 e identidade do EXE; Evidence ID: `ev_8ccab34d400a5af87127ee0e605ac6182c23455895ccc93268cc42e46a6a29c5`. Cobertura: 16 entradas, 6 recursos. Limites desse Evidence: inventário estático de recursos; payloads exceto grupos de ícones opacos; nenhuma execução, fallback de idioma ou renderização observados. **Esse Evidence não fundamenta sozinho a identificação da API gráfica.**
- As observações de imports, exports, strings, referências candidatas e trechos assembly vêm de inspeção **local** com Python/pefile 2024.8.26 e Capstone 5.0.7. O script reprodutível é `scripts/Inspect-Target.py`; cada registro tem SHA-256 e `record_id` local, sem fingir ser Evidence REA.
- Os achados adicionais de R2 são inspeção estática local pefile/Capstone e leitura de log local ignorado, não saída de REA. Não há trace, pilha de falha ou dump que ligue esses caminhos ao encerramento real da 0.2.
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

As opções são procuradas como substrings, em minúsculas, não como um parser moderno de argumentos. O launcher não repassa argumentos arbitrários: usa apenas as duas opções verificadas. `nonfullscreen` contém `fullscreen`, mas o código usa `else`/salto após reconhecer `nonfullscreen`; não aciona os dois ramos.

A mesma tabela encaminha `WM_SHOWWINDOW` (`0x18`) ao ramo `0x20a41`, que atualiza a flag usada para habilitar resize. Não se infere esse comportamento a partir de `WM_ACTIVATEAPP`: essa mensagem cai no procedimento padrão no switch observado.

GDI, GDI+ e `d3dxof.dll` também estão presentes. Isso não transforma o renderer em D3D9. A ausência de imports D3D9 não exclui absolutamente todos os caminhos possíveis de plugins.

## Implementação escolhida

### 1. Seleção nativa de modo

Usar as flags desde a criação do processo é menos frágil que alterar globais por RVA ou interceptar só `SetDisplayMode`. Não são suprimidas chamadas COM mantendo o aplicativo convencido de que tem superfícies exclusivas. A eficácia sobre todas as GPUs continua dependente de runtime.

### 2. Host padrão e renderer filho

O host usa a própria classe `ContourParent`, registrada antes da criação principal. Seu `WM_SIZE` contém a reconstrução de layout/superfícies, mas R2 identificou riscos de teardown global e tratamento de erros. A área gráfica mantém seu WndProc original e vira `WS_CHILD`, seguindo o modelo de hospedagem que o próprio binário contém. Não se troca a WndProc inteira nem se inventa um render loop.

**Na 0.3, `[compatibility] NativeResize=0` é o padrão.** O shim conserva o tamanho original do framebuffer, não envia `WM_SIZE` inicial e não encaminha o resize do host ao procedimento nativo de rebuild. Remove `WS_THICKFRAME`/`WS_MAXIMIZEBOX` e impede os comandos de resize/maximizar. `Width`/`Height` não são aplicados nesse modo. O posicionamento não reduz a janela para caber na área útil: ela pode ultrapassá-la ou ter recorte. É uma redução diagnóstica da interferência no renderer, **não uma correção completa do objetivo de resize livre nem uma garantia de estabilidade**. Isso não impede todo rebuild que o próprio aplicativo possa iniciar por outros caminhos.

Na 0.1/0.2, o hook de `GetMessageA/W` era aplicado somente à IAT do EXE. Na 0.3, a observação de `GetMessageA/W` e `PeekMessageA/W` alcança os imports do EXE e de `Contour.dll`, além do lookup dinâmico coberto. **Somente chamadas de `GetMessage` cuja origem esteja na imagem do EXE podem agendar o fallback de setup**; `PeekMessage` não o agenda. No EXE inspecionado, `GetMessageA` é chamado em VA `0x004057d0`/`0x004057ff`, depois do retorno da inicialização do plugin e do método seguinte. Na 0.1, esse era o único marco que podia revelar o host: essa dependência foi insuficiente no uso real (R1).

Na 0.2, o host acompanha imediatamente o pedido nativo de visibilidade do cliente, mantendo o rebuild desabilitado nesse momento. O posicionamento inicial é enfileirado após o tratamento original da mensagem nativa `0x8032`, com o `GetMessage` externo apenas como fallback. A mensagem privada é postada ao **cliente**, que a encaminha à moldura: isso também atende loops que filtram por HWND e não retirariam mensagens postadas apenas ao novo parent. Bombear `PeekMessage` antes desse pedido/notificação não habilita resize. Não é usado um timer arbitrário nem se presume que apenas criar a janela signifique que o startup terminou.

A 0.3 mantém esse hook de visibilidade e o agendamento, mas só permite rebuild com **`NativeResize=1`, EXPERIMENTAL**. A propriedade `LakeWindowedFix.HostSetupCompleted` e a linha `Host setup complete (not a renderer health check)` indicam apenas conclusão do setup da janela. Não verificam HRESULTs nativos, superfícies ou saúde da GPU; tampouco o antigo `Native windowed host ready` da 0.2 fazia essa verificação.

O fechamento da moldura envia `WM_CLOSE` ao main original e respeita eventual veto. A moldura é removida após a destruição do filho, sem substituir sua confirmação/rotina de cleanup por `DestroyWindow` direto.

O host é criado antes da instalação de sua subclass. Mensagens de criação podem, portanto, alcançar o procedimento nativo antes da guarda do shim; a fixture não demonstra ausência de rebuild nessa fase no Lake. Não foi observado rebuild ou crash nesse caminho. A política fixa não constitui uma garantia universal contra reconstruções iniciadas pelo aplicativo.

Somente no opt-in `NativeResize=1`, `Width`/`Height` voltam a definir o tamanho inicial e o resize é coalescido durante o arraste até `WM_EXITSIZEMOVE`, filtrando tamanhos zero/minimizados. A reconstrução original pode consumir tempo ou falhar; não há promessa de latência zero. Uma trava existente dentro do renderer não é corrigida pelo shim.

### 3. Foco e mensagens

Não se retorna “ativo” permanentemente nem se descarta `WM_ACTIVATEAPP`. Apenas a perda de foco do main para fora do host desvia da recuperação de foco legada. A saída passa pelo procedimento padrão; controles dentro do host continuam usando o tratamento original. Sistemas com touch, IME, acessibilidade e diálogos modais precisam de testes adicionais.

Na 0.3, `WM_CLOSE`, `WM_DESTROY`, `WM_NCDESTROY` e `WM_DISPLAYCHANGE` têm registros diagnósticos nos caminhos acompanhados. **`WM_DISPLAYCHANGE` continua entregue sem alteração**, inclusive ao ramo nativo que pode solicitar saída. Os hooks cobertos de `PostQuitMessage` e `ExitProcess` registram código e endereço do chamador, depois chamam a API real; a retirada de `WM_QUIT` por `GetMessage` ou `PeekMessage(PM_REMOVE)` também é observada.

O host não posta zero por cima de um quit nativo pendente observado na thread da interface. Esse estado é limpo quando os hooks de `GetMessage`/`PeekMessage(PM_REMOVE)` retiram o quit; quits de workers não o definem. Sem quit nativo pendente observado, o host posta seu próprio `PostQuitMessage(0)`, com registro separado. A cobertura não inclui solicitações/retiradas feitas por caminhos fora dos hooks.

Esses registros de lifecycle/saída e o observador de exceções usam try-lock de melhor esforço e não chamam `OutputDebugString`: podem omitir linhas se o logger estiver ocupado, em vez de esperar pelo lock. Isso não torna a escrita em arquivo assíncrona nem garante ausência de espera por I/O. Não há interceptação para cancelar a saída.

### 4. IAT e carregamento

A DLL usa sua própria IAT original ao chamar APIs reais, evitando recursão. Apenas o EXE e a instância carregada de `Contour.dll` são modificados. Os exports originais são pré-resolvidos fora do lock. Um lock curto serializa gravações atômicas de ponteiros, sem chamar APIs do loader, chamadas de janela ou rebuild gráfico enquanto o mantém. Proteções de páginas são restauradas. Não há inline detours, sobrescrita de vtables COM ou driver.

O hook recusa sobrescrever um slot que não aponta nem para a API original esperada nem para o próprio hook, com `InterlockedCompareExchangePointer` para não fazer uma troca incondicional após uma verificação sujeita a corrida. `SetWindowLongPtr` em x86 é alias de header de `SetWindowLong`; não se tenta importar um export inexistente com esse nome.

Limites explícitos: sem IAT de outros plugins, ordinais, delay imports, APIs equivalentes desconhecidas ou `LoadLibraryEx`. O carregamento observado é normal/`LoadLibraryA`.

Os registros de chamadas/exceções atribuem endereço/RVA ao EXE ou a `Contour.dll` apenas dentro dos intervalos de imagem cacheados. Endereços externos são marcados `outside-cached-target-images`; não há enumeração completa de módulos, stack trace ou cobertura universal de saídas. A ausência de uma linha não exclui chamada por outro caminho.

`AddVectoredExceptionHandler` observa primeira chance de violação de acesso, instrução ilegal e divisão inteira por zero. **Sempre retorna `EXCEPTION_CONTINUE_SEARCH`**, sem engolir exceções nem instalar recuperação gráfica. Uma primeira chance pode ser tratada normalmente pelo aplicativo e não prova crash. O log é de melhor esforço e não observa toda classe de falha. A fixture usa uma exceção levantada por software e explicitamente tratada, não uma violação real de acesso à memória.

### 5. Inicialização do processo

O launcher é x86. Inicia um filho próprio sob a API de debug e pausa no entry point PE com um `INT3` temporário de **um byte somente em memória**, restaurando byte original, proteção de página, instruction cache e EIP antes de prosseguir. O endereço deriva do PE/base ASLR observados, não de um offset fixo. A pausa deve ocorrer **depois** de liberar o loader lock, não no primeiro breakpoint do loader. Em seguida desanexa o debugger, carrega a DLL por caminho absoluto, chama o export de inicialização em uma thread remota e só então libera a thread principal. Não resta patch funcional em instruções do EXE.

O endereço remoto de `LoadLibraryW` é calculado a partir do módulo que realmente contém o export resolvido, incluindo forwarders KernelBase, e de seu endereço base observado no filho. Não se presume que o endereço local absoluto seja o endereço remoto. O export próprio é localizado por RVA em uma imagem mapeada sem executar `DllMain` local.

Falhas de inicialização têm timeout; nesse caso somente o novo filho pertencente ao launcher é encerrado. Um buffer de carregamento não é liberado enquanto uma thread remota com timeout ainda poderia acessá-lo. O EXE original no disco permanece intacto. Nenhum processo Lake existente é anexado, modificado ou encerrado.

Na execução normal de produção, **`--wait` espera sem timeout pelo encerramento**, consulta `GetExitCodeProcess`, imprime o resultado e grava `logs/LakeWindowedFix-<PID>-exit.log` com decimal/hexadecimal. O fallback temporário é tentado também após falha de abertura/escrita local, mesmo com o diretório já existente; ambos os destinos usam append, sem truncar registros anteriores. Não mata o Lake por demorar nessa espera. Funciona também com `--native-only`, que não injeta a DLL e, portanto, não gera o log interno do shim. O launcher de fixture continua tendo limite de tempo próprio, separado do comportamento de produção. Código zero não comprova saúde gráfica, e código não zero sozinho não identifica a causa.

## Checklist e lacunas

| Pergunta | Estado |
| --- | --- |
| API gráfica e arquitetura? | Respondida estaticamente no escopo observado: x86, DD7/D3D7, GDI/GDI+ auxiliares |
| Criação de janela e estilos? | Respondida com IAT, registros de classe e assembly |
| Mudança de resolução/exclusividade? | Respondida parcialmente: métodos COM/flags identificados; nenhuma chamada runtime capturada |
| Flags internas e resize nativo? | Parser/handlers verificados; riscos em R2. Resize desligado por padrão na 0.3; opt-in experimental ainda sem validação gráfica |
| Causa do encerramento real da 0.2? | **Indeterminada**; log termina após setup, sem exit code, evento correspondente ou dump encontrado |
| Diagnóstico de saída da 0.3? | Implementado e testado sinteticamente; coleta no Lake real com `--wait` pendente |
| Código aberto, licença, scripts e reversibilidade? | Implementados no projeto; sem redistribuição proprietária |
| Elimina o travamento de Alt+Tab em Windows 10 **e** 11? | **Não demonstrado**; requer matriz gráfica/manual, inclusive dependências legadas |
| Hardware áudio/rede funciona após a mudança? | **Não testado**; fora dos testes sintéticos e sem alteração intencional dessas funções |

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

A 0.3 foi compilada em `dist-v0.3/`, preservando `dist-v0.2/`, com tamanho fixo por padrão e observadores de saída. O log local ignorado `private/build-validation-v03.txt` registra **133 PASS, zero falhas** em três suítes sintéticas após corrigir preservação de quit, contenção do logger e fallback do relatório; não é teste GPU/Lake. Essas correções não identificam a causa do encerramento da 0.2. A próxima evidência necessária é o reteste pelo usuário com `--wait` e, se continuar fechando, a comparação separada `--native-only --wait`, conforme o README. O comportamento real da 0.3 e o objetivo de resize livre continuam pendentes.
