# Performance e usabilidade — candidata 0.6

> A revisão de desenvolvimento posterior, com snapshot conjunto, consulta de cursor reaproveitada e profiling opcional, está documentada em [OPTIMIZATION.md](OPTIMIZATION.md). As medições abaixo pertencem à 0.6 histórica; não comprovam ganhos da revisão nova.

**Implementada, com validação limitada em OFFLINE.** Mantém o renderer original, o canvas **1088×612**, a janela inicial **1024×576 lógica** e **32 bits de cor**. A saída nova é **`dist-v0.6/`**; não substitui as distribuições 0.3/0.4/0.5. Não é promessa de 60 FPS nem correção comprovada de vazamento de memória.

## Alterações implementadas

- Snapshot de geometria dentro de cada chamada de input e a mesma geometria no round-trip do cursor. Sem cache entre `PeekMessage` e dispatch, nem entre eventos.
- Classificação do cursor com uma única amostra de ponto/HWND por passagem, preservando as decisões de visibilidade, foco, captura e fallback.
- Painter GDI e fills dispensados quando todos os retângulos de letterbox têm área zero. A apresentação nativa, seus argumentos e seu HRESULT continuam reais.
- Configuração usada exclusivamente por diagnóstico consultada somente após falha; sem formatação de mensagens já emitidas uma vez. Locks de lifetime, pins e callbacks necessários preservados. Nos forwarders, uma reconfiguração reentrante seleciona agora o logger posterior à chamada nativa; há regressão explícita desse contrato.
- Mínimo configurável, para evitar reduzir acidentalmente a interface a um tamanho pouco legível.
- Monitor de recursos somente-leitura, para investigar memória e custo das opções em ensaios mais longos.

## Validação da candidata 0.6

Build Zig 0.14.1, Windows x86: **oito suítes, 1.159 PASS, zero FAIL e zero warnings de compilação**. Distribuição: 119 geometria + 77 políticas de UI + 86 COM/presenter + 22 métricas PowerShell + 65/63 modos legados + 5 saída + 722 escalado/launcher. Os testes do monitor importam um módulo aritmético puro; não precisam do Lake.

Reteste real, uma instância própria, **OFFLINE/canvas vazio**, Windows 11, DPI 125%, monitor principal **1920×1080/32 bpp**; área virtual total **2720×1080**, igual no início/fim. As janelas testadas permaneceram no principal:

| Medida | Resultado e limite |
| --- | --- |
| Arraste real até o mínimo padrão | Cliente **1000×562 físicos**, compatível com **800×450 lógicos** a 125%; retorno a 1280×720. |
| Opt-out e repetição do padrão em cópias privadas | `320/180` resultou em **400×225 físicos**, igual ao controle 0.5. Outra execução com INI padrão repetiu **1000×562**; ambas restauraram 1280×720. Distribuição/INI entregues não foram alterados para esses testes. |
| Pedido programático de 800×600 | O Windows efetivamente limitou o cliente a **1000×600**. O shim não acrescenta um clamp no hook de `SetWindowPos`, mas não garante que o sistema aceite tamanhos menores. |
| Home/Modules, clientes 1280×720 e 1000×600 | **48/48** cliques reconhecidos, seis por intervalo nominal de 20/40/80/120 ms e geometria. Sem captura durante DOWN/UP; limites das APIs não medem duração física do botão. |
| 16 mudanças 1000×600 ↔ 1280×720 | **16/16** observações compatíveis; primeira observação de barra: mediana **29,6 ms**, máximo **48,2 ms**, incluindo custo de captura. Não é FPS ou ganho comprovado: geometria/cena/história diferem da amostra 0.5. |
| Margens e foco | Quatro cliques nas barras pretas conservaram Home; Alt+Tab/retorno e minimizar/restaurar mantiveram resposta e interface completa. |
| Cursor após o percurso | Trajetórias de canvas/barra: posições **240/240 corretas em cada**, mas `GetCursorInfo` reportou **flags=0/handle nulo**, não `CURSOR_SUPPRESSED`. O bitmap permaneceu visível. Movimentos de mouse adicionais não demonstraram a seta nativa neste estado; não contar isso como sucesso da seta independente ou atribuir causa sem controle. |
| Cursor em execuções novas, mesmo build | Com opt-out: **240/240 visíveis** após resize e outras **240/240** após foco/minimizar-restaurar. Com INI padrão: **240/240** após arraste até o mínimo e **240/240** na barra após resize/margens; sem divergências de posição. Controle 0.5 também teve 240/240 após resize e foco. Não reproduziu uma falha geral da seta na 0.6, mas não explica o estado oculto anterior nem garante todas as transições. |
| Monitor empacotado | Smoke de 10 s, imagem admitida, dez amostras, intervalos CPU válidos. Limiar baixo encerrou somente o monitor após uma amostra; o mesmo alvo permaneceu vivo e responsivo. PID não-Lake recusado antes da coleta. |
| Repouso de cinco minutos | **300 amostras, 299 intervalos CPU válidos, zero slots perdidos**. CPU média ponderada **1,93% do total** dos 12 lógicos (23,10% de um core). Memória privada **313,96 → 297,74 MiB**, mínimo/máximo **284,93/332,86 MiB**; working set **121,38 → 104,15 MiB**. Handles **592 → 592**, GDI **229 → 231**, USER **3064 → 3064**. |
| Saída normal com monitor ativo | `WM_CLOSE` encerrou a sessão vazia diretamente com **exit 0**. O monitor terminou com `TargetExited` após 23 amostras; não seguiu um novo processo. A tentativa posterior de captura foi recusada porque o alvo já havia saído, não foi tratada como crash. |

