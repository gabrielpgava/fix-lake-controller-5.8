# Validação desta revisão 0.7

Base `ed8756b`, referência `5e9c4f9`; branch `work/reliability-0.7`.
Baseline e matriz requisito/implementação/teste/pendência:
[IMPLEMENTATION.md](IMPLEMENTATION.md). Nenhuma execução abaixo iniciou o Lake,
operou áudio/equipamentos ou alterou instalação operacional.

## Candidato final de fontes limpas

Snapshot compilado: **`28fe4e3057604d7428987d7a2b0692e20cb0619b`**,
`SourceDirty=false`, fingerprint
`5cec08f20c98822580faf4133b88a72953f2d7614093f74a0ca448e400be2b5f`.
Runtime, profiling, setup e GraphicsCheck têm exatamente esse snapshot. Fontes
ficaram congeladas durante todos esses builds/testes/payload/pacote. Este relato
posterior é somente documentação: **não muda a identidade dos binários para o
HEAD posterior**. Não há alegação de reprodução bit a bit em outra máquina.

| Artefato / evidência final | BuildId | Resultado |
| --- | --- | --- |
| `dist-final-zig` | `b0fb95b656db4a5197c9d37645940f7a` | **18/18 suítes**, zero falhas |
| `dist-final-profile` | `22e12ded4b314b45a42d53e4628942e3` | **18/18 suítes**, zero falhas; executado depois do normal, separado do pacote |
| `dist-final-setup` | `73dc64699f32432aa18961bd4be523bd` | **2/2 suítes**, zero falhas; **421 checks**, 1 skip opcional symlink/1314 (não PASS); payload final exit 0 |
| `build-final-graphics` | `53724b49a6c84c8b86f4c51303a7db5f` | Seis self-tests CPU; gate sem confirmações retorna **2/NOT_EXECUTED**, sem iniciar o EXE gráfico |
| CMake `LAKE_BUILD_SETUP=ON`, runtime previamente testado acima | Manual, não pacote | **18/18 CTest**, zero falhas, **282,38 s**; sandbox 259,27 s; payload exit 0; ABI aponta à DLL final stripped |

O setup liga o payload ao runtime `b0fb95b656db4a5197c9d37645940f7a` e ao manifesto
SHA-256 `32d1c7aeb180de8c7dd3e986bfe08200686c5c4618573ac69b0a5fe545b66b1f`.
Todos os testes exigidos retornaram exit 0. A correção da barreira acrescentou
três checks por execução Win32 comum/escalada; os checks dos módulos nativos
(129/104/211/97/158/84/22/7/154) permanecem os descritos na tabela da integração.

Testes adicionais **reexecutados nos artefatos finais**: **38** cenários splitter,
**24** recibos/PDB runtime, **12** setup, **12** GraphicsCheck; provenance **30** e
guards mock **27**, todos sem falhas. PDBs correspondem por GUID/age ao CodeView
dos mesmos links otimizados; não foram recriados em Debug. Stderr dos quatro
scripts de build vazio; nenhum warning/erro de compilação encontrado. O aviso
CMake de identificação ASM do wrapper é separado de erro de compilação.

Código/layout executável da DLL e launcher tem o mesmo digest que no primeiro
candidato limpo de `2f7a1d4`: a correção foi no harness, não no runtime. Os módulos
`profile.*`, `ui_quality.h` e seus testes continuam sem diferença contra `ed8756b`.

### Arquivos locais e hashes finais

Pacote: **`out/candidates/v0.7.0/`**, experimental **DevelopmentUnsigned**.
O ZIP contém exatamente seis arquivos MIT, byte a byte iguais aos do payload e
runtime testado. Promoção real por diretório terminou `Committed`; o candidato
anterior de `2f7a1d4` permanece no backup verificado, sem sobrescrita parcial.

| Arquivo | Bytes | SHA-256 |
| --- | ---: | --- |
| `LakeWindowedFix-v0.7.0-win32.zip` | 452911 | `27bd1d93640928c633a93287c8638f42636cebd27620f3df80b3ff34f35519b7` |
| `LakeWindowedFix-Setup-v0.7.0.exe` | 1667584 | `5739235eddb6b0f1e884c54110a1324fc3130d9cff804fe3407d24f66d75c7e9` |
| `SHA256SUMS.txt` | 200 | `1165708decb8b8c47ee59c31a1347825fa3326d49ba729f452da5e3b451258c5` |
| `RELEASE-MANIFEST.json` | — | `669192ffc03981064aac89ec80fd8c2f1003910e9950e6ecbbc1995acb3b67bc` |

Runtime stripped: DLL **503808** bytes, SHA-256
`c602a026702c02d8f5dc53e56ae84cea5a88c715dd4ab08dcc3ba4467160c3c9`;
launcher **449024** bytes, SHA-256
`d1fea231c6ec95ddac718fa93a6cc56eb774f6ae534605f4c7b05f5903005551`.
Imagens originais, DWARF, PDBs e recibos ficam somente em
`build-final-*/symbols/<BuildId>/`, fora do pacote do operador.

