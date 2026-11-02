# Validação desta revisão 0.7

Base `ed8756b`, referência `5e9c4f9`; branch `work/reliability-0.7`.
Baseline e matriz requisito/implementação/teste/pendência:
[IMPLEMENTATION.md](IMPLEMENTATION.md). Nenhuma execução abaixo iniciou o Lake,
operou áudio/equipamentos ou alterou instalação operacional.

## Integração local executada antes dos commits finais

Windows 11 x64, executáveis/testes **x86 Release**, Zig **0.14.1**, C++17 `-O2 -g`,
`-Wall -Wextra -Wpedantic`; LLVM objcopy **23.1.4** para separar DWARF.
PowerShell 5.1 / Python 3.12.8. Não houve recompilação Debug para obter símbolos.

| Rodada local | BuildId | Resultado real |
| --- | --- | --- |
| `build-verify-zig` → `dist-verify-zig` | `e6167f690fd446f2821266635b91b522` | **18/18 suítes**, zero falhas; ABI inspeciona DLL final stripped |
| `build-verify-profile` → `dist-verify-profile` | `3a15e4fd65e04a5a8bf5ee5bc936a906` | **18/18 suítes**, zero falhas; profiling separado e não distribuível |
| `build-verify-setup` → `dist-verify-setup` | `ac970ebb3eb74aa192f42aa3a502f4a2` | **2/2 suítes**, zero falhas; **421 checks** sandbox, **1 skip** opcional (symlink 1314, não PASS); payload final exit 0 |
| `build-verify-graphics` | `bbde1086b53147118e1da597f3997080` | Seis self-tests CPU na imagem final; GPU **não executada** |
| `build-cmake-verify`, `LAKE_BUILD_SETUP=ON` | CMake manual, não pacote | **18/18 CTest**, zero falhas, **300,29 s**; sandbox 273,94 s; `--verify-payload` exit 0 |

Runtime normal/profiling e setup acima congelaram commit
`04ddea7682ba30efc89a815ea74949ad73bf7bcf`, **SourceDirty=true**, fingerprint
`46e20b8456fcfce56531fc4f72893e33123773fdc31a623506becec0a9b31e2e`.
São evidência intermediária rastreável, não alegação de fontes limpas. O setup
referencia exatamente o manifesto/payload do runtime normal, não o profiling.

As 18 suítes incluem: geometria **129**, UI **104**, presenter **211**,
profiler OFF **97** / ON **158**, startup **84**, config/layout **22**, ABI **7**,
logger **154 checks em 13 casos** (mais 13 verificações do watchdog), métricas
**22**, monitor **9**, opções de build mock **9**, snapshots Python **4**, recibos
dos dois PEs e quatro runners Win32 (safe/native-resize/exit/scaled). Os runners
escalados cobrem separadamente o canvas 1536×864 e o default 1088×612, minima,
recriação ANSI/Unicode, input capturado main/child e veto de fechamento.

Resultados adicionais realmente executados:

- Proveniência **30** cenários e publicação mock **27**, zero falhas. O teste de
  publicação foi repetido com o pacote real `out/verify-0.7` como entrada de leitura;
  não publica nem consulta o remoto de verdade.
- Splitter DWARF **38** cenários nos PEs próprios previamente construídos de
  `build-validation-zig` (BuildId `02f369e1b3bc4b27b0b3c737e95715b8`); PEs não
  executados por esse teste. Recibos/PDBs dos builds novos: **24** cenários runtime,
  **12** setup e **12** GraphicsCheck, incluindo GUID/age/MSF e adulterações rehashed.
- Pacote real montado, ZIP de seis arquivos conferido byte a byte contra runtime
  testado e payload do setup; `Test-Candidate.ps1` aprovado com opt-in unsigned.
- Stderr dos quatro scripts de build vazio. Sem warnings/erros de compilação
  encontrados nos logs; CMake emitiu aviso de identificação ASM desconhecida do
  wrapper Zig, mas compilou/linkou o manifesto e todos os alvos/testes.

Logs locais ignorados: `build-verify-*.log`,
`build-*/.build-evidence/<BuildId>/*.txt`, `build-cmake-verify-*.log`,
`build-*-link-tests.log`, `build-provenance-post-argv.log`. Proveniência completa
registra hashes de cada teste/artefato e comandos de compilação locais; esses
caminhos/logs privados não são assets do pacote. A rodada inicial teve argumentos
nulos de flags opcionais omitidos pelo PowerShell; o build final passou a recusá-los
e registrar apenas os argv efetivos.

Uma chamada agregada de testes de scripts excedeu o timeout externo de 120 s e
**não foi contada como aprovação**. As suítes foram executadas novamente completas
com limite maior. Um build integrado anterior a estes terminou seus testes mas
foi corretamente invalidado por fontes alteradas durante o build: ele não foi
promovido a candidato distribuível.

## Evidência isolada anterior (não substitui a integração acima)

### Falha detectada na primeira rodada limpa de profiling

Build `188c61b837a94e56bb27ee1f240264ee`, fontes limpas de `2f7a1d4`: a fixture
`windows_safe_no_rebuild` falhou ao exigir os logs de quit. O build ficou
**Building**, não foi promovido e não é contado como aprovação. Os dois registros
estão no arquivo próprio (offsets 7184 e 21687, tamanho 32494 bytes); não foi uma
regressão demonstrada de quit ou perda desses registros. A fixture fazia uma única
tentativa de `Flush`, embora essa API rejeite contenção try-only por contrato
(a suíte do logger já cobre essa recusa determinística).

