# Observação somente de leitura

## Janelas

```powershell
python .\scripts\Inspect-WindowState.py --pid <PID> --output .\private\window-state.json
python .\tests\window_state_test.py
```

O inspetor reconhece `LakeWindowedFix.ScaledHost.v1` (host escalado),
`ContourParent` (host legado) e `Lake Controller` (canvas). Registra HWND, thread,
pai/filhos conhecidos, visibilidade, retângulos da janela/cliente e papel.
`HostSetupCompleted` só é lido no host: no canvas o resultado é `NotApplicable`,
nunca uma falha inferida da propriedade ausente. Não coleta títulos nem textos de
controles, lê memória, envia input ou altera janelas. Desktop atual apenas;
visibilidade não comprova renderização/áudio.

Executado nesta revisão: **4 testes Python** de snapshots sintéticos, aprovados;
`python -m py_compile .\scripts\Inspect-WindowState.py` aprovado.

## Sessões longas de recursos

```powershell
.\scripts\Measure-LakeResources.ps1 -ProcessId <PID> -DurationSeconds 14400 -IntervalSeconds 1 -MaximumSamples 20000 -Label idle-comparison
.\tests\Resource-Metrics.Test.ps1
.\tests\Monitor-Policy.Test.ps1
```

Duração configurável: 10–86400 segundos (até 24 horas); `MaximumSamples` limita o
CSV a 1–1000000 linhas (default 100000). Para quatro horas a 1 Hz são cerca de
14400 linhas. Alcançar qualquer limite encerra **somente o monitor**, sem reiniciar,
parar ou suspender o alvo. Para vários segmentos, cada invocação usa nomes novos;
retenha/remova relatórios privados conscientemente. Não há arquivo sem limite por
uma única execução.

Preservados: handle original + criação para não seguir PID reciclado, relógio
monotônico real, CPU kernel+user, private bytes separado de working set,
indisponível=null distinto de zero, CSV incremental, slots perdidos e sumário de
melhor esforço. Contadores não são snapshot atômico nem prova de leak/saúde de áudio.

Executado: **22 testes aritméticos + 9 de política/falhas**, zero falhas. Inclui
identidade reciclada e saída simuladas na política usada em produção, atraso além
da duração, limite de amostras; o script real rejeitou o próprio PowerShell (hash
não admitido) e registrou zero amostras, mantendo o processo vivo. Falha de saída
preservou um arquivo alheio. Não foi feito bypass de hash nem seguido um Lake real.

**Não executado:** interrupção Ctrl+C de um monitor coletando o Lake legítimo,
saída real desse alvo e ensaio comparativo 15–30 minutos/2–4 horas. Procedimento:
em ambiente isolado sem áudio ativo, iniciar uma única instância admitida,
registrar commit/hashes/configuração/canvas/viewport/DPI/cena/aquecimento; comparar
baseline e candidata na mesma duração, ordem alternada e repetições. Encerrar o
Lake normalmente ou interromper apenas o monitor; conferir StopReason, intervalos,
slots perdidos e que nenhuma instância foi seguida/substituída. Não extrapolar o
diagnóstico gráfico próprio para desempenho do Lake.
