# Diagnóstico

## Selecionou a interface de rede e a tela principal não apareceu

Foi identificada uma falha da versão 0.1: o cliente podia receber `WS_VISIBLE`, mas a moldura continuava oculta. Use a compilação 0.2 em `dist-v0.2\` nesta sessão, não a DLL antiga em `dist\`.

Antes de tentar novamente, feche a instância antiga. Se não houver janela, confira `Lake Controller.exe` em **Gerenciador de Tarefas → Detalhes**. Finalizar um processo pode perder trabalho não salvo; faça isso somente na instância de teste sem dados a preservar. O launcher novo não encerra processos existentes e recusa criar outro enquanto encontrar um Lake aberto (exit 4).

Não execute o launcher repetidamente para tentar “destravar”: o software original também usa um mutex e uma segunda instância pode apenas sair. A mensagem “processo criado/DLL inicializada” não significa que o startup gráfico terminou.

Os logs da 0.2 ficam em `logs\LakeWindowedFix-<PID>.log` ao lado da DLL, ou `%TEMP%\LakeWindowedFix\` se não houver permissão de escrita. Procure:

- `Native ShowWindow ... host-visible=1`: pedido de visibilidade replicado.
- `Ready queued after native 0x8032 notification`: marco nativo recebido.
- `Initial placement complete` / `Native windowed host ready`: posicionamento concluído.
- `Native resize begin` sem `Native resize complete`: a chamada ao rebuild nativo não retornou; é uma falha diferente de apenas manter a moldura oculta.

Para um diagnóstico passivo, sem injeção adicional nem alterações de rede:

```powershell
python .\scripts\Inspect-WindowState.py --output .\private\window-state.json
```

O script consulta nomes de processos e estado de janelas no desktop atual. Não abre memória de processo nem lê os nomes/IPs dos adaptadores.

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
- Não use `--native-only` para a correção completa.
- Não copie a DLL como `ddraw.dll`, `user32.dll` ou `d3d9.dll`.
- Leia o arquivo de log ou use DebugView para `OutputDebugString`: procure `[LakeWindowedFix 0.2]`, `Initialized`, `client hosted` e `host ready`.
- O patch não corrige uma janela criada por outro módulo/classe desconhecido. Informe hash e contexto, não envie o EXE.

## Resize lento, tela preta, erro de 16-bit, RDP/hotplug

O renderer e a recuperação de superfícies continuam sendo nativos. Resize usa rebuild após o arraste, não a cada pixel. Se houver tela preta ou erro, feche normalmente, volte ao EXE original e documente GPU/driver, sistema, DPI, tamanho e operação que causou a falha. Não há garantia de suporte a mudança de profundidade de cor, sessão remota ou reconexão de monitor.

Teste primeiro sem processadores de áudio de produção. Se o Lake já estiver travado, o launcher não é ferramenta de recuperação nem interfere nesse processo.

## Permissões

O uso portátil com `--app-dir` não grava o patch em `Program Files`. A instalação opcional nessa pasta normalmente requer elevação. Os comportamentos originais de escrita de configurações e alteração de rede continuam sujeitos às permissões do próprio Lake.