A correção limita a **quatro** tentativas somente da barreira no caminho de
controle do teste, cada export com espera de até 2000 ms, pausando 1 ms entre
recusas. Falha persistente continua sendo falha; a busca dos registros permanece
obrigatória. Três checks sintéticos cobrem sucesso imediato, recusa transitória
e limite permanente. Não se repetem input/frames/registros nem se altera o logger
de produção. As evidências falhas ficam em `build-candidate-profile`; a rodada
completa posterior usa novos diretórios/BuildIds.

Probe corrigido com `-O2 -g -Wall -Wextra -Wpedantic -Werror`, mantendo a mesma
DLL de teste otimizada da rodada falha: três execuções legacy/profiling e o
runner escalado completo passaram. Nas repetições 2 e 3, a métrica registrou
**uma recusa real da barreira seguida de sucesso**, no mesmo ponto do teste de
quit; os registros continuaram exigidos. Evidência local em
`build-fixture-barrier-regression/legacy-profile-{1,2,3}.log` e
`scaled-profile.log`. Isso valida a correção do harness, não um ganho no runtime.

### Rodadas iniciais por módulo

| Camada | Execução | Resultado / limite |
| --- | --- | --- |
| Matemática | Geometria/captura x86 `build-input-regression` | 129 checks, zero falhas; não pixels |
| Win32 sintético | Fixture do input contra DLL `ed8756b` | Falhas esperadas de captura externa; contrato antigo corrigido |
| COM sintético | Presenter original + regressões | 126 PASS / 10 FAIL esperados; [detalhes](PRESENTER-RELIABILITY.md) |
| COM sintético | Presenter revisado normal/profiling | 177 checks cada, zero falhas |
| Win32 sintético | Startup/políticas/filhos próprios | 84 checks, zero falhas; revisado com `-Werror` |
| Win32 sintético | ABI na DLL própria de produção | 7 checks, zero falhas; query sem init e init inválido recusado |
| Matemática/política | Configuração/layout guard | 19 checks, zero falhas; sem corrupção em proprietário |
| Concorrência sintética | Ring diagnóstico | 13 cenários, 153 checks, zero falhas; não prova I/O sem pausa do SO |
| PowerShell/Python | Métricas/monitor/inspeção | 22 + 9 checks / 4 testes Python, zero falhas |
| Scripts/mock | Proveniência | 29 checks, zero falhas; simulações não são testes dos EXEs fictícios |
| Scripts/mock | Opções de build | 7 checks, zero falhas; INI editado preservado |
| CPU própria | GraphicsCheck em seis canvases | Self-tests aprovados; GPU **não executada** |

As contagens acima são execuções intermediárias locais, não aprovação do pacote.
Build integrado/proveniência registrará somente seus próprios logs/saídas/testes.

## Testes externos não executados

- **Lake real OFFLINE:** ambiente isolado/sem operação de áudio não estabelecido;
  não se iniciou o Lake. Sem uso de hashes alternativos ou presets/rede automatizados.
- **GPU/desktop real:** falta garantia de desktop ativo/desbloqueado. Compilar,
  CPU self-test, QI falso ou HRESULT não substitui readback/inspeção visual.
- **MSVC/MinGW GCC local:** SDK/compiladores não disponíveis inicialmente. CI foi
  preparado; resultado remoto deve ser registrado separadamente, não presumido.
- **Certificado/assinatura:** nenhum configurado. Perfil Signed recusado;
  candidato de desenvolvimento é explicitamente não assinado.
- **Áudio/hardware e desempenho Lake 15–30 min / 2–4 h:** não executados; sem
  alegação de FPS, leak corrigido ou confiabilidade em evento.

## Procedimentos de continuação

1. Build/teste x86 conforme README; conferir BUILD-PROVENANCE e os testes do build.
2. GraphicsCheck: seguir [GRAPHICS-CHECK.md](GRAPHICS-CHECK.md), guardar ambiente,
   método de readback/pixels e inconclusivos/skips separados de PASS.
3. Lake legítimo admitido e isolado: matriz de [VALIDATION.md](VALIDATION.md),
   DPI 100/125/150/200%, monitores/negativos, resize/foco/modais/veto/saída,
   minimize/restore, bloqueio/RDP/hotplug e perda/restauração quando pertinente.
   Não controlar equipamentos nem inferir isolamento de rede a partir de OFFLINE.
4. Performance: [OBSERVATION.md](OBSERVATION.md), mesma cena/canvas/viewport/DPI,
   aquecimento/duração e repetições. Blt elapsed não é frame time GPU.
5. Assinatura/proveniência e publicação: [BUILD-PROVENANCE.md](BUILD-PROVENANCE.md).
   `release-check` é candidato local; preflight de publicação continua pendente
   na branch de trabalho e não autoriza merge/tag/upload/release.

Logs/dumps/capturas integrais permanecem locais e ignorados. Somente resumos
revisados, sem caminhos pessoais/equipamentos/presets, são documentação pública.
