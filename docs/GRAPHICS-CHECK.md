# Diagnóstico gráfico independente

`LakeFixGraphicsCheck.exe` é uma aplicação **x86 própria**, não um teste do Lake.
Não precisa do Lake, Contour, DLL de injeção, assets proprietários ou áudio.
Um resultado positivo **não aprova o Lake, seu renderer nem áudio**.

## Segurança e execução

Só execute numa sessão Windows 10/11 interativa, **ativa e desbloqueada**, com
autorização do operador. Não execute em serviço, desktop bloqueado, tarefa sem
desktop interativo ou agente cujo acesso ao desktop não esteja garantido.
Compilar e usar `--help` / `--self-test` não carrega DirectDraw nem cria janela.

O modo gráfico exige `--desktop-confirmed`, além de verificar sessão WTS ativa,
estado desbloqueado e desktop de entrada `Default` igual ao desktop da thread.
Também exige a window station interativa visível `WinSta0`.
Falha/indisponibilidade dessa verificação bloqueia o teste, sem tentar desbloquear
ou trocar desktop. A verificação é repetida entre operações/etapas; não elimina a
possibilidade de o operador bloquear a sessão durante uma chamada já em curso.

```powershell
# Seguro, CPU somente (sem confirmação de desktop):
.\build-graphics-next\LakeFixGraphicsCheck.exe --self-test

# Apenas com desktop ativo/desbloqueado verificado pelo operador:
.\tests\Run-GraphicsCheck.ps1 -BinaryDirectory .\build-graphics-next `
  -ConfirmActiveUnlockedDesktop -ConfirmNoOperationalAudio

# Janela interativa; após a sequência, clique no controle, redimensione, ESC/feche:
.\tests\Run-GraphicsCheck.ps1 -BinaryDirectory .\build-graphics-next `
  -Mode Manual -ConfirmActiveUnlockedDesktop -ConfirmNoOperationalAudio
```

Sem ambas as confirmações (`-ConfirmActiveUnlockedDesktop` e
`-ConfirmNoOperationalAudio`), o script **não inicia a aplicação** e grava
`NOT_EXECUTED` em um relatório novo. Essa situação não é PASS. O executável também
recusa execução gráfica sem sua confirmação explícita.

`--automated` usa janela `WS_EX_NOACTIVATE`, exibição/restauração sem ativação,
redimensionamento somente da própria janela e mensagens `WM_LBUTTONDOWN/UP`
síncronas enviadas **somente ao HWND próprio**. Não usa `SendInput`, hooks,
`SetForegroundWindow`, captura de mouse, mudança de foco, leitura/movimento do
ponteiro ou eventos de teclado globais. Não restaura foco alterando janelas de
terceiros. O foreground próprio no modo automatizado é falha de isolamento.
Interações reais de mouse são ignoradas nesse modo. `--manual` permite interação
normal com a própria janela.

## O que é exercitado

* `DirectDrawCreateEx` real, carregado pelo caminho do diretório de sistema;
  `DDSCL_NORMAL`, primary real, clipper associado ao HWND próprio e canvas
  offscreen `DDSCAPS_SYSTEMMEMORY`, xRGB32, padrão **1088 × 612**.
* `WrapDirectDraw7` e `ConfigureScalePresenter`, sem alterar vtable nativa.
  QI de D3D7 antes/depois compara HRESULT e identidade IUnknown nativa; a
  indisponibilidade nativa de D3D7 é registrada, não simulada. DD7/primary mantêm
  identidade de proxy; offscreen continua nativo. Não há renderização D3D.
* Grid, quatro marcadores sólidos nos cantos, RGB/CMY/branco, texto de glifos
  geométricos próprios, marcador de frame e controle ON/OFF.
* Estado inicial, clique por mensagem própria, resize largo/alto, novo clique
  com a geometria apresentada, rejeição de clique em barra/fundo, retorno ao
  tamanho original, minimizar/restaurar e comparação de marcadores/controle.
* Coordenadas negativas **somente se houver monitor disponível nessa posição**;
  move apenas a janela própria e depois retorna. Sem monitor adequado, SKIP.
* `IsLost`/`Restore` quando necessário após restauração, recriação do conteúdo
  próprio e liberação dos proxies ao final.

A viewport proposta não publica imediatamente o mapeamento de entrada. A
aplicação configura o tamanho esperado e usa `ScalePresentationCommit`: somente
o callback pós-`Blt` bem-sucedido, com dimensões e retângulos exatos da consulta,
confirma a geometria. O destino originalmente solicitado tem a extensão total
do canvas para respeitar a admissão do presenter; este o substitui pela viewport
do cliente real. O commit ainda não comprova pixels, que são lidos em seguida.

## Pixels e limites

