# Candidata de otimização posterior à 0.6

Esta é uma revisão de desenvolvimento, não uma nova release publicada. As distribuições históricas, os binários proprietários, o INI padrão e os atalhos existentes não são substituídos. Não há ganho adicional de FPS/CPU ou correção de leak comprovados por estas alterações.

## Alterações

- **Cursor:** a observação de `GetCursorInfo` é reutilizada somente dentro da mesma chamada de pintura, quando já confirma a seta esperada visível e nenhuma compensação foi tentada. Recuperação, falha, cursor inesperado e supressão conservam a consulta de confirmação. Não há cache entre eventos; créditos de `ShowCursor` e fallback bitmap continuam existindo.
- **Apresentação:** destino, fonte desembrulhada e configuração são capturados sob uma aquisição compartilhada do registro, em vez de até três aquisições separadas. Os pins permanecem até terminar a chamada. Callbacks, COM e destrutores executam fora do lock. O cache de geometria, a consulta real de dimensões e os contratos de erro/lifetime não foram removidos.
- **Diagnóstico opcional:** durações QPC agregadas em memória, habilitadas somente na compilação de diagnóstico. Sem novas threads, timers, polling ou gravação por quadro. O build normal compila esse caminho como no-op.

Permanecem canvas **1088×612**, janela inicial **1024×576 lógica**, mínimo **800×450 lógico**, **32 bpp**, `SmoothFonts=1`, `SystemCursor=1` e `NativeResize=0`.

## Compilar sem sobrescrever a 0.6

