# Input: hit-test e captura

## Defeito demonstrado e contrato

A baseline `ed8756b` transforma qualquer ponto externo em `(-1,-1)`, mesmo
durante captura. Isso perde um eixo válido e faz movimentos/releases saltarem
para uma coordenada sem relação com o arraste. A fixture anterior exigia essa
política incorreta.

`ViewToSource` continua rejeitando novos cliques nas margens.
`ViewToSourceCaptured` prolonga a transformação afim por eixo, com floor assinado:
um arraste/release mantém destinatário e coordenadas negativas/exteriores, como
no contrato Win32 de captura. Não há clamp universal nem captura própria do shim.
O limite LONG é saturado apenas para evitar overflow. Botão DOWN externo continua
rejeitado, mesmo se outro botão mantiver captura.

Os caminhos de dispatch do main, fila/Get/Peek A/W, filhos capturados, wheel e
GetCursorPos usam a mesma decisão de captura nativa. PM_NOREMOVE não altera o
objeto original da fila. Não se mantém um estado de operação que sobreviva ao
ReleaseCapture/WM_CAPTURECHANGED nativo.

## Testes desta execução (Zig 0.14.1, x86 Release)

Regressão: recompilar `tests/fixture.cpp`, copiar apenas a DLL/launcher de teste e
Contour **sintética** de `build-baseline-ed8756b` para `build-input-before`, executar:

```powershell
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-input-before -NativeResize 0 -ScaleRendering
```

**Falha esperada** na baseline; log local `build-input-regression-before-final.log`.
Após compilar a DLL corrigida em `build-input-regression`:

```powershell
.\build-input-regression\LakeFixScaleGeometryTest.exe
.\tests\Run-Fixture.ps1 -BinaryDirectory .\build-input-regression -NativeResize 0 -ScaleRendering
```

Geometria: **129 verificações, zero falhas**. Runner escalado: **844 linhas PASS,
zero falhas** (inclui assertions do launcher/runner); cenário de input: 484
verificações, cinco cenários de política: 66 cada. Logs locais ignorados
`build-input-geometry-after.log` e `build-input-regression-after.log`.

Exercita: eixos/bordas/cantos negativos, captura main/filho, releases L/R/M/X,
modificadores, A/W, repeat-peek, dispatch único, wheel propagado, resize e veto
nativo de fechamento herdados. Não movimenta o ponteiro global. Camadas:
**matemática + Win32 sintético**, não controles proprietários/GPU/Lake.

## Deltas pequenos — hipótese não ativada

O teste reproduz 17 deltas de +1 que individualmente ficam em zero na relação
1088×612 → 1024×576, enquanto +17 agregado move +16 pixels. O getter/setter
reconhecido por RVA não prova que chamadas consecutivas pertençam à mesma operação
ou devam compartilhar resíduos. **Acumulação continua desativada.** Instrumentar
somente esse par em uma sessão legítima isolada é o próximo passo; correlacionar
captura, thread, geometria, operação e movimentos externos antes de mudar comportamento.

Sem evidência nova dos estados especializados, cursor híbrido também não é ativado.
`SystemCursor=0/1` e `SmoothFonts` existentes são preservados.

Referências consultadas: [SetCapture](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture)
e [DPI desktop](https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows).
Não há alteração de awareness/DPI apenas no host nem promessa de 1:1 físico.
