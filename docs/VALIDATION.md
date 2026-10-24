# Validação e limites

**Versão 0.3: experimental, tamanho fixo diagnóstico por padrão (`NativeResize=0`).** Os testes abaixo não validam o Lake real nem cumprem o objetivo de resize livre. O modo `NativeResize=1` é um opt-in experimental separado.

## Ambiente desta implementação

- Host: Windows 11 Home Single Language, `10.0.26300`, 64-bit.
- Toolchain disponível usada: Zig 0.14.1, target `x86-windows-gnu`, C++17.
- Inspeção: Python 3.12.8, pefile 2024.8.26, Capstone 5.0.7.
- MSVC/MinGW e Windows 10: instruções fornecidas; execução não observada neste ambiente.
- O agente não executou os binários Lake. Na investigação da 0.1, houve consulta passiva às janelas de uma instância aberta pelo usuário, sem interagir com rede/hardware ou alterar/encerrar o processo. Depois o usuário executou a 0.2, que fechou após o setup, sem causa determinada. **A 0.3 ainda precisa de reteste real pelo usuário; funcionamento gráfico/áudio não foi validado.**

## Testes automáticos

Resultados registrados em 2026-10-24, sem executar o Lake:

### Atualização 0.3

Build Release x86 Zig concluído após as correções de diagnóstico; binários em **`dist-v0.3/`**, sem sobrescrever `dist-v0.2/`. O log local ignorado `private/build-validation-v03.txt` contém **133 verificações PASS, zero falhas**:

| Suíte | PASS | Falhas | O que comprova no alvo sintético |
| --- | --- | --- | --- |
| `NativeResize=0` | 65 | 0 | Tamanho original preservado, sem rebuild inicial/por arraste e sem controles de resize/maximizar; visibilidade e diagnósticos |
| `NativeResize=1` | 63 | 0 | Opt-in de resize/rebuild simulado, coalescência e regressões de visibilidade |
| Saída intencional `23` com `--wait` | 5 | 0 | Launcher observa a saída não zero e verifica o arquivo com código decimal/hexadecimal |

As contagens incluem verificações do launcher sintético. A saída 23 da fixture é **intencional**, não falha nem crash; o teste passa ao observá-la corretamente. As duas suítes de janela também verificam que `PostQuitMessage(7)` chega intacto a `WM_QUIT`, sem o shim substituir o código.

Regressões finais: quit 23 postado no `WM_DESTROY` nativo permanece 23 após o cleanup do host; depois de consumido, não deixa estado obsoleto, e quit 99 de uma worker não impede o host da interface de postar zero. Um SRWLOCK próprio mantido por outra thread verifica que o helper de logging best-effort rejeita contenção em menos de 1000 ms; não força o lock privado da DLL nem testa toda espera possível de I/O. O launcher mantém um log local próprio aberto sem compartilhamento de escrita e confirma fallback em `%TEMP%`, código decimal/hex e conteúdo local preservados, sem alterar ACLs.

Os logs das fixtures confirmaram **zero rebuilds** no modo fixo e **cinco pares begin/complete** no modo experimental. Esses pares só medem o procedimento sintético, não reconstruções gráficas reais.

`tests/Run-Fixture.ps1` cria uma pasta nova por execução sob o diretório de build (`build-zig/test-runs/<GUID>/` nesta toolchain), copia **apenas artefatos sintéticos** e escreve um INI próprio. Assim, as três suítes não sobrescrevem a configuração/DLL umas das outras nem usam a instalação real. O build as executa por padrão; os nomes CTest são `windows_safe_no_rebuild`, `windows_experimental_resize` e `windows_exit_observer`.

Para repetir somente esses testes com os artefatos sintéticos já compilados:

```powershell
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 0
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 1
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-zig -NativeResize 0 -ExpectedExit 23
```

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

### Cobertura comum e limites

O build padrão compila uma aplicação Win32 inteiramente própria (`LakeFixFixture.exe`), uma **`Contour.dll` sintética** de teste e um launcher de teste (`LakeFixInjectionTest.exe`). O launcher de teste reutiliza o fonte de injeção de produção, mas só inicia o nome fixo da fixture. O launcher de produção mantém sua allowlist SHA-256 e não contém esse modo de teste. A DLL sintética não contém código Lake, nunca é instalada por este projeto e **não deve ser copiada para a instalação real**.

Cobertura sintética, respeitando a política de resize selecionada:

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

Esses testes **não** criam superfícies DirectDraw/Direct3D7 nem exercitam GPU/driver ou o teardown/renderer proprietário identificado estaticamente. Não validam mouse/touch no Lake, dependências legadas, perda de dispositivo, latência de áudio ou correção do travamento/encerramento real. Preserve logs e estado final **localmente**, sem versioná-los; não tratar “compilou” ou “133 PASS” como “testado no Lake”.