Com Zig 0.14.1 completo já disponível:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:/tools/zig/zig.exe -BuildDirectory build-next -OutputDirectory dist-next
```

O comando compila e executa apenas testes próprios/sintéticos. Não inicia o Lake real nem instala o patch. `-BuildDirectory` também permite isolar os arquivos intermediários; não selecione as distribuições históricas como destino.

Os testes de opções de build usam um compilador PowerShell simulado, saídas sintéticas isoladas e `-SkipTests`, sem executar os EXEs/DLLs fictícios. Conferem a flag de profiling, separação de saídas e preservação de INI editado. A compilação real validada neste ambiente usa Zig; MSVC/MinGW/CMake continuam alternativas não exercitadas aqui.

## Build de diagnóstico

```powershell
make profile
```

Esse alvo **não abre o Lake**, não gera instalador e não publica release. Usa `build-zig-profile/` e `dist-profile-v0.6.0/`, separados do runtime normal e da distribuição 0.6 validada. Não exige edição do INI. Alternativa:

```powershell
.\scripts\Build.ps1 -Toolchain Zig -ZigPath C:/tools/zig/zig.exe -EnableProfiling -OutputDirectory dist-profile
```

No CMake, a opção equivalente é `-DLAKE_ENABLE_PROFILING=ON` (padrão `OFF`). CMake recusa combinar profiling com `LAKE_BUILD_SETUP=ON`. O fluxo comum `make build/package` não habilita profiling; não distribua manualmente a DLL de diagnóstico como runtime normal.

Os targets Zig/Clang-MinGW de diagnóstico usam `-fno-emulated-tls` para que o guard local à thread do relatório não aloque memória por emulação de TLS. Essa flag não é acrescentada ao runtime normal. A coleta usa um lock próprio com tentativa sem espera; a emissão copia os dados e libera o lock antes de chamar o logger, inclusive se ele consultar o snapshot novamente.

Após uma execução de teste explicitamente autorizada, o diagnóstico usa o log já existente ao lado da DLL, com fallback documentado em TEMP. Produz no máximo um resumo no primeiro `PostQuitMessage` observado da UI e outro no caminho coberto de `ExitProcess`. O primeiro é rotulado como **pedido de quit, não saída**; nenhum dos dois é captura universal de encerramentos. Saídas não cobertas, falhas de acesso ou disputa pelo lock do log podem deixar o relatório incompleto. Não há dump adicional em `DllMain` ou no observador de exceções.

As fixtures de política com janelas ocultas demonstram esse limite: podem destruir suas janelas, postar quit pela IAT nativa do próprio shim e retornar pelo CRT sem chamar os imports de término cobertos. Nesse caso o teste exige inicialização do profiler, mas não inventa um relatório de saída. As demais fixtures verificam os relatórios nos caminhos efetivamente interceptados.

## Interpretação

Os grupos separam o total de `SurfaceBlt`, snapshot do registro, `GetSurfaceDesc`, provider de viewport, `Blt` nativo, painter de letterbox, mapeamento da fila, getter do cursor, tratamento da pintura do cursor e logging/escrita de arquivo.

- As durações são ticks de **tempo decorrido QPC**, convertíveis pela frequência registrada. Não são tempo de CPU, latência física de mouse ou quadros exibidos.
- Escopos podem se sobrepor/nestear: **não some seus totais** como se fossem custos exclusivos. O total de `SurfaceBlt` inclui trabalho nativo e instrumentação. `NativeBlt` inclui encaminhamentos e fills, não apenas apresentações de imagens.
- Histogramas têm faixas em potências de dois ticks e uma última faixa aberta. Percentis derivados são limites de faixa, não medições exatas.
- Contadores saturados, amostras inválidas e descartes por concorrência precisam ser considerados; ausência de amostras não significa custo zero.
- A contabilização cobre escopos ativos e concluídos fora da supressão do próprio relatório. Escopos anteriores à inicialização, criados durante relatório ou encerrados dentro de seu callback são inertes/suprimidos, não amostras de custo zero.
- O profiler também tem custo. Compare o runtime normal sem instrumentação com a baseline para avaliar desempenho final; use o build instrumentado para localizar custos.
- A escrita do resumo não é incluída como trabalho de logging do próprio resumo. O logging comum continua síncrono; bufferização assíncrona foi adiada até existir evidência que a justifique.

## Validação e próximos ensaios

A baseline anterior às alterações foi recompilada em saída isolada: **1.159 verificações sem falhas**. A candidata final foi compilada com Zig 0.14.1 para Windows x86 e validada sem iniciar o Lake real:

| Validação | Resultado |
| --- | --- |
| Runtime normal, `dist-next/` | **1.487 PASS, zero FAIL, zero warnings**, profiling desativado. |
| `make profile`, `dist-profile-v0.6.0/` | **1.487 PASS, zero FAIL, zero warnings**, profiling habilitado. |
| Engine de instalação/remoção recompilado | **58 PASS, zero FAIL** em sandbox, um grupo opcional de symlink pulado por falta de privilégio; sem pedir elevação. |
| Guardas de publicação | **17 PASS**, Git/GitHub simulados, sem operações remotas. |

Cada build do runtime executa onze suítes: 119 geometria, 104 políticas de UI/cursor, 116 COM/presenter, 97 profiler desabilitado, 158 profiler habilitado, 22 métricas, sete opções de build e os cenários Win32/launcher anteriores. Há nove verificações adicionais do runner sobre o estado/alcance do diagnóstico. Os mesmos testes em dois builds não são medições independentes de desempenho.

O cursor confirma uma consulta no caminho visível esperado e consulta nova nas recuperações/falhas; as fixtures COM cobrem pins de destino/fonte, self-source, self inválido e reconfiguração reentrante do tuple de callbacks. A suíte habilitada do profiler verifica saturação, histograma, concorrência, contagem de descartes, `LastError` e callbacks fora do lock. Revisões estáticas independentes não encontraram bloqueadores nos contratos de cursor/presenter ou no backend de diagnóstico; isso não substitui ensaio com o aplicativo real.

Os arquivos originais e as distribuições 0.6 anteriores foram conferidos por hash e preservados. Não houve instalação, alteração de atalhos, commit, push ou publicação. Os resultados históricos em [PERFORMANCE.md](PERFORMANCE.md) não são resultados novos desta candidata.

Para uma comparação real posterior: autorização explícita, **OFFLINE**, uma instância por vez, mesma cena vazia, DPI, viewport, posição do mouse, aquecimento e duração; repetir com ordem alternada. Não configurar adaptadores/processadores, salvar/recuperar presets ou interagir com áudio em produção. Observar separadamente CPU, memória privada, resposta visual e visibilidade do cursor. A consulta de módulos carregados identifica a DLL efetiva; o caminho do EXE original sozinho não distingue lançamento pelo launcher.

Logs, CSVs, ferramentas, capturas e relatórios integrais ficam privados e fora do Git. Não há commit, push, instalação ou publicação implícitos no build.