A queda líquida de memória e os limites observados **não provam correção/ausência de leak**; a memória variou em ambas as direções, a sessão já estava aquecida e não é um controle equivalente aos 30 s históricos. Resultados integrais e capturas permanecem privados. Revisões independentes dos snapshots/mínimos e do monitor não encontraram bloqueadores estáticos; isso não substitui testes de outros cenários. O reteste final da mesma DLL confirmou novamente **1.159 PASS/zero FAIL**, sem recompilar ou trocar os binários operados. As **sete execuções reais**, incluindo controles, encerraram normalmente com código 0, sem salvar presets ou forçar término.

## Evidência que motivou as mudanças — 0.5

Ensaio **OFFLINE, canvas vazio**, Windows 11 `10.0.26300` x64, DPI 125%, monitor principal 1920×1080/32 bpp, 12 processadores lógicos:

| Medida | Resultado e limite |
| --- | --- |
| Home/Modules, clientes físicos 1280×720 e 800×600 | **48/48** cliques reconhecidos pelo estado de cores da barra, seis tentativas por intervalo nominal e geometria. Não abrange todos os controles ou hardware. |
| Cliques nominais 20/40/80/120 ms | Modo sem captura durante DOWN/UP; a duração física não foi medida, apenas limites temporais das APIs de input sintético. |
| Resposta visual após UP, método observado separado | Medianas próximas de **40–50 ms**; a captura custou aproximadamente **16–18 ms** e interfere no ensaio. Não é latência física ou tempo de dispatch. |
| 16 mudanças 800×600 ↔ 1280×720 | Primeira observação de barra compatível: mediana **39 ms**, máximo **62 ms**, incluindo captura. Não é prova de quadro inteiro concluído. |
| Arraste real pela borda | Oito geometrias e oito amostras de cores compatíveis durante o arraste. Não conta novas apresentações/FPS; o Windows pode esticar uma imagem anterior. |
| CPU em repouso | Aproximadamente **1,4–2,1% do total** dos 12 lógicos; equivalente a cerca de 16–26% de um core. Não diagnostica gargalo em carga real. |
| Memória privada em 30 s | Aproximadamente **+6,1 MiB na 0.5** e **+6,4 MiB no controle 0.4**. Comparação sequencial exploratória com histórias de processo diferentes; não prova vazamento nem sua origem. |

A amostra de controle 0.4 teve CPU total próxima de **1,1%**. Não se atribui a diferença a um hook específico: faltava isolar `SmoothFonts` e `SystemCursor` no mesmo build, aquecimento e cenário. Working set e memória privada não são equivalentes; o working set caiu no controle mesmo com aumento de memória privada.

Os percentis de grupos pequenos devem ser interpretados com cautela: nos grupos de seis cliques e 16 resizes, o p95 nearest-rank era o máximo da amostra. As leituras de cor usam limiar, não um oráculo pixel-exato da página inteira. Os registros integrais ficam privados.

## Mínimo de janela

Com o Lake fechado, no INI ao lado da DLL:

```ini
[window]
Width=1024
Height=576
MinWidth=800
MinHeight=450
```

São dimensões **lógicas da área cliente**, não pixels físicos garantidos em qualquer DPI. `MinWidth` aceita 320–8192 e `MinHeight`, 180–8192; valores inválidos voltam aos padrões de cada eixo. O mínimo é limitado pela área útil disponível do monitor quando necessário. Uma configuração não torna o texto legível em qualquer monitor/DPI.

O mínimo é informado por `WM_GETMINMAXINFO` no modo escalado. Não há bloqueio adicional no hook de `SetWindowPos`, mas o próprio Windows pode limitar pedidos programáticos menores, como ocorreu neste ensaio. Não altera o canvas nem habilita rebuild gráfico. Os modos legado fixo/rebuild mantêm suas regras anteriores. Para restaurar o limite pequeno da 0.5, use `MinWidth=320` e `MinHeight=180`.

## Monitor de CPU e memória

