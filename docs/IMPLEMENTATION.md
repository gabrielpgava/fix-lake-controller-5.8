# Confiabilidade — candidata de desenvolvimento 0.7

## Base e regras desta execução

- Base: `ed8756b1633649ff8a9129434326ab3bcfb47f9f`; branch `work/reliability-0.7`.
- Referência solicitada: `5e9c4f97633fe7b3e5d671548546c1f80e90667f`.
- Árvore inicialmente limpa. A diferença é a revisão de snapshots/cursor e profiling
  opcional de `ed8756b` (18 arquivos); não será substituída pela implementação antiga.
- Windows 11 Home Single Language 10.0.26300 x64; runtime/testes x86, Release,
  Zig 0.14.1 (`x86-windows-gnu`, C++17, `-O2`). Python 3.12.8, PowerShell 5.1.
  MSVC, CMake e MinGW GCC não estavam disponíveis no PATH inicial.
- Toolchain portátil obtida com `python -m pip install --target .\build-tools\python ziglang==0.14.1`.
  Não modifica PATH global nem configuração do Lake. Ferramentas/saídas são ignoradas.
- Sem execução do Lake, instalação operacional, áudio, presets, rede ou hardware.
  Nenhuma autorização de release/tag/merge é presumida.

## Baseline reproduzível

Snapshot independente em worktree detached, preservando a branch de implementação:

```powershell
git worktree add --detach "$env:LOCALAPPDATA\Temp\opencode\lake-baseline-ed8756b" ed8756b1633649ff8a9129434326ab3bcfb47f9f
# Executado nesse worktree, com os três caminhos abaixo absolutos:
.\scripts\Build.ps1 -Toolchain Zig -ZigPath <repo>\build-tools\python\ziglang\zig.exe -BuildDirectory <repo>\build-baseline-ed8756b -OutputDirectory <repo>\dist-baseline-ed8756b
```

Resultado desta execução: build concluído; zero falhas e zero warnings/erros de
compilação encontrados no log `build-baseline-ed8756b.log` (local, ignorado).
Geometria: 119; UI: 104; presenter: 116; profiler OFF: 97; profiler ON: 158.
As demais suítes PowerShell/Win32 também terminaram com sucesso. Contagens históricas
do README não são evidência da candidata. Setup e publicação têm validações separadas.

Setup da baseline: executável sandbox original via `Start-Process -Wait -PassThru`
com stdout/stderr separados: **58 checks, zero falhas, 1 skip opcional** (symlink,
erro 1314; não contado como aprovação). A invocação do build com `*>` encontrou
`NativeCommandError` ao redirecionar stderr informativo; não é falha dos 58 testes.
Logs locais `build-baseline-setup-tests.stdout.log` / `.stderr.log`.

## Camadas de evidência

1. **Lógica/matemática:** geometria, políticas, configuração, métricas e scripts/mock.
2. **Win32/COM sintético:** janelas próprias, injeção em fixture própria, proxies com
   objetos falsos, sandbox de setup. Não comprova GPU nem renderer proprietário.
3. **DirectDraw real próprio:** diagnóstico independente; desktop/GPU devem ser
   registrados. HRESULT bem-sucedido não substitui verificação de pixels/estado.
4. **Lake real OFFLINE:** não executado nesta revisão. Requer instalação legítima
   admitida, isolamento e ausência de operação de áudio. OFFLINE não isola a rede.

## Matriz requisito → implementação → teste → evidência → pendência

**Implementado** abaixo significa código integrado e evidência no nível indicado,
nunca homologação do Lake/GPU/áudio. Builds, IDs e commits exatos:
[VALIDATION-07.md](VALIDATION-07.md). As contagens são as rodadas integradas desta
revisão, não as contagens históricas do README.