### Comandos da rodada final

PowerShell na raiz; sem download por esses scripts:

```powershell
$env:TEMP = Join-Path $env:LOCALAPPDATA 'Temp\opencode'
$env:TMP = $env:TEMP
$zig = '.\build-tools\python\ziglang\zig.exe'
.\scripts\Build.ps1 -Toolchain Zig -ZigPath $zig -BuildDirectory build-final-zig -OutputDirectory dist-final-zig
.\scripts\Build.ps1 -Toolchain Zig -ZigPath $zig -BuildDirectory build-final-profile -OutputDirectory dist-final-profile -EnableProfiling
.\scripts\Build-Installer.ps1 -Toolchain Zig -ZigPath $zig -RuntimeDirectory dist-final-zig -BuildDirectory build-final-setup -OutputDirectory dist-final-setup -AllowUnsignedDevelopment
.\scripts\Build-GraphicsCheck.ps1 -ZigPath $zig -BuildDirectory build-final-graphics
.\scripts\New-ReleasePackage.ps1 -RuntimeDirectory dist-final-zig -InstallerDirectory dist-final-setup -OutputDirectory out/candidates/v0.7.0 -AllowUnsignedDevelopment
.\scripts\Test-Candidate.ps1 -PackageDirectory out/candidates/v0.7.0 -AllowUnsignedDevelopment
.\tests\Provenance.Test.ps1
.\tests\Release-Package.Test.ps1 -PackageDirectory out/candidates/v0.7.0
.\tests\Symbols.Test.ps1 -InputDirectory build-final-zig -BuildId b0fb95b656db4a5197c9d37645940f7a
.\tests\Link-Symbols.Test.ps1 -SymbolDirectory build-final-zig/symbols/b0fb95b656db4a5197c9d37645940f7a -BinaryDirectory dist-final-zig -BuildId b0fb95b656db4a5197c9d37645940f7a
```

`Link-Symbols.Test.ps1` também foi executado para setup e GraphicsCheck com seus
IDs/diretórios da tabela. Toolchain: Zig **0.14.1**, SHA-256 do driver
`1ddf230367e07738c4a769eae66c1db7469e37bd520e81c86356453d0db2b9fd`;
LLVM objcopy **23.1.4** conforme [SYMBOLS.md](SYMBOLS.md). Cada chamada real do
compilador/argv e seus hashes está no manifesto local; **nenhum argv vazio** foi
atestado na rodada final. Não foram reutilizados logs de testes anteriores.

CMake portátil **3.31.10**, Ninja **1.13.2**, toolchain Zig x86 local já descrita
na rodada inicial. Reconfigurado com `LAKE_BUILD_SETUP=ON`,
`LAKE_RUNTIME_DIRECTORY=<repo>/dist-final-zig`,
`LAKE_BUILD_STAMP_HEADER=<repo>/build-final-zig/generated/lake_build_stamp.h`,
`LAKE_TEST_PRODUCTION_DLL=<repo>/dist-final-zig/LakeWindowedFix.dll`; build com
`--parallel 2`, CTest com `--output-on-failure --no-tests=error --parallel 1`.
O setup CMake manual usa esse stamp só para testes; não é o EXE empacotado.

Logs/recibos locais: `build-final-*.log`,
`build-final-*/.build-evidence/<BuildId>/*.txt`, `build-cmake-final-*.log`,
`BUILD-PROVENANCE.json` nos diretórios finais e stores de símbolos. Todos ignorados
pelo Git; somente este resumo revisado é versionado.

`Publish-Release.ps1 -DryRun -AllowUnsignedDevelopment` foi **recusado**, como
esperado: o snapshot não é `origin/main` (`ed8756b`). Candidate check aprovado não
é autorização para merge/tag/release. Nenhuma dessas operações foi feita.

### Commits da implementação

| Commit | Etapa |
| --- | --- |
| `0abfc4a` | Baseline isolada e matriz inicial |
| `e379fba` | Captura contínua independente por eixo |
| `04ddea7` | Inspector host/canvas e monitor limitado |
| `0300fd1` | Presenter/COM/admissão/Restore/cache |
| `c3d4bff` | Setup, recibos, reparo, upgrade/rollback |
| `54383fd` | Startup/ABI/identidade, logger/config, builds/payload/proveniência/símbolos e diagnóstico próprio |
| `2f7a1d4` | CI x86, wrappers e documentação/escopo |
| `28fe4e3` | Barreira try-only do harness, snapshot do candidato final |

O commit posterior deste relato não recompila nem carimba esses artefatos. As
pendências de engenharia e testes externos continuam explícitas em
[IMPLEMENTATION.md](IMPLEMENTATION.md); CI remoto/MSVC não são presumidos aprovados.

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
