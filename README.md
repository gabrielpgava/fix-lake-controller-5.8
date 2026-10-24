# Lake Controller Windowed Fix

Shim de compatibilidade **experimental**, MIT, para **Lake Controller 5.8 Build 007, x86**, no Windows 10/11. O objetivo é oferecer título, bordas, movimento, minimizar/restaurar e redimensionamento sem entrar no caminho de fullscreen exclusivo legado. **A 0.3 usa tamanho fixo por padrão: ainda não é a correção completa do objetivo de resize livre.**

## Atualização 0.3: tamanho fixo diagnóstico e registro de saída

- **`[compatibility] NativeResize=0` é o padrão.** Preserva o tamanho original do framebuffer, não envia o `WM_SIZE` inicial nem encaminha o resize do host ao rebuild nativo. Desabilita a alça de redimensionamento e maximizar; `Width`/`Height` são ignorados. A janela pode ultrapassar a área útil do monitor ou ficar recortada.
- A correção de visibilidade da 0.2 foi mantida. `NativeResize=1` permite resize/rebuild e as dimensões configuradas, mas é **EXPERIMENTAL** e deve ser testado separadamente.
- O novo **`--wait`** mantém o launcher aguardando o encerramento do aplicativo, consulta `GetExitCodeProcess` e imprime/grava o código em decimal e hexadecimal. Na espera normal, não há timeout que mate o Lake; timeouts de falha de inicialização continuam separados.
- A DLL registra mensagens de fechamento, `WM_DISPLAYCHANGE`, chamadas cobertas de `PostQuitMessage`/`ExitProcess` e algumas exceções de primeira chance. **Não bloqueia a saída, não descarta `WM_DISPLAYCHANGE` e não trata essas exceções.** `HostSetupCompleted` significa apenas setup da janela, não saúde do renderer/GPU.
- O fechamento preserva um quit nativo pendente da thread da interface, sem usar quits de workers. Registros de saída são de melhor esforço, sem esperar pelo lock do logger; o relatório final tenta `%TEMP%` também após falha de abertura/escrita local.