`HRESULT` de `Blt` **não basta**. Cada etapa compara amostras sólidas esperadas
com `GetPixel` do DC da primary. Se não disponível, tenta `Lock` read-only da
primary, decodificando máscaras RGB nativas 16/24/32 bpp. Se esses caminhos não
endereçarem as amostras (por exemplo monitor negativo), tenta DC **da janela
própria** e registra explicitamente `LIMITED`, sem alegar readback da primary.
Um valor válido mas incorreto da primary não é trocado por outro método para
mascarar falha. Tolerância fixa: 8 por canal, inclusive quantização RGB565.
Há até cinco leituras para latência de composição, sem redesenhar entre elas.

`WindowFromPoint` confirma que cada ponto pertence à janela própria; o teste
fica inconclusivo se outra janela cobrir amostras. Nenhuma janela de terceiro é
movida para resolver isso. Não são coletadas screenshots ou pixels de regiões
arbitrárias de outras aplicações.

O relatório registra BPP/capabilities **reais**, dimensões da primary, monitores,
sessão remota, métricas de desktop, awareness DPI observada, método de leitura,
RGB esperado/observado, estado/cliques e HRESULTs por operação/etapa.
Não muda modo de vídeo, BPP global, capabilities, manifesto ou awareness DPI,
nem do diagnóstico nem de processo legado.

DPI virtualizado/misto, compositor, RDP, gerenciamento de cor e coordenadas
da primary restritas ao adaptador podem afetar as leituras. Um PASS comprova
apenas os pontos e caminhos registrados nessa aplicação/ambiente; não prova
scanout físico, cursor de hardware, todos os pixels, todos os adaptadores,
renderização proprietária, funcionamento do Lake ou áudio. Comparação visual
manual continua útil, mas não substitui nem reclassifica um teste não executado.

## Argumentos e resultados

```text
LakeFixGraphicsCheck.exe --automated|--manual --desktop-confirmed
  --no-operational-audio-confirmed
  [--width 1088 --height 612] [--report NOVO-ARQUIVO.log]
LakeFixGraphicsCheck.exe --self-test [--width 1088 --height 612]
LakeFixGraphicsCheck.exe --help
```

Canvas configurável: largura 320–4096, altura 180–4096. A janela é ajustada à área
de trabalho disponível, sem mudar o canvas. O relatório UTF-8 não sobrescreve
arquivo existente. Sem `--report`, fica junto ao EXE com timestamp/PID.
O script cria um subdiretório único em `graphics-check-runs`.

| Exit | Significado |
| --- | --- |
| 0 | PASS restrito às etapas executadas; SKIP de monitor negativo é permitido |
| 1 | Falha de setup, estado, pixels, isolamento ou cleanup |
| 2 | Não executado: confirmação/desktop indisponível, argumentos ou arquivo inválidos |
| 3 | Inconclusivo: oclusão, readback indisponível ou desktop deixou de ser seguro |

Não trate exit 2/3 como aprovação. Preserve os relatórios inclusive de falha.

## Build x86 isolado (Zig 0.14.1)

A integração do target em CMake/Build é separada. Build direto, sem editar os
arquivos do presenter ou o build existente:

```powershell
New-Item -ItemType Directory -Force build-graphics-next | Out-Null
& .\build-tools\python\ziglang\zig.exe c++ -target x86-windows-gnu `
  -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 `
  -O2 -Wall -Wextra -Wpedantic src/graphics_check.cpp src/ddraw_scale.cpp `
  src/profile.cpp -luser32 -lgdi32 -lwtsapi32 -municode `
  -o build-graphics-next/LakeFixGraphicsCheck.exe
```

Não instalar/injetar este EXE no Lake. Não adicionar esse teste gráfico à execução
automática genérica de CI sem a autorização/garantia explícita de desktop.
Quando isso não puder ser garantido, registrar exatamente: **compilado; etapas
gráficas não executadas por ausência de garantia de desktop ativo/desbloqueado**.

## Validação desta entrega (2026-11-01)

* Zig 0.14.1, x86, `-O2 -Wall -Wextra -Wpedantic -Werror`: compilou sem warnings;
  artefato em `build-graphics-next/LakeFixGraphicsCheck.exe`, PE Machine `0x014c`.
* Self-test CPU: PASS em 1088×612, 320×180, 4096×4096, 320×4096, 4096×180 e
  701×503; sintaxe do runner PowerShell validada.
* Recusa sem confirmação: exit 2 tanto no EXE quanto no runner; relatório
  existente preservado. Esses testes não carregaram DirectDraw/criaram janela.
* **Etapas gráficas reais NÃO EXECUTADAS**: não havia garantia de desktop
  ativo/desbloqueado. Readback, resize, restauração e QI nativo estão
  implementados, mas não foram observados nessa entrega. Não há aprovação
  gráfica, do Lake ou de áudio derivada desses testes CPU/build.
