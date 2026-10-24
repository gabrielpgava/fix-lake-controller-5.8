# Diagnóstico

## Comece pela 0.3 com tamanho fixo e `--wait`

Use **`dist-v0.3\`**, não `dist\` (0.1) nem `dist-v0.2\`. Mantenha launcher/DLL/INI da mesma versão e confirme no INI ao lado da DLL:

```ini
[compatibility]
NativeResize=0
```

Esse é o **padrão diagnóstico**, não uma correção completa do objetivo de resize livre: conserva o framebuffer original, não envia `WM_SIZE` inicial nem encaminha o resize do host ao rebuild nativo. Alça de resize e maximizar ficam desabilitados; `Width`/`Height` são ignorados. A janela pode ultrapassar a área útil ou ficar recortada. Isso não comprova que a GPU/renderer estejam saudáveis.

Com outras instâncias fechadas, **o usuário pode executar o Lake real**, fora de produção:

```powershell
.\dist-v0.3\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

Mantenha o console aberto; `--wait` aguarda o aplicativo encerrar, consulta `GetExitCodeProcess` e imprime/grava o código decimal e hexadecimal em `dist-v0.3\logs\LakeWindowedFix-<PID>-exit.log`. Na espera normal de produção não há timeout que mate o Lake. Isso é separado dos timeouts de inicialização e dos testes sintéticos.

Se continuar fechando, faça uma comparação **separada**, somente com as flags internas e sem DLL/host/hooks:

```powershell
.\dist-v0.3\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only --wait
```

Esse modo gera o relatório de saída pelo launcher, mas não o log interno da DLL. Também executa os comportamentos reais de rede/configuração do Lake; não é um teste automatizado seguro para áudio ao vivo. Diferenças entre as duas execuções são pistas, não confirmação automática de causa.

## Selecionou a interface de rede e a tela principal não apareceu

Foi identificada uma falha da **0.1**: o cliente podia receber `WS_VISIBLE`, mas a moldura continuava oculta. A correção de visibilidade entrou na **0.2** e foi mantida na **0.3**. Ela não equivale a corrigir o encerramento posterior observado com a 0.2.

Antes de tentar novamente, feche a instância antiga. Se não houver janela, confira `Lake Controller.exe` em **Gerenciador de Tarefas → Detalhes**. Finalizar um processo pode perder trabalho não salvo; faça isso somente na instância de teste sem dados a preservar. O launcher novo não encerra processos existentes e recusa criar outro enquanto encontrar um Lake aberto (exit 4).

Não execute o launcher repetidamente para tentar “destravar”: o software original também usa um mutex e uma segunda instância pode apenas sair. A mensagem “processo criado/DLL inicializada” não significa que o startup gráfico terminou.