No reteste real da **0.2**, iniciado pelo usuário (PID histórico `14208`), o log chegou a `Native resize complete`, `Initial placement complete` e `Native windowed host ready`, mas o processo depois fechou. Não foi encontrado evento Aplicativo 1000/1001/1002 correspondente nem dump. **Não sabemos se foi crash ou saída normal; o rebuild é um risco identificado estaticamente, não uma causa confirmada.** Veja [R2 em ANALYSIS.md](docs/ANALYSIS.md#r2--encerramento-após-o-setup-da-02-e-diagnóstico-na-03).

### Retestar a 0.3

**Os binários desta sessão estão em `dist-v0.3\`, sem sobrescrever `dist-v0.2\`.** Essas pastas são saídas locais ignoradas pelo Git. Use launcher, DLL e INI da mesma versão e confirme `NativeResize=0` no INI ao lado da DLL. Feche outras instâncias de teste; o launcher recusa outra instância, inclusive oculta, e não encerra processos já existentes.

Os comandos abaixo são para **o usuário executar o Lake real**, em ambiente de teste e sem áudio de produção:

```powershell
.\dist-v0.3\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

Se continuar fechando, compare, em uma execução separada, somente as flags nativas, **sem DLL/host/hooks**:

```powershell
.\dist-v0.3\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only --wait
```

Mantenha o console aberto até o Lake terminar. O primeiro comando gera `dist-v0.3\logs\LakeWindowedFix-<PID>.log`; ambos podem gerar `logs\LakeWindowedFix-<PID>-exit.log` ao lado do launcher. O fallback, quando disponível, é `%TEMP%\LakeWindowedFix\`. O código final, mesmo zero, **não comprova correção gráfica**; uma exceção de primeira chance pode ser tratada pelo próprio aplicativo e não comprova crash. A comparação `--native-only` não isola sozinha a causa.

Há **133 verificações PASS, zero falhas**, registradas nas três suítes sintéticas da 0.3 após as correções de diagnóstico. Elas não executam o Lake nem criam superfícies DirectDraw/Direct3D7. O reteste real da 0.3 permanece pendente; detalhes em [docs/VALIDATION.md](docs/VALIDATION.md).

### Histórico 0.1 → 0.2

A 0.1 podia deixar a moldura oculta após a seleção de rede, mesmo com `WS_VISIBLE` no cliente. A 0.2 passou a replicar imediatamente o pedido nativo de exibição **sem rebuild nesse momento**, agendando o posicionamento/resize inicial após a notificação `0x8032`, com o loop externo como fallback. Esse resize inicial agora só existe com `NativeResize=1`. O relato de encerramento da 0.2 é distinto da falha de visibilidade da 0.1; os registros históricos estão preservados na documentação.

**O executável e as DLLs originais não são alterados no disco.** Nenhum componente proprietário é distribuído. Este projeto não é oficial e não modifica licenças, firmware, rede ou processamento de áudio.

> Ainda é necessária validação gráfica com o Lake real em cada GPU/driver e Windows. Nem o modo fixo diagnóstico nem os testes sintéticos Win32 demonstram que o travamento relatado foi eliminado. Não use inicialmente em um sistema de áudio de produção.

## O que a análise encontrou

- O EXE carrega `Contour.dll` dinamicamente. É essa DLL que importa **DirectDraw**, usa interfaces **DirectDraw/Direct3D 7**, GDI e GDI+.
- Não foi encontrado import normal de `d3d9.dll` ou `opengl32.dll` nos quatro arquivos inspecionados. Isso não exclui todo carregamento dinâmico possível.
- Existem opções internas `windowed` e `nonfullscreen`. O parser e os ramos que usam essas opções foram inspecionados, não apenas as strings.
- A janela gráfica normalmente recebe `WS_POPUP | WS_SYSMENU` (`0x80080000`). O `WM_SIZE` de `ContourParent` refaz superfícies e layout, com teardown global e caminhos de erro que exigem validação; não é uma operação comprovadamente segura.
- Há um caminho nativo de `WM_DISPLAYCHANGE` que pode chamar `PostQuitMessage(0)` mesmo em windowed. Não há evidência de que tenha ocorrido no encerramento observado da 0.2.
- O handler legado de foco tenta `SetFocus(main)` tanto ao ganhar quanto ao perder foco.

Veja [docs/ANALYSIS.md](docs/ANALYSIS.md) para hashes, RVAs, evidências, limites e decisões técnicas.

## Como funciona

1. O launcher verifica SHA-256 do EXE e de `Contour.dll`. Builds diferentes são recusados, sem opção de ignorar a verificação.
2. Inicia uma **nova** instância com `windowed nonfullscreen`, diretório de trabalho correto e pausa antes do entry point do EXE. Um `INT3` transitório de um byte é colocado/restaurado **só na memória do filho**; nenhum byte do arquivo é alterado. As DLLs de importação inicial já foram inicializadas nesse momento.
3. Carrega `LakeWindowedFix.dll` e chama sua inicialização **fora de `DllMain`/loader lock**. Só depois libera a thread principal. Não injeta em processos existentes.
4. Redireciona imports IAT de `user32`/`kernel32` apenas no EXE e em `Contour.dll`; o hook de `LoadLibraryA/W` alcança o carregamento posterior da DLL gráfica. `GetProcAddress` cobre as APIs selecionadas resolvidas dinamicamente.
5. Intercepta a criação da classe `Lake Controller` e cria um host com título/bordas, derivado de `WS_OVERLAPPEDWINDOW`, mas sem `WS_THICKFRAME`/`WS_MAXIMIZEBOX` no padrão fixo. A área gráfica vira `WS_CHILD`, sem `WS_POPUP`, preservando o renderer original.
6. A visibilidade acompanha `ShowWindow`/`SetWindowPos(SHOWWINDOW)` do cliente. O setup inicial fica separado, após a notificação nativa `0x8032` ou o `GetMessage` externo como fallback. Com `NativeResize=0`, mantém o framebuffer original e não chama o rebuild pelo resize do host. Só com `NativeResize=1` reutiliza o resize de `ContourParent`, adiando o rebuild durante arraste até `WM_EXITSIZEMOVE` e filtrando tamanhos zero/minimizados. A imagem pode ficar recortada durante o arraste experimental.
7. Impede a recuperação indevida de foco **apenas** quando `WM_KILLFOCUS` sai desse host. `WM_ACTIVATE` e `WM_ACTIVATEAPP` não são falsificados nem descartados globalmente.
8. Neutraliza alterações `ChangeDisplaySettings[A/W/Ex]` vindas dos imports cobertos. Consultas `CDS_TEST` continuam reais.
9. Encaminha o fechamento da moldura ao `WM_CLOSE` original do renderer, respeitando confirmação/recusa e cleanup nativos.
10. Observa os caminhos de saída cobertos e exceções selecionadas sem impedir o comportamento original. Endereço/RVA só é atribuído ao EXE ou a `Contour.dll` quando estiver nas imagens cacheadas; não é uma captura universal de todas as saídas.

**Por que não simplesmente `ddraw.dll` ou `d3d9.dll`?** Mudar só o estilo e retornar sucesso de `SetDisplayMode`/`SetCooperativeLevel` pode deixar um flip chain exclusivo inconsistente. Aqui o próprio caminho windowed do programa escolhe a criação de superfícies. Não há implementação parcial de COM nem proxy gráfico com exports incompletos.

**Não basta copiar a DLL e abrir o EXE original.** A DLL tem nome próprio, não é carregada automaticamente. Use o launcher.

## Compilar do zero

### MSVC / Visual Studio 2022

1. Instale **Visual Studio 2022 Build Tools** (ou Visual Studio Community) com o workload **Desenvolvimento para desktop com C++**, MSVC x86/x64, Windows SDK e CMake.
2. Instale Git, obtenha este repositório e abra um **Developer PowerShell for VS 2022** com `cmake.exe` no PATH.
3. Na raiz do projeto:

```powershell
.\scripts\Build.ps1 -Toolchain MSVC -OutputDirectory dist-v0.3
```

O script usa `-A Win32` e executa as três suítes sintéticas. `-OutputDirectory` funciona em todas as toolchains; os exemplos isolam a 0.3 em `dist-v0.3\` (sem o parâmetro, a saída padrão continua `dist\`). O CRT do MSVC é estático (`/MT`). Python e o software Lake **não são necessários para compilar ou rodar os testes**.

Equivalente CMake:

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A Win32
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
cmake --install build-msvc --config Release --prefix dist-v0.3
Copy-Item .\config\LakeWindowedFix.ini .\dist-v0.3\LakeWindowedFix.ini
```

O último comando manual substitui a configuração; o script de build a preserva. Em builds incrementais, confira explicitamente `NativeResize=0` antes do reteste: um INI anterior com valor 1 não é migrado automaticamente. Se sua política de scripts bloquear arquivos baixados, revise-os e desbloqueie apenas esses arquivos conforme a política local; não é necessário mudar a política global da máquina.

### MinGW-w64 x86

Com CMake, Ninja e compilador **i686** disponíveis no PATH:

```powershell
.\scripts\Build.ps1 -Toolchain MinGW -MinGWCompiler i686-w64-mingw32-g++.exe -OutputDirectory dist-v0.3
```

Uma toolchain exclusivamente x64 não serve. Não misture DLL x64 com o processo Lake x86. Não instale DLLs deste projeto em `System32`/`SysWOW64`.

### Zig portátil (alternativa)

O script aceita Zig **0.14.1** já extraído; não faz downloads nem modifica PATH/configuração global:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:\tools\zig\zig.exe -OutputDirectory dist-v0.3
```

Também é possível obter a distribuição Python desse compilador em uma pasta local ignorada, explicitamente:

```powershell
python -m pip install --target .\build-tools\python ziglang==0.14.1
.\scripts\Build.ps1 -Toolchain Zig -ZigPath .\build-tools\python\ziglang\zig.exe -OutputDirectory dist-v0.3
```

Esse comando baixa uma toolchain de terceiros; é opcional. Consulte [docs/VALIDATION.md](docs/VALIDATION.md) para o que foi efetivamente compilado/testado.

## Usar sem instalar (recomendado para o primeiro teste)

1. Feche outras instâncias do Lake. Faça backup dos seus presets/configurações pelos mecanismos normais do software.
2. Em um ambiente de teste, sem processadores de áudio em produção, execute:

```powershell
.\dist-v0.3\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --wait
```

O launcher, a DLL e o INI permanecem em `dist-v0.3\`; não é necessário escrever na pasta do Lake. Eles devem ficar juntos. O próprio Lake ainda pode precisar das permissões e dependências originais da instalação.

3. No padrão fixo, confira título/bordas, movimento, Alt+Tab, minimizar/restaurar e eventual recorte seguindo [a matriz de teste](docs/VALIDATION.md). Resize/maximizar só fazem parte de uma execução experimental separada com `NativeResize=1`.
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
.\scripts\Install.ps1 -AppDirectory "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

Para instalar a compilação isolada nova, acrescente `-SourceDirectory dist-v0.3`. Não instale enquanto houver uma instância do Lake aberta.

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
Height=768

[compatibility]
NativeResize=0
```

**Padrão diagnóstico (`NativeResize=0`):** preserva as dimensões originais do framebuffer; `Width`/`Height` não são aplicados. Não há resize interativo nem maximizar. A janela não é reduzida para caber no monitor e pode ultrapassar a área útil/ter recorte. Esse modo reduz a interferência do shim no renderer, mas não comprova estabilidade nem cumpre o objetivo de resize livre.

**Opt-in experimental (`NativeResize=1`):** habilita o rebuild legado, resize/maximizar e `Width`/`Height` como dimensões iniciais da área cliente. Mínimo: **800×600**. Valores inválidos voltam ao padrão; a janela tenta caber na área útil sem violar o mínimo. Desktops menores podem não comportar todas as bordas. Não há promessa de ausência de travamento, tela preta ou latência. Feche o Lake antes de mudar a configuração e teste fora de produção.

Para comparar somente as flags internas, **sem bordas/host/hooks**, há um diagnóstico:

```powershell
.\dist-v0.3\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only --wait
# Ou, sem compilar:
.\scripts\Start-NativeWindowed.ps1 -AppDirectory "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

Esses comandos executam o Lake real, incluindo seus comportamentos originais de rede/configuração. Não são testes automatizados seguros para um sistema ao vivo. O script alternativo não fornece o novo relatório `--wait` do launcher.

## Limites e segurança operacional

- Apenas os hashes documentados são aceitos. O nome “5.8” sozinho não garante equivalência de binários.
- A DLL não implementa recuperação universal de `DDERR_SURFACELOST`, mudanças de monitor, RDP, hotplug ou de profundidade de cor. Elas precisam de teste; os avisos antigos de 16-bit podem continuar aparecendo.
- Sem patch global de User32/DirectDraw, serviço, driver, registro, AppInit_DLLs ou injeção em terceiros.
- Hooks IAT de outros programas não são sobrescritos deliberadamente. Não combine inicialmente com DxWnd, overlays ou outros wrappers.
- Chamadas por outros módulos, ordinais, delay imports e `LoadLibraryEx` não estão cobertas. O caminho observado usa imports normais e `LoadLibraryA`.
- A inicialização anterior ao entry point é transitória; não mantém o aplicativo sob um debugger. Veja os detalhes em `docs/ANALYSIS.md`.
- Logs vão para `logs\LakeWindowedFix-<PID>.log` ao lado da DLL (fallback em `%TEMP%\LakeWindowedFix`). Registros comuns também usam `OutputDebugString`; observadores de saída/exceção não o usam e podem omitir linhas se o lock estiver ocupado. A escrita em arquivo continua síncrona. Com `--wait`, o launcher acrescenta `LakeWindowedFix-<PID>-exit.log`. Incluem PID/thread, etapas, mensagens e endereços/RVAs diagnósticos; não há coleta intencional de nomes de adaptadores, IPs, presets ou credenciais. Revise qualquer trecho antes de compartilhá-lo; não versione logs/dumps nem originais. A DLL fica carregada até o processo terminar; não tente descarregá-la a quente.
- O software original pode alterar arquivos ou adaptadores de rede por conta própria. Este projeto não neutraliza essas funções.

## Auditoria e contribuição

```text
src/                    DLL, launcher, políticas Win32 e exports x86
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
