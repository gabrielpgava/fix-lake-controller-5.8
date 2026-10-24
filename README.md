# Lake Controller Windowed Fix

Shim de compatibilidade **experimental**, MIT, para **Lake Controller 5.8 Build 007, x86**, no Windows 10/11. O objetivo é oferecer título, bordas, movimento, minimizar/restaurar e redimensionamento sem entrar no caminho de fullscreen exclusivo legado.

## Atualização 0.2: desaparecimento após escolher a interface de rede

A versão 0.1 podia criar a área gráfica com `WS_VISIBLE`, mas manter sua moldura oculta. Foi observado esse estado na instância de teste já aberta pelo usuário e acrescentada uma regressão que falha na DLL antiga. A 0.2 replica imediatamente o pedido nativo de exibição **sem fazer rebuild nesse momento**; o posicionamento/resize inicial é agendado após a notificação nativa `0x8032`, com o loop externo como fallback.

**Nesta sessão, a versão nova está em `dist-v0.2\`; `dist\` ainda contém a versão anterior, possivelmente em uso. Não use a pasta antiga para o reteste.** Feche a instância antiga de teste antes de continuar. Se ela não tiver janela, localize `Lake Controller.exe` em **Gerenciador de Tarefas → Detalhes**; só finalize essa instância se não houver trabalho a preservar.

```powershell
.\dist-v0.2\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

A versão nova recusa iniciar outro Lake se encontrar uma instância existente, inclusive oculta. A mensagem do launcher agora distingue **DLL inicializada** de **janela principal aberta**. Logs de diagnóstico ficam em `dist-v0.2\logs\LakeWindowedFix-<PID>.log`; se a pasta da DLL não permitir escrita, o fallback é `%TEMP%\LakeWindowedFix\`.

Para recompilar sem sobrescrever um arquivo em uso:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath .\build-tools\python\ziglang\zig.exe -OutputDirectory dist-v0.2
```

