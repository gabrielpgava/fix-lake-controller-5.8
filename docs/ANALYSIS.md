# Análise estática e decisões técnicas

Data: 2026-10-24. Escopo estático: os quatro artefatos abaixo da instalação fornecida, sem executá-los para a inspeção. A seção R1 acrescenta observação passiva de uma instância posteriormente aberta pelo usuário; o agente não iniciou o Lake real. Endereços VA usam a base preferida do PE, não endereços garantidos em runtime. `Contour.dll`: base `0x10000000`, máquina `0x014c` (x86).

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
| A9 | Sem parent explícito, main recebe `0x80080000 = WS_POPUP | WS_SYSMENU` | criação em RVA `0x302a1`–`0x30319`; com retângulo/parent fornecidos, há outro ramo `WS_CHILD` |
| A10 | Classe `ContourParent` usa WndProc RVA `0x20120`, com rebuild nativo em resize | registro em RVA `0x301ed`; nome RVA `0x4650ac`; handler `0x20120`–`0x2027a`: ignora `SIZE_MINIMIZED`/`SIZE_MAXHIDE` e estado não pronto, chama `MoveWindow(main,0,0,w,h)`, atualiza dimensões e refaz recursos sob sincronização nativa. A proteção adicional contra 0×0 é do shim |
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

O host usa a própria classe `ContourParent`, registrada antes da criação principal. Seu `WM_SIZE` já sabe reconstruir layout/superfícies. A área gráfica mantém seu WndProc original e vira `WS_CHILD`, seguindo o modelo de hospedagem que o próprio binário contém. Isso evita trocar a WndProc inteira, inventar um render loop ou manter dimensões de framebuffer diferentes da janela.

O hook de `GetMessageA/W` é aplicado **somente à IAT do EXE**, não à de `Contour.dll` nem ao lookup dinâmico dessa API. No EXE inspecionado, `GetMessageA` é chamado em VA `0x004057d0`/`0x004057ff`, depois do retorno da inicialização do plugin e do método seguinte. Na 0.1, esse era o único marco que podia revelar o host: essa dependência foi insuficiente no uso real (R1).

Na 0.2, o host acompanha imediatamente o pedido nativo de visibilidade do cliente, mantendo o rebuild desabilitado nesse momento. O posicionamento inicial é enfileirado após o tratamento original da mensagem nativa `0x8032`, com o `GetMessage` externo apenas como fallback. A mensagem privada é postada ao **cliente**, que a encaminha à moldura: isso também atende loops que filtram por HWND e não retirariam mensagens postadas apenas ao novo parent. Bombear `PeekMessage` antes desse pedido/notificação não habilita resize. Não é usado um timer arbitrário nem se presume que apenas criar a janela signifique que o startup terminou.

O fechamento da moldura envia `WM_CLOSE` ao main original e respeita eventual veto. A moldura é removida após a destruição do filho, sem substituir sua confirmação/rotina de cleanup por `DestroyWindow` direto.

O resize é coalescido durante o arraste. A reconstrução original pode consumir tempo; não há promessa de latência zero. Uma trava existente dentro do renderer não é corrigida magicamente pelo shim.

### 3. Foco e mensagens

Não se retorna “ativo” permanentemente nem se descarta `WM_ACTIVATEAPP`. Apenas a perda de foco do main para fora do host desvia da recuperação de foco legada. A saída passa pelo procedimento padrão; controles dentro do host continuam usando o tratamento original. Sistemas com touch, IME, acessibilidade e diálogos modais precisam de testes adicionais.

### 4. IAT e carregamento

A DLL usa sua própria IAT original ao chamar APIs reais, evitando recursão. Apenas o EXE e a instância carregada de `Contour.dll` são modificados. Os exports originais são pré-resolvidos fora do lock. Um lock curto serializa gravações atômicas de ponteiros, sem chamar APIs do loader, chamadas de janela ou rebuild gráfico enquanto o mantém. Proteções de páginas são restauradas. Não há inline detours, sobrescrita de vtables COM ou driver.

O hook recusa sobrescrever um slot que não aponta nem para a API original esperada nem para o próprio hook, com `InterlockedCompareExchangePointer` para não fazer uma troca incondicional após uma verificação sujeita a corrida. `SetWindowLongPtr` em x86 é alias de header de `SetWindowLong`; não se tenta importar um export inexistente com esse nome.

Limites explícitos: sem IAT de outros plugins, ordinais, delay imports, APIs equivalentes desconhecidas ou `LoadLibraryEx`. O carregamento observado é normal/`LoadLibraryA`.

### 5. Inicialização do processo

O launcher é x86. Inicia um filho próprio sob a API de debug e pausa no entry point PE com um `INT3` temporário de **um byte somente em memória**, restaurando byte original, proteção de página, instruction cache e EIP antes de prosseguir. O endereço deriva do PE/base ASLR observados, não de um offset fixo. A pausa deve ocorrer **depois** de liberar o loader lock, não no primeiro breakpoint do loader. Em seguida desanexa o debugger, carrega a DLL por caminho absoluto, chama o export de inicialização em uma thread remota e só então libera a thread principal. Não resta patch funcional em instruções do EXE.

O endereço remoto de `LoadLibraryW` é calculado a partir do módulo que realmente contém o export resolvido, incluindo forwarders KernelBase, e de seu endereço base observado no filho. Não se presume que o endereço local absoluto seja o endereço remoto. O export próprio é localizado por RVA em uma imagem mapeada sem executar `DllMain` local.

Falhas têm timeout; somente o novo filho pertencente ao launcher é encerrado. Um buffer de carregamento não é liberado enquanto uma thread remota com timeout ainda poderia acessá-lo. O EXE original no disco permanece intacto. Nenhum processo Lake existente é anexado, modificado ou encerrado.

## Checklist e lacunas

| Pergunta | Estado |
| --- | --- |
| API gráfica e arquitetura? | Respondida estaticamente no escopo observado: x86, DD7/D3D7, GDI/GDI+ auxiliares |
| Criação de janela e estilos? | Respondida com IAT, registros de classe e assembly |
| Mudança de resolução/exclusividade? | Respondida parcialmente: métodos COM/flags identificados; nenhuma chamada runtime capturada |
| Flags internas e resize nativo? | Parser/handlers verificados estaticamente; eficácia gráfica real ainda não observada |
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

A 0.2 também passa a registrar etapas em arquivo, recusar outra instância com o mesmo nome de imagem e deixar explícito no console que injeção bem-sucedida não prova abertura da janela. Os novos binários foram gravados em `dist-v0.2/`, sem sobrescrever a DLL antiga em uso. **O reteste gráfico real da 0.2 ainda depende do usuário.**