Os logs internos ficam em `logs\LakeWindowedFix-<PID>.log` ao lado da DLL, com fallback em `%TEMP%\LakeWindowedFix\` quando disponível. O relatório `-exit.log` fica em `logs\` ao lado do launcher; tenta o fallback temporário também após erro de abertura/escrita local, mesmo se `logs` já existir. Procure, na 0.3:

- `Native ShowWindow ... host-visible=1`: pedido de visibilidade replicado.
- `Ready queued after native 0x8032 notification` ou `Ready queued by EXE GetMessage fallback`: setup agendado.
- `NativeResize=0: original framebuffer size retained; no native graphics rebuild`: política fixa selecionada.
- `Initial placement complete WITHOUT WM_SIZE or framebuffer size change`: posicionamento sem resize inicial do framebuffer.
- `Host setup complete (not a renderer health check)`: apenas setup da janela. A propriedade `LakeWindowedFix.HostSetupCompleted` tem esse mesmo limite.
- `Native resize begin`/`Native resize complete`: esperados somente com `NativeResize=1` nos caminhos de resize do host. Se aparecerem num reteste que deveria ser fixo, confira o INI e a versão da DLL. “Complete” significa retorno da chamada, não sucesso gráfico comprovado.

Para um diagnóstico passivo, sem injeção adicional nem alterações de rede:

```powershell
python .\scripts\Inspect-WindowState.py --output .\private\window-state.json
```

O script consulta nomes de processos e estado de janelas no desktop atual. Não abre memória de processo nem lê os nomes/IPs dos adaptadores.

## O processo fechou após abrir ou escolher a rede

No caso histórico **0.2, PID 14208**, o log `dist-v0.2/logs/LakeWindowedFix-14208.log` chegou a `Native ShowWindow cmd=1/5`, `Initial placement begin`, `Native resize begin/complete`, `Initial placement complete` e `Native windowed host ready`. O processo depois fechou. Não foi encontrado evento Aplicativo 1000/1001/1002 correspondente nem dump. **Não há causa confirmada e não sabemos se foi crash ou saída normal.**

A inspeção estática encontrou riscos no rebuild e um ramo nativo de `WM_DISPLAYCHANGE` que pode chamar `PostQuitMessage(0)` mesmo em windowed; não estabelece relação causal com esse encerramento. Veja [ANALYSIS.md, R2](ANALYSIS.md#r2--encerramento-após-o-setup-da-02-e-diagnóstico-na-03).

Reteste a 0.3 com `NativeResize=0` e `--wait` conforme acima. Correlacione PID/horário e, no log interno, procure:

- `Host`/`Client WM_CLOSE`, `WM_DESTROY` e `WM_NCDESTROY`: sequência de fechamento/destruição, sem concluir por ela quem iniciou tudo.
- `Client WM_DISPLAYCHANGE ... (forwarded unchanged)`: mensagem entregue ao original; a 0.3 **não a descarta**.
- `Application PostQuitMessage`, `Application ExitProcess`, `EXE`/`Module GetMessageA/W returned WM_QUIT` e `PeekMessageA/W removed WM_QUIT`: caminhos cobertos de saída. Código, endereço e RVA são registrados quando aplicável às imagens cacheadas do EXE/`Contour.dll`; outros caminhos podem não passar pelos hooks.
- `Host WM_DESTROY: preserving pending native UI quit code=...`: quit nativo observado da thread da interface preservado, sem postar zero por cima. Quits de workers não definem esse estado.
- `Host WM_DESTROY: no native UI quit pending; shim calls PostQuitMessage(0)`: pedido próprio do host, distinto de um pedido registrado do aplicativo.
- `First-chance exception (may be handled by application)`: observação, **não confirmação de crash**. O aplicativo pode tratar a exceção; o observador sempre retorna `EXCEPTION_CONTINUE_SEARCH`. Observa AV, instrução ilegal e divisão inteira por zero, sem engolir/tratar a falha.

O shim não cancela `ExitProcess`/`PostQuitMessage`. Os registros de lifecycle/saída/exceção são de melhor esforço: não esperam pelo lock do logger nem chamam `OutputDebugString`, e podem omitir linhas sob contenção. A escrita em arquivo ainda é síncrona. O código final é um dado adicional: zero não prova funcionamento correto, e não zero sozinho não prova crash (a fixture, por exemplo, sai intencionalmente com 23). Ausência de evento/dump ou de linha de exceção também não prova saída normal.

Guarde os logs localmente e informe versão, `NativeResize`, código decimal/hexadecimal, última etapa, GPU/driver, Windows, monitores/DPI e operação realizada. Compartilhe somente trechos revisados e sem dados sensíveis; **não envie ao Git logs/dumps, relatórios privados integrais ou arquivos originais**. Capture dump apenas com autorização. Os 133 PASS sintéticos da 0.3 não substituem esse reteste no Lake real.

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
- Leia o arquivo de log ou use DebugView para `OutputDebugString`: procure `[LakeWindowedFix 0.3]`, `Initialized`, `client hosted` e `Host setup complete`. Setup concluído não significa renderer saudável.
- O patch não corrige uma janela criada por outro módulo/classe desconhecido. Informe hash e contexto, não envie o EXE.

## Sem resize/maximizar ou `Width`/`Height` sem efeito

É esperado com **`NativeResize=0`**, padrão da 0.3. O shim mantém o framebuffer no tamanho original, mesmo que a janela não caiba no monitor. Não habilite resize apenas para esconder recorte durante a coleta diagnóstica inicial.

Para testar o objetivo de resize livre, é necessário optar explicitamente por **`[compatibility] NativeResize=1`, EXPERIMENTAL**, fechar/reabrir o aplicativo e usar uma execução separada fora de produção. Só nesse modo `Width`/`Height` definem o tamanho cliente inicial (mínimo 800×600) e resize/maximizar ficam habilitados. Build/instalação preservam INIs existentes; confira o valor usado em vez de presumir que foi atualizado.

## Resize experimental lento, tela preta, erro de 16-bit, RDP/hotplug

O renderer e a recuperação de superfícies continuam sendo nativos. Com `NativeResize=1`, resize usa rebuild após o arraste, não a cada pixel, mas há riscos estáticos de teardown/recriação e erros não propagados. Se houver tela preta ou erro, feche normalmente, retorne ao modo fixo para diagnóstico ou ao EXE original e documente GPU/driver, sistema, DPI, tamanho e operação realizada. Não há garantia de suporte a mudança de profundidade de cor, sessão remota ou reconexão de monitor, mesmo no modo fixo.

Teste primeiro sem processadores de áudio de produção. Se o Lake já estiver travado, o launcher não é ferramenta de recuperação nem interfere nesse processo.

## Permissões

O uso portátil com `--app-dir` não grava o patch em `Program Files`. A instalação opcional nessa pasta normalmente requer elevação. Os comportamentos originais de escrita de configurações e alteração de rede continuam sujeitos às permissões do próprio Lake.