`Measure-LakeResources.ps1` observa somente uma instância do Lake explicitamente indicada. Não inicia/fecha o aplicativo, não injeta código, não envia input e não altera INI, rede, presets, DPI, resolução ou timers.

Exemplo, após abrir a instância de teste pelo launcher e substituir `1234` pelo PID informado:

```powershell
.\scripts\Measure-LakeResources.ps1 -ProcessId 1234 -DurationSeconds 300 -IntervalSeconds 1 -Label default -OutputDirectory .\private\resource-tests
```

O script e seu módulo também acompanham a distribuição 0.6. O CSV incremental e o sumário JSON ficam em arquivos novos, sem sobrescrever medições existentes. Só são coletados contadores e metadados necessários à identidade: não há títulos de janelas, nomes/IPs de adaptadores, presets, credenciais ou conteúdo da memória.

O PID, imagem/hash admitido e identidade de criação devem corresponder ao alvo. Se o alvo sair, a observação termina; não segue outro processo que reutilize o PID. Limites de duração/memória encerram **o monitor**, nunca o Lake. Falhas de acesso não são tratadas como uso zero de recursos.

CPU percentual de um core e percentual do total têm denominadores diferentes. Crescimento de memória privada, working set, handles e objetos GDI/USER devem ser avaliados separadamente. Uma subida curta ou contagens estáveis não demonstram ausência/presença de leak; não extrapolar linearmente por horas.

### Comparação controlada das opções

Foram feitas quatro execuções **sequenciais** da mesma 0.6 em cópias privadas, sem alterar INI de processo aberto, e um controle novo da 0.5 preservada. Mesma preparação Home → Modules → Home, OFFLINE vazio, canvas 1088×612, cliente físico 1280×720/DPI 125%, mouse no canvas; **15 s adicionais quietos + 30 s de observação**, 30 amostras/29 intervalos CPU válidos por caso. Todos usaram o limite pequeno, sem mudar a geometria observada.

| Caso | SmoothFonts | SystemCursor | CPU média, total dos 12 lógicos | Delta privado |
| --- | --- | --- | --- | --- |
| Controle 0.5 | 1 | 1 | **1,48%** | **+3,95 MiB** |
| 0.6 padrão | 1 | 1 | **1,47%** | **+5,98 MiB** |
| 0.6 só fontes | 1 | 0 | **1,41%** | **+5,01 MiB** |
| 0.6 só cursor | 0 | 1 | **1,41%** | **+5,75 MiB** |
| 0.6 sem as duas opções | 0 | 0 | **1,31%** | **+4,08 MiB** |

CPU calculada pelo delta cumulativo dividido pelo intervalo monotônico realmente observado, não pela duração pedida. GDI/USER ficaram estáveis nos quatro primeiros; no último, GDI −2 e handles −4, USER estável. Houve uma única amostra por configuração, sem aleatorização/repetições; tempo entre preparação e comandos, histórico do renderer, aquecimento de caches e carga do sistema não são idênticos. As diferenças pequenas **não demonstram ganho sustentado sobre a 0.5**, não são FPS nem isolam causalmente uma função. As reduções de chamadas estão cobertas pelos testes/inspeção do código, não por um benchmark gráfico completo.

O crescimento privado também apareceu **com ambas as opções desligadas**. Não há correção comprovada de leak, nem base para atribuir todo o crescimento às fontes/cursor. Manter as opções padrão preserva qualidade/usabilidade; a amostra não justifica desligá-las universalmente.

Para reproduzir, use cópias separadas dos arquivos do shim/INI, os mesmos originais admitidos, canvas, viewport, página, DPI e posição de mouse; primeiro OFFLINE e fora de produção. Aqueça pelo mesmo período e observe pelo mesmo tempo, preferencialmente com repetições e ordem alternada. Registre mudanças de cena/carga. O monitor não altera essas opções automaticamente.

## O que permanece fora do escopo

- Não há prova de um cap global de 50 ms. A rotina de 50 ms inspecionada era de normalização do **cursor**, não um `Sleep(50)` demonstrado no loop de renderização.
- As amostras históricas de entradas em `Blt` próximas de 52 ms não são medidas de frames efetivamente exibidos nem explicam sozinhas onde o tempo é gasto.
- Não forçar 16 bpp: houve corrupção visual sem ganho demonstrado. Não falsificar capacidades/HRESULTs, trocar vtables nativas ou pular trabalho original devolvendo sucesso.
- Não coalescer/descartar cliques, wheel ou captura para reduzir custo. Não cachear dimensões de superfície só pelo ponteiro: a fonte offscreen continua nativa e pode mudar.
- Sem validação de áudio/rede/hardware, muitos módulos, todos os controles, outros drivers/DPI/Windows 10 ou estabilidade prolongada. Um novo teste de memória não substitui análise de alocações quando o crescimento persistir.