O parâmetro `-OutputDirectory` também funciona nas outras toolchains. Nos exemplos gerais abaixo, `dist\` continua sendo a saída padrão de uma compilação nova.

**O executável e as DLLs originais não são alterados no disco.** Nenhum componente proprietário é distribuído. Este projeto não é oficial e não modifica licenças, firmware, rede ou processamento de áudio.

> Ainda é necessária validação gráfica com o Lake real em cada GPU/driver e Windows. Testes sintéticos Win32 não demonstram que o travamento relatado foi eliminado. Não use inicialmente em um sistema de áudio de produção.

## O que a análise encontrou

- O EXE carrega `Contour.dll` dinamicamente. É essa DLL que importa **DirectDraw**, usa interfaces **DirectDraw/Direct3D 7**, GDI e GDI+.
- Não foi encontrado import normal de `d3d9.dll` ou `opengl32.dll` nos quatro arquivos inspecionados. Isso não exclui todo carregamento dinâmico possível.
- Existem opções internas `windowed` e `nonfullscreen`. O parser e os ramos que usam essas opções foram inspecionados, não apenas as strings.
- A janela gráfica normalmente recebe `WS_POPUP | WS_SYSMENU` (`0x80080000`). A classe interna `ContourParent` já possui tratamento de `WM_SIZE` para refazer superfícies e layout.
- O handler legado de foco tenta `SetFocus(main)` tanto ao ganhar quanto ao perder foco.

Veja [docs/ANALYSIS.md](docs/ANALYSIS.md) para hashes, RVAs, evidências, limites e decisões técnicas.

## Como funciona

1. O launcher verifica SHA-256 do EXE e de `Contour.dll`. Builds diferentes são recusados, sem opção de ignorar a verificação.
2. Inicia uma **nova** instância com `windowed nonfullscreen`, diretório de trabalho correto e pausa antes do entry point do EXE. Um `INT3` transitório de um byte é colocado/restaurado **só na memória do filho**; nenhum byte do arquivo é alterado. As DLLs de importação inicial já foram inicializadas nesse momento.
3. Carrega `LakeWindowedFix.dll` e chama sua inicialização **fora de `DllMain`/loader lock**. Só depois libera a thread principal. Não injeta em processos existentes.
4. Redireciona imports IAT de `user32`/`kernel32` apenas no EXE e em `Contour.dll`; o hook de `LoadLibraryA/W` alcança o carregamento posterior da DLL gráfica. `GetProcAddress` cobre as APIs selecionadas resolvidas dinamicamente.
5. Intercepta a criação da classe `Lake Controller` e cria um host com `WS_OVERLAPPEDWINDOW`. A área gráfica vira `WS_CHILD`, sem `WS_POPUP`, preservando o renderer original.
6. Reutiliza o procedimento nativo de `ContourParent` para resize. A visibilidade acompanha `ShowWindow`/`SetWindowPos(SHOWWINDOW)` do cliente; o resize inicial fica separado, após a notificação nativa `0x8032` ou o `GetMessage` externo como fallback. Durante arraste, adia o rebuild até `WM_EXITSIZEMOVE`; tamanhos zero/minimizados não chegam ao rebuild. A imagem pode ficar temporariamente recortada durante o arraste.
7. Impede a recuperação indevida de foco **apenas** quando `WM_KILLFOCUS` sai desse host. `WM_ACTIVATE` e `WM_ACTIVATEAPP` não são falsificados nem descartados globalmente.
8. Neutraliza alterações `ChangeDisplaySettings[A/W/Ex]` vindas dos imports cobertos. Consultas `CDS_TEST` continuam reais.
9. Encaminha o fechamento da moldura ao `WM_CLOSE` original do renderer, respeitando confirmação/recusa e cleanup nativos.

**Por que não simplesmente `ddraw.dll` ou `d3d9.dll`?** Mudar só o estilo e retornar sucesso de `SetDisplayMode`/`SetCooperativeLevel` pode deixar um flip chain exclusivo inconsistente. Aqui o próprio caminho windowed do programa escolhe a criação de superfícies. Não há implementação parcial de COM nem proxy gráfico com exports incompletos.

**Não basta copiar a DLL e abrir o EXE original.** A DLL tem nome próprio, não é carregada automaticamente. Use o launcher.

## Compilar do zero

### MSVC / Visual Studio 2022

1. Instale **Visual Studio 2022 Build Tools** (ou Visual Studio Community) com o workload **Desenvolvimento para desktop com C++**, MSVC x86/x64, Windows SDK e CMake.
2. Instale Git, obtenha este repositório e abra um **Developer PowerShell for VS 2022** com `cmake.exe` no PATH.
3. Na raiz do projeto:

```powershell
.\scripts\Build.ps1 -Toolchain MSVC
```

O script usa `-A Win32`, executa testes sintéticos e coloca os arquivos em `dist\`. O CRT do MSVC é estático (`/MT`). Python e o software Lake **não são necessários para compilar ou rodar os testes**.

Equivalente CMake:

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A Win32
cmake --build build-msvc --config Release --parallel
ctest --test-dir build-msvc -C Release --output-on-failure
cmake --install build-msvc --config Release --prefix dist
Copy-Item .\config\LakeWindowedFix.ini .\dist\LakeWindowedFix.ini
```

O último comando manual substitui a configuração; o script de build a preserva. Se sua política de scripts bloquear arquivos baixados, revise-os e desbloqueie apenas esses arquivos conforme a política local; não é necessário mudar a política global da máquina.

### MinGW-w64 x86

Com CMake, Ninja e compilador **i686** disponíveis no PATH:

```powershell
.\scripts\Build.ps1 -Toolchain MinGW -MinGWCompiler i686-w64-mingw32-g++.exe
```

Uma toolchain exclusivamente x64 não serve. Não misture DLL x64 com o processo Lake x86. Não instale DLLs deste projeto em `System32`/`SysWOW64`.

### Zig portátil (alternativa)

