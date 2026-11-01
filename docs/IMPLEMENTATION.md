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

## Camadas de evidência

1. **Lógica/matemática:** geometria, políticas, configuração, métricas e scripts/mock.
2. **Win32/COM sintético:** janelas próprias, injeção em fixture própria, proxies com
   objetos falsos, sandbox de setup. Não comprova GPU nem renderer proprietário.
3. **DirectDraw real próprio:** diagnóstico independente; desktop/GPU devem ser
   registrados. HRESULT bem-sucedido não substitui verificação de pixels/estado.
4. **Lake real OFFLINE:** não executado nesta revisão. Requer instalação legítima
   admitida, isolamento e ausência de operação de áudio. OFFLINE não isola a rede.

## Matriz de execução (atualizada por etapa validada)

| Requisito / classificação inicial | Arquivos | Implementação / verificação prevista | Evidência atual / pendência |
| --- | --- | --- | --- |
| 1. Baseline e comparação | este documento, build/testes | Snapshot isolado, comandos reais, preservar HEAD | Baseline executada acima |
| 2. Falhas de proxy/layout: risco estático de escala parcial | `fix.cpp`, `ddraw_scale.*`, testes | Fail-closed de criação, estágio explícito, regressões de falha | Em implementação; Lake pendente |
| 2. Erros e corrida launcher: defeitos demonstráveis | `launcher.cpp`, políticas/testes novos | Early exit/erro/timeout distintos, snapshot limitado, mutex, handshake/identidade | Em implementação |
| 3. Captura externa perde ambos eixos: defeito lógico | `scale_geometry.h`, `fix.cpp`, fixture | Separar hit-test de transformação contínua sem clamp universal | Regressão antes da correção |
| 3. Deltas pequenos: matemática, sem semântica real comprovada | geometria/diagnóstico | Instrumentação restrita; acumulação comportamental desativada | Investigação proprietária pendente |
| 4. Restore não invalida letterbox: risco estático reproduzível COM | `ddraw_scale.cpp`, presenter test | Invalidar após sucesso, preservar HRESULT/reentrância | Em implementação |
| 4. Critério de frame amplo/publicação prematura: defeito lógico | presenter/provider, testes | Admitir papel/dimensões/destino; publicar somente após sucesso | Em implementação |
| 5. Supressão global/log síncrono: defeito de diagnóstico/risco | presenter, logger, testes | Limites por operação, fila fixa, perdas, consumo fora do renderer/VEH | Em implementação |
| 5. Inspeção omite host: defeito demonstrado no script | `Inspect-WindowState.py`, snapshots | Papel host/canvas e propriedade apenas no host, sem textos privados | Pendente |
| 6. Estatísticas/sessão longa: melhoria de produto | DLL, monitor, testes | Stats baixa frequência; duração/arquivos limitados | Pendente |
| 7. Reinstalação/remoção parcial: defeitos de lifecycle | setup engine/testes | Dados preservados reconhecíveis, ausência idempotente, recuperação | Em implementação independente |
| 7. Upgrade/rollback transacional: melhoria de produto | setup engine | Staging, identidade anterior, configuração preservada | Não alegar suporte sem testes |
| 8. Proveniência tardia/promoção parcial: defeitos de distribuição | scripts, testes, build | Build manifest, ligação dos testes, candidate check separado, rollback | Em implementação independente |
| 8. Assinatura/símbolos: melhoria de distribuição | scripts/build/CI | Ordem final e candidato não assinado, símbolos próprios | Certificado ausente; distribuição pendente |
| 9. Diagnóstico DirectDraw independente: melhoria de teste | aplicação própria, build | DD7 real, padrões próprios, geometria/pixels/estado | Pendente desktop/GPU específico |
| 10. Configuração/UI/posição: melhoria de produto | config, DLL/setup, testes | Sessão imutável, schema/origem, UI simples, posição recuperável | Pendente; sem mudança ampla de DPI |
| 10. Cursor híbrido: hipótese proprietária | UI policy/docs | Preservar fallback e opções atuais | Sem evidência: híbrido desativado |
| 11. Falhas/símbolos: melhoria de diagnóstico | DLL/launcher/build | Build/etapa/códigos; sem minidump em VEH | Coletor externo opcional pendente |
| 12. CI x86 e desempenho | workflows/docs | Zig fixado/MSVC, setup/mocks, sem Lake em CI | Pendente; sem alegação de FPS/leak |
| 12. Ensaio 15–30 min/2–4 h e matriz Lake | `VALIDATION.md`, `PERFORMANCE.md` | Mesmo cenário/configuração, recursos e tendências | Não executado; ambiente isolado não estabelecido |

## Contratos preservados

Allowlist Build 007 x86 não ampliada; canvas fixo configurável (default 1088×612),
janela inicial lógica 1024×576, mínimo 800×450 limitado à área útil,
`ScaleRendering=1`, `NativeResize=0`, 32 bpp. Sem mudança global de DPI/cor,
vtables nativas, capacidades, backend ou hooks de terceiros. Sem unload/hot unhook,
descarte de input/frames, upload ou binário proprietário no Git/pacote.

As atualizações desta matriz registrarão o commit de implementação e os comandos
realmente executados; itens não concluídos permanecem explicitamente pendentes.