O registro histórico da 0.1 inclui análise sintática dos scripts anteriores. Os instaladores não foram executados contra a pasta real protegida do Lake. MSVC, MinGW GCC e Windows 10 continuam sem execução observada neste ambiente.

## Matriz gráfica/manual ainda pendente

Faça os testes em uma instalação licenciada, com presets/configuração de teste e sem processadores de áudio em produção. Registre versão/hash do shim, `NativeResize`, Windows 10 ou 11, GPU/driver, monitores e DPI. Não misture resultados do modo fixo com os de resize experimental.

### Primeiro: padrão diagnóstico `NativeResize=0`

Use o comando **0.3 com `--wait`** do README. Se continuar fechando, compare `--native-only --wait` em execução separada, sem hooks/host. Essa comparação também executa o Lake real e não determina sozinha a causa; guarde o código decimal/hexadecimal e os trechos relevantes dos logs localmente.

| Caso | Resultado esperado | Estado |
| --- | --- | --- |
| Abrir pelo launcher, selecionar rede e permanecer aberto | Host com título/bordas, framebuffer original, sem rebuild inicial pelo shim; verificar renderização real | Pendente |
| Mover entre posições do desktop | Janela se move; mouse permanece alinhado | Pendente |
| Resize/maximizar no modo fixo | Controles desabilitados; tamanho original não é adaptado por `Width`/`Height` | Pendente no Lake real |
| Janela maior que a área útil | Registrar recorte/acesso aos controles; não esperar ajuste automático do framebuffer | Pendente; limitação conhecida |
| Alt+Tab 100 vezes | Outra janela recebe foco; volta sem travar | Pendente |
| Minimizar/restaurar 50 vezes | Sem buffers 0×0; repaint e input normais | Pendente |
| Botão interno “minimize” | Minimiza o host, não apenas o renderer filho | Pendente |
| Diálogos de arquivos, avisos e texto | Modalidade, teclado e foco funcionam | Pendente |
| Touch, IME e acessibilidade | Sem regressões de input | Pendente |
| 100%, 125%, 150%, 200% DPI | Coordenadas e bordas verificadas, não presumidas | Pendente |
| Vários monitores / DPI misto | Mover e restaurar; testar clipping | Pendente |
| RDP, hotplug, bloqueio de sessão | Registrar perda/recuperação de superfícies | Pendente, sem garantia |
| Fechar por X e pela UI interna | Threads encerram; sem processo órfão | Pendente |
| Encerramento observado com `--wait` | Console e `LakeWindowedFix-<PID>-exit.log` registram código final; correlacionar mensagens sem presumir causa | Pendente no Lake real |
| `WM_DISPLAYCHANGE`/mudança de ambiente | Mensagem segue para o original; registrar eventual saída nativa, sem bloqueá-la | Pendente, sem garantia |
| Reabrir pelo EXE original | Comportamento original, hashes idênticos | Pendente |
| Rede/áudio fora de produção | Descoberta/controle normal; sem alterações involuntárias | Pendente |

### Depois, opcional: `NativeResize=1` EXPERIMENTAL

Feche o aplicativo antes de alterar o INI. Além de repetir a matriz essencial, registre separadamente:

| Caso | Resultado a verificar | Estado |
| --- | --- | --- |
| `Width`/`Height` iniciais | Dimensões aplicadas, respeitando o mínimo 800×600; renderização real correta | Pendente |
| Resize até mínimo e maximizar/restaurar | UI/superfícies reconstruídas; sem tela preta, trava ou saída inesperada | Pendente |
| Arraste contínuo de borda | Sem rebuild por pixel; redesenho após soltar e input alinhado | Pendente |

O fato de `Native resize complete` aparecer não prova que HRESULTs e recursos nativos estejam válidos. Volte a `NativeResize=0` para a coleta diagnóstica inicial; não use o opt-in em áudio de produção.

Se o processo travar, capture evidência **com autorização** e guarde em `dumps/` ignorado. Não publique logs integrais, presets, dados de rede, dumps ou originais. Uma primeira chance pode ser tratada, e ausência de evento/dump não prova saída normal. Não faça firmware update nem altere adaptadores como parte de um teste gráfico.

## Critério para chamar a correção de validada

Somente após a matriz essencial passar em **cada** combinação de SO/GPU/driver alegada, com o modo de configuração identificado. Aprovar o modo fixo permitiria apenas uma alegação limitada a esse modo; **não validaria o objetivo de resize livre**. Este exige também os testes de resize/maximizar com `NativeResize=1`. Até lá, ambos continuam **experimentais**, mesmo com todos os testes Win32 sintéticos aprovados.