O script aceita Zig **0.14.1** já extraído; não faz downloads nem modifica PATH/configuração global:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:\tools\zig\zig.exe
```

Também é possível obter a distribuição Python desse compilador em uma pasta local ignorada, explicitamente:

```powershell
python -m pip install --target .\build-tools\python ziglang==0.14.1
.\scripts\Build.ps1 -Toolchain Zig -ZigPath .\build-tools\python\ziglang\zig.exe
```

Esse comando baixa uma toolchain de terceiros; é opcional. Consulte [docs/VALIDATION.md](docs/VALIDATION.md) para o que foi efetivamente compilado/testado.

## Usar sem instalar (recomendado para o primeiro teste)

1. Feche outras instâncias do Lake. Faça backup dos seus presets/configurações pelos mecanismos normais do software.
2. Em um ambiente de teste, sem processadores de áudio em produção, execute:

```powershell
.\dist\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

O launcher e a DLL permanecem em `dist\`; não é necessário escrever na pasta do Lake. Eles devem ficar juntos. O próprio Lake ainda pode precisar das permissões e dependências originais da instalação.

3. Confira título/bordas, arraste, redimensione, Alt+Tab e minimizar/restaurar seguindo [a matriz de teste](docs/VALIDATION.md).
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

Para instalar a compilação isolada nova, acrescente `-SourceDirectory dist-v0.2`. Não instale enquanto houver uma instância do Lake aberta.

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
```

São dimensões iniciais da área cliente. Mínimo: **800×600**. Valores inválidos voltam ao padrão; a janela tenta caber na área útil do monitor sem violar esse mínimo. Desktops menores podem não comportar todas as bordas. Resize reconstrói recursos nativos e não é prometido como operação sem latência.

Para comparar somente as flags internas, **sem bordas/host/hooks**, há um diagnóstico:

```powershell
.\dist\LakeWindowedLauncher.exe --app-dir "C:\Program Files (x86)\Lake\Lake Controller v5.8" --native-only
# Ou, sem compilar:
.\scripts\Start-NativeWindowed.ps1 -AppDirectory "C:\Program Files (x86)\Lake\Lake Controller v5.8"
```

Esses comandos executam o Lake real, incluindo seus comportamentos originais de rede/configuração. Não são testes automatizados seguros para um sistema ao vivo.

## Limites e segurança operacional

- Apenas os hashes documentados são aceitos. O nome “5.8” sozinho não garante equivalência de binários.
- A DLL não implementa recuperação universal de `DDERR_SURFACELOST`, mudanças de monitor, RDP, hotplug ou de profundidade de cor. Elas precisam de teste; os avisos antigos de 16-bit podem continuar aparecendo.
- Sem patch global de User32/DirectDraw, serviço, driver, registro, AppInit_DLLs ou injeção em terceiros.
- Hooks IAT de outros programas não são sobrescritos deliberadamente. Não combine inicialmente com DxWnd, overlays ou outros wrappers.
- Chamadas por outros módulos, ordinais, delay imports e `LoadLibraryEx` não estão cobertas. O caminho observado usa imports normais e `LoadLibraryA`.
- A inicialização anterior ao entry point é transitória; não mantém o aplicativo sob um debugger. Veja os detalhes em `docs/ANALYSIS.md`.
- Logs vão para `OutputDebugString` e `logs\LakeWindowedFix-<PID>.log` ao lado da DLL (fallback em `%TEMP%\LakeWindowedFix`). Contêm etapas de inicialização, PID/thread e operações gráficas, não nomes de adaptadores, IPs, presets ou credenciais. A DLL fica carregada até o processo terminar; não tente descarregá-la a quente.
- O software original pode alterar arquivos ou adaptadores de rede por conta própria. Este projeto não neutraliza essas funções.

## Auditoria e contribuição

```text
src/                    DLL, launcher, políticas Win32 e exports x86
scripts/                build, instalação/remoção e inspeção offline
tests/fixture.cpp       aplicativo sintético, sem código proprietário
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