| Requisito / classificação | Implementação | Teste e evidência executada | Pendência / limite |
| --- | --- | --- | --- |
| 1. Baseline/comparação — concluído | Worktree isolado de `ed8756b`, comparação com `5e9c4f9`; snapshots/profiling existentes mantidos | Baseline runtime e setup acima; regressões negativas de input/presenter/lookup | Nenhuma comparação de FPS Lake foi feita |
| 2. Proxy/layout parcialmente escalado — implementado | `fix.cpp`, `layout_policy.h`, `ddraw_scale.*`: admissão HRESULT separada, seed guardado, factory/primária fail-closed | 211 checks COM; 22 checks config/layout; fixtures de fechamento adiado com veto nativo | Renderer e modais proprietários não executados; sem promessa de fallback universal |
| 2. Startup/identidade — implementado | `launcher.cpp`, `component_contract.h`, `file_identity.h`: EXIT/erro/timeout distintos, retry limitado, mutex usuário/sessão, pins/ABI/hash; DLL confere módulo retornado e fixa seu lifetime | 84 checks startup, 7 ABI na DLL final; fixtures Contour homônimo, pin/reload e export estrangeiro/LastError | Corrida com launcher não participante e falhas de kernel não são eliminadas; [STARTUP-RELIABILITY.md](STARTUP-RELIABILITY.md) |
| 3. Captura externa perde eixos — corrigido | `scale_geometry.h`, `fix.cpp`: extrapolação assinada por eixo para captura main/child; novo down em margem rejeitado | 129 checks geometria; fixture escalada cobre L/R/M/X, modifiers, repeat-peek e canvas default | Input proprietário, DPI/monitores reais pendentes; [INPUT-RELIABILITY.md](INPUT-RELIABILITY.md) |
| 3. Deltas fracionários — instrumentado, sem mudança comportamental | `[diagnostics] DetailedInput=0/1`, contadores no par de RVAs admitido | Matemática mostra perda de frações; fixture nunca admite RVAs proprietários | Semântica real não comprovada: acumulação/híbrido continuam desativados |
| 4. Restore/ReleaseDC e cache — corrigido sinteticamente | `ddraw_scale.cpp`: gerações de owner/primária; invalidar só após sucesso; sem locks próprios em COM/callbacks | 211 checks COM normal/profiling, inclusive invalidação reentrante, HRESULT/LastError e aliases | Perda/restauração real de dispositivo e recarga de bitmaps do aplicativo não executadas |
| 4. Frame amplo/publicação prematura — corrigido | Papel/extent/caps/flags/destino explícitos; query sem publicação, commit pós-sucesso do mesmo snapshot | Suíte COM e fixtures integradas; [PRESENTER-RELIABILITY.md](PRESENTER-RELIABILITY.md) | Native aliases/Flip/BltBatch/outros caminhos não viram emulação universal |
| 5. Log síncrono/supressão global — implementado | `session_diagnostics.*`: ring 256, worker por evento, teto 4 MiB; presenter limita pares operação/HRESULT; só registros diagnósticos podem ser descartados | 154 checks em 13 casos isolados, mais 13 verificações do watchdog; COM cobre tabela/limites/reentrância | Sem garantia hard real-time/crash-safe ou persistência sob falha de disco; [DIAGNOSTICS.md](DIAGNOSTICS.md) |
| 5. Inspector omite host — corrigido | `Inspect-WindowState.py`: host legado/escalado/canvas, propriedade somente no host; sem textos de controles | 4 testes Python | Snapshot read-only não comprova pixels nem responsividade futura |
| 6. Observação longa — implementado em ferramenta, não ensaiado no Lake | Monitor até 24h, limite de amostras/identidade retida; estatísticas periódicas da DLL | 22 checks métricas + 9 de lifecycle/falhas; nenhum alvo terminado | Ensaios de 15–30 min e 2–4 h não executados; contagens proxy não medem toda memória nativa/GPU |
| 7. Reparo/remoção parcial nativos — implementado | `setup_engine.*`: recibo persistente, ausências idempotentes, conflito preservado, mensagens dos objetos mantidos, migração INI com backup/journal | 421 checks sandbox, zero falhas, 1 skip opcional de symlink (1314, não PASS) | Worker real de autorremoção/Apps não executado; perda de energia/EDR não simulados; [SETUP-RECOVERY.md](SETUP-RECOVERY.md) |
| 7. Upgrade/rollback — implementado com limite | `--upgrade`/`--rollback`, archives, renames sem overwrite, registro GUID exclusivo nos testes | Sandbox cobre checkpoints de promoção/retomada/rollback, INI/logs/dados desconhecidos preservados | Apenas uma geração anterior; sem cadeia arbitrária, redo/purge ou coleta automática |
| 7. Scripts PowerShell legados — **não atualizados** | `Install.ps1`/`Uninstall.ps1` conservados, não usados pelo candidato nativo | Sem nova evidência lifecycle desses scripts | Ainda não oferecem o contrato de recuperação do setup novo; não inferir migração automática entre os dois métodos |
| 8. Proveniência/promoção de pacote — implementado | `Build-Provenance.ps1`: snapshot antes do compile, comandos reais, BuildId, hashes finais/testes/payload; promoção por diretório/journal; candidate separado de publish | 30 checks provenance, 27 guards de publicação mock; pacote real local verificado byte a byte | Atestação do runner não é assinatura; preflight `HEAD == origin/main` não foi aprovado na branch |
| 8. Símbolos próprios — integrado | Split DWARF antes de hash/testes; PDB exato do link otimizado, CodeView/MSF GUID/age e recibos por BuildId; CMake ABI aponta à DLL final | 38 cenários splitter; 24 mutantes runtime, 12 setup e 12 GraphicsCheck; suíte obrigatória de recibos | Debug fonte em debugger e MSVC local não executados; stripping não anonimiza CodeView; [SYMBOLS.md](SYMBOLS.md) |
| 8. Assinatura — **não implementada** | `DevelopmentUnsigned` exige opt-in; `Signed` recusado | Guards rejeitam alegação falsa de assinatura | Certificado, pinning, timestamp/trust e pipeline Signed reais pendentes |
| 9. Diagnóstico DD7 próprio — implementado, GPU **não executada** | `graphics_check.cpp`: própria janela/DD7/padrões/markers/pixels/controle/cleanup; gates desktop e ausência de áudio operacional | Build x86 + seis self-tests CPU; nenhum carregamento DirectDraw nesses testes | GPU/desktop real, readback e inspeção visual precisam de condições confirmadas; [GRAPHICS-CHECK.md](GRAPHICS-CHECK.md) |
| 10. Configuração — snapshot implementado | `session_config.h`: schema 1, origem/override/hash, sessão imutável; setup mescla só chaves ausentes | 22 checks política/layout + edição live na fixture; ABI 2 / versão 0.7.0 | UI comum de configuração/seleção de perfis **não implementada** |
| 10. Posição de janela — **não implementada** | Mantidos dimensionamento/work-area/defaults e comportamento anterior | Fixtures de canvas/host/minimum; não são teste de persistência | Salvar/restaurar posição com DPI/múltiplos monitores segue pendente |
| 11. Sessão/falhas — parcialmente implementado | GUID de sessão, UTC/tick, FILETIME de criação do processo, build/etapa/códigos; VEH não coleta dump | Testes logger/startup/saída; contador de perda e fallback | Coletor externo de dump e retenção automática entre sessões **não implementados**; recursos PE VERSIONINFO também pendentes |
| 12. CI x86 — preparado | `.github/workflows/windows-x86.yml`: Zig fixado/MSVC, setup/mocks, sem Lake/assinatura/publicação; símbolos só em dispatch opt-in | YAML/workflow revisado; execução local Zig separada | Não presumir resultado remoto; MinGW GCC/MSVC locais não executados |
| 12. Matriz Lake/DPI/RDP/hotplug/desempenho — **não executada** | Procedimentos em `VALIDATION.md`, `PERFORMANCE.md`, `OBSERVATION.md` | Nenhuma execução de Lake/áudio/equipamentos nesta revisão | Ambiente legítimo isolado não estabelecido; nenhuma alegação de FPS, leak ou homologação em campo |

## Contratos preservados

Allowlist Build 007 x86 não ampliada; canvas fixo configurável (default 1088×612),
janela inicial lógica 1024×576, mínimo 800×450 limitado à área útil,
`ScaleRendering=1`, `NativeResize=0`, 32 bpp. Sem mudança global de DPI/cor,
vtables nativas, capacidades, backend ou hooks de terceiros. Sem unload/hot unhook,
descarte de input/frames, upload ou binário proprietário no Git/pacote.

Itens de produto não implementados acima são escopo restante de engenharia, não
testes externos meramente bloqueados pelo ambiente. Não há alegação de conclusão
integral desses itens ou de aprovação de uso operacional.
