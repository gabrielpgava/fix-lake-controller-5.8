# Confiabilidade do presenter — candidata 0.7

Escopo: `src/ddraw_scale.{h,cpp}` e COM **inteiramente sintético** em `tests/presenter_test.cpp`. Base de trabalho `ed8756b`, referência informada `5e9c4f9`. As rodadas isoladas abaixo não executaram Lake, DirectDraw/GPU real, hardware/presets ou instalação. Não aprovam perda de dispositivo real, pixels exibidos, FPS, input do aplicativo ou estabilidade universal.

## Achados e classificação

| Achado | Classificação | Evidência / decisão |
| --- | --- | --- |
| Cache de letterbox sobrevivia a `Restore` e `RestoreAllSurfaces` bem-sucedidos | **P1, defeito reproduzido em COM sintético**, compatível com contrato Microsoft | Memória restaurada não implica pixels anteriores restaurados. O próximo frame de geometria idêntica precisa preencher novamente as faixas. |
| `ReleaseDC` bem-sucedido podia conservar cache depois de escrita nas faixas pelo DC da primária | **P1 condicional, caminho sintético demonstrado** | A fixture modela escrita no DC seguida de `ReleaseDC`; não mede GDI/DirectDraw real. Invalidação conservadora após sucesso. |
| Qualquer fonte/destino podia ser considerado frame completo | **P1, admissão indevida reproduzida** | Fonte auxiliar 640×480, textura, primária, descrição sem papel, destino parcial e destino nulo eram escalados/consultavam o provider. Agora permanecem nativos. |
| Provider publicava geometria antes do HRESULT da imagem | **P1, risco de coerência confirmado por leitura; nova API testada** | Consulta somente-leitura e commit separado após sucesso. A integração em `fix.cpp` é obrigatória; ver abaixo. Não foi executada a integração no aplicativo real. |
| Capacidade esgotada da primária retornava sucesso e objeto nativo sem proxy | **P1, escape nativo/fail-open reproduzido**, não leak de memória demonstrado | A criação escalada agora retorna falha/null e libera exatamente a referência retornada. Factory DD7 dispõe de finalizador com a mesma regra. |
| Um único erro consumia todos os diagnósticos de falha posteriores | **P2, comportamento anterior verificado; substituição testada** | Contagem e recuperação por operação/HRESULT, tabela e orçamento de mensagens fixos. |
| Ausência do seed/layout era expressa como `ScaledViewport == false`, permitindo apresentar nativamente com input ainda escalado | **P1, bloqueador de revisão confirmado por leitura; contrato novo testado sinteticamente** | A admissão HRESULT separada recusa o frame antes do provider, sem Blt nativo nem commit. O integrador de `fix.cpp` deve exigir `LayoutAdmitted`, com seed antes do render. |

### Contratos Microsoft consultados antes da mudança

- [IDirectDrawSurface7::Restore](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-restore): realoca memória, **não recarrega bitmaps**; inclui anexos implícitos, não anexos explícitos independentes.
- [IDirectDraw7::RestoreAllSurfaces](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdraw7-restoreallsurfaces): efetivamente chama Restore para cada superfície criada pelo objeto DirectDraw.
- [IDirectDrawSurface7::ReleaseDC](https://learn.microsoft.com/en-us/windows/win32/api/ddraw/nf-ddraw-idirectdrawsurface7-releasedc): libera o DC obtido por GetDC e desbloqueia a superfície. Não promete preservar as faixas sobre as quais o chamador desenhou.

Os HRESULTs nativos desses métodos, seus argumentos e `LastError` continuam encaminhados intactos. Apenas sucesso invalida o cache. Restore invalida a primária correspondente; RestoreAllSurfaces incrementa uma geração do **owner**, sem percorrer objetos COM nem atingir outro owner. Uma geração de superfície/owner impede que um Blt externo ressuscite cache ou faça commit depois de invalidação reentrante durante a imagem. Mudança reentrante da cooperação invalida também um plano consultado para o owner anterior.

Chamadas feitas diretamente por aliases nativos continuam fora dessa observação: não se interceptam suas tabelas. Esse limite, assim como Flip/BltBatch/Lock e outros caminhos gráficos não adaptados, não é emulação universal de identidade ou garantia de recuperação do backend.

## Admissão de frame

Todos os requisitos devem ser satisfeitos:

1. Destino é um proxy de primária e fonte não é primária conhecida pelo registro, inclusive quando passada pelo endereço nativo.
2. Dimensões esperadas positivas foram configuradas. `GetSurfaceDesc` **real e atual** da fonte confirma exatamente largura/altura e `DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS`.
3. Fonte contém `DDSCAPS_OFFSCREENPLAIN`. Só são admitidos os qualificadores `3DDEVICE`, `SYSTEMMEMORY`, `VIDEOMEMORY`, `LOCALVIDMEM`, `NONLOCALVIDMEM`; papéis/capacidades adicionais desconhecidos e `dwCaps2/3/4` não zero ficam nativos.
4. `destination` não é nulo e tem a extensão integral esperada, calculada sem overflow. A origem pode variar. Isto reconhece a forma da cópia completa observada, não a identidade universal de cada frame do aplicativo.
5. `sourceRect == nullptr`, sem apresentação reentrante, flags limitadas a `WAIT`, `DONOTWAIT`, `DDFX`. Até um sourceRect explicitamente integral mantém o contrato anterior de encaminhamento nativo.
6. O callback opcional `ScalePresentationAdmission` aprova o pré-requisito do frame. Ele roda **depois da classificação acima e antes do provider**, fora de locks. Falha retorna exatamente seu HRESULT, invalida o cache e **não chama provider, Blt nativo ou commit**. Preserva `LastError` de entrada, pois nenhum Blt nativo ocorreu. Sucesso da admissão não substitui o HRESULT/LastError do Blt real.
7. A query fornece viewport válido do owner atual. `provider == false` permanece um bypass nativo legítimo, por exemplo quando minimizado; não deve codificar uma falha fatal de seed/layout.

Falha de classificação, papel desconhecido, Blt auxiliar/crop/keyed, fonte de outra dimensão, destino parcial/nulo e ausência de configuração não chamam admissão, não publicam geometria nem recebem escala. Flags/effects/retângulos do encaminhamento não são modificados; os argumentos de interface de proxy continuam desembrulhados. `GetCaps`, formatos, enumeração e cooperação **não são falsificados**. Offscreens/aliases continuam nativos, não ganham wrappers para D3D. Uma recusa da admissão é diferente desses bypasses: nunca vira apresentação nativa silenciosa.

## Integração obrigatória em `src/fix.cpp`

Este documento conserva a descrição do contrato e sua evidência isolada. A
integração atual em `fix.cpp` instala admissão/query/commit antes dos hooks, exige
`LayoutAdmitted`, publica a geometria somente depois do sucesso e usa o finalizador
fail-closed da factory. Rodadas integradas: [VALIDATION-07.md](VALIDATION-07.md).

### Admissão, query e commit

`ScaleViewportProvider` mantém a assinatura anterior, mas é uma **query somente-leitura**: produz retângulos, pode recusar o viewport por condições transitórias e não altera `presentationSize` ou estado de apresentação/input. Retire o `presentationSize.store(...)` de `ScaledViewport`. Não basta retornar false quando o layout não foi admitido: esse retorno preserva o encaminhamento nativo por contrato.

Callbacks adicionais:

```cpp
HRESULT AdmitScaledPresentation(HWND window) noexcept;
void CommitPresented(HWND window, LONG width, LONG height,
                     const RECT* full, const RECT* fitted) noexcept;

lakefix::ConfigureScalePresenter(ScaledViewport, PresenterLog, ClipWindow,
    PaintLetterbox, renderSize.cx, renderSize.cy, CommitPresented,
    AdmitScaledPresentation);
```

O **oitavo argumento opcional** tem tipo `using ScalePresentationAdmission = HRESULT (*)(HWND) noexcept`. `nullptr` aceita por padrão para standalone/GraphicsCheck de baixo nível; preserva compatibilidade de fonte dos chamadores anteriores, não representa admissão de startup em produção. Se um guard foi explicitamente configurado, a ausência de provider também não permite pular sua rejeição.

Na integração de produção, `AdmitScaledPresentation` deve validar o cliente/lifetime e exigir **`LayoutAdmitted` com seed aplicado antes do render/layout inicial**. Se não houver essa condição, deve registrar a falha de startup e encerrar o startup controladamente e/ou devolver HRESULT explícito de falha. Não transforme a falha em `provider=false`, nem invente sucesso, capacidades ou apresentação nativa com input escalado. O presenter não implementa seed nem encerramento de startup nesta parte independente. O guard de produção precisa ser instalado antes de expor os hooks/factory.

O commit deve validar o cliente/lifetime/estado escalado e dimensões esperadas antes de publicar `presentationSize`. A política do aplicativo deve limpar essa publicação no teardown/recriação do cliente; não tratar um HWND antigo como prova de lifetime. O presenter invoca o commit do **mesmo snapshot** da query, depois do HRESULT bem-sucedido do Blt da imagem e sem invalidação reentrante da geração. Falha de fill/imagem, query recusada, bypass e operação aninhada não fazem commit. `S_FALSE` continua sendo sucesso HRESULT, não prova de pixels exibidos/completude GPU. Sem dimensões esperadas, a API permanece compilável, mas não admite escala.

Admissão/query/commit/logger pertencem ao mesmo snapshot, inclusive se GetSurfaceDesc ou a admissão reconfigurarem callbacks. A admissão pode reentrar estatísticas/configuração ou retirar a última referência API: os pins de metadados persistem até a saída. As referências COM nativas continuam sendo responsabilidade do chamador durante reentrância; não foram inseridos AddRefs ocultos. Um Blt aninhado conserva o encaminhamento nativo sem recursão do guard.

### Factory escalada sem fail-open

Para a saída `IID_IDirectDraw7` do hook escalado, substitua o uso fail-open de `WrapDirectDraw7` por:

```cpp
return lakefix::CompleteScaledDirectDrawCreation(nativeResult, object);
```

Use somente no ramo escalado/IID correto. O helper conserva falha nativa; em sucesso de wrapping conserva inclusive o HRESULT positivo original. Se faltar memória retorna `E_OUTOFMEMORY`; capacidade de registro retorna `HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)`. Em falha, zera a saída **antes** de liberar a referência nativa. Mantém `LastError` da factory mesmo se Release reentrar callbacks. Não faça outro Release dessa saída.

`WrapDirectDraw7(void**)` de baixo nível preserva o contrato antigo: falha não altera o ponteiro nem consome a referência. **Não** é um finalizador seguro para criação escalada. A primária criada pelo proxy DD7 já segue automaticamente a política fail-closed, sem exigir tratamento adicional no hook. Criações auxiliares/desconhecidas permanecem nativas.

### Recursos e diagnósticos

- `ConfigureScalePresenterResources(ScalePresenterResources)` reduz capacidades de registro (hard bounds 64 DD7 / 256 primárias) e/ou um orçamento de futuras tentativas de alocação. `~0UL` significa orçamento ilimitado. O mesmo caminho de admissão/erro funciona em produção e testes; não há macro que transforme falha em sucesso, variável de ambiente ou bypass de segurança. Não é necessário configurar recursos no runtime normal. Objetos já vivos não são retirados.
- `GetScalePresenterDiagnostics()` retorna contagens saturadas. Até **96 pares operação/HRESULT**, sem eviction; pares excedentes entram em `untrackedFailures`, sem logs. Cada par tem **quatro mensagens no máximo por processo**, somando primeiro erro, repetição (16/256), reaparição e recuperação. A recuperação indica que a mesma operação retornou sucesso depois da falha, não que o dispositivo inteiro está saudável. `suppressedMessages` conta notificações elegíveis omitidas depois do orçamento, não cada repetição silenciosa. Reconfigurar callbacks não reinicia limites.
- As contagens são observações por operação, não causas independentes ou frames: uma rejeição de wrapping e o erro devolvido por CreateSurface podem ser registrados em operações distintas. O HRESULT na mensagem de recuperação identifica o par de falha anterior.
- Nome de operação distingue os slots DD7/Surface7, descrição da fonte, fill, imagem, clipper, wrapping e **`presentation admission`** (87 operações nomeadas; capacidade continua sendo 96 **pares**, não operações). Assim um erro não mascara todos os HRESULTs seguintes; tempestades e alternância erro/sucesso continuam limitadas.
- Nem falha auxiliar nativa nem saturação de diagnóstico tornam o presenter globalmente/sticky fatal. Cada candidato consulta seu guard; bypasses e Blts continuam devolvendo o HRESULT nativo real. A política de startup é do integrador, não do orçamento de logs.
- Consulta de estatísticas/recursos/diagnóstico preserva `LastError`. Loggers, admissão, providers, painter, commit, COM, Release de cleanup e destrutores nunca executam sob locks próprios do presenter. O snapshot único de identidade/fonte/configuração e pins de HEAD foi conservado; não foram acrescentados AddRefs nativos ocultos.

## Regressão antes da correção e resultados reais

O presenter original foi copiado para `build-presenter-next/original/` **antes das alterações**. A revisão intermediária dos testes, ainda compatível com a API original, adicionou os casos e alterou a expectativa de capacidade da primária. Compilada com o presenter original, produziu **126 verificações PASS, 10 FAIL esperados, exit 1** (o resumo final não entra nessas contagens):

- 3 invalidações ausentes: Restore, RestoreAllSurfaces, ReleaseDC;
- 6 admissões indevidas: destino parcial/nulo, dimensão auxiliar, fonte primária/textura, papel ausente;
- 1 escape nativo indevido no contrato de criação escalada: capacidade da primária entregava sucesso e saída nativa em vez de falha/null. Não foi demonstrado leak de memória no código anterior.

`git hash-object` confirmou que os snapshots de `ddraw_scale.cpp` / `.h` são os mesmos blobs de `ed8756b`: `8284355952232a4274a720d96d098fb0e5dc066c` / `c24ac002b5a5b71431fcc5f8b2a71361700b4965`.

Os testes novos de API query/commit/finalizador não compilam contra o header antigo; não são alegados como regressão executada naquele snapshot. Os casos legados de fonte-primária/proxy agora conferem **encaminhamento nativo**, mantendo pins/lifetime e self-source, em vez de exigir escala para uma fonte não admitida.

### Revisão bloqueadora: pré-requisito de layout separado do bypass de viewport

Antes de acrescentar o oitavo argumento, outra cópia isolada foi preservada em `build-presenter-next/before-admission/` e recompilada: **177 PASS, zero FAIL, exit 0**. A suíte de sete argumentos confirma que `provider=false` encaminha nativamente; ela não expressa um guard HRESULT de layout e não detectava a diferença entre ausência de seed e minimização. Os novos testes exigem o header de oito argumentos e **não foram alegados como testes executados/falhando na API anterior**. O bloqueador foi identificado por revisão do fluxo em `fix.cpp`/`SurfaceBlt`, não por execução do Lake ou uma baseline negativa inventada.

`TestPresentationAdmissionGuard` cobre rejeição sem Blt/commit/query, caller LastError intacto, bypass legítimo, HRESULT nativo positivo/negativo, operação de log distinta, 1.000 rejeições e recuperação, null guard, classificação anterior ao guard e ausência de erro global sticky por auxiliares. `TestPresentationAdmissionReentrancy` e o tuple anterior cobrem troca reentrante de admission/query/commit/logger, retirada do proxy e pins, GetSurfaceDesc reentrante e Blt aninhado. **`TestOpDiag96Budget`** exercita a tabela já cheia por operações reais: rejeição de admissão permanece falha, encaminhamento/imagem preservam seus HRESULTs nativos e 1.000 erros adicionais só incrementam contagens, sem novas entradas/logs.

Validação isolada com Zig **0.14.1**, Windows x86, C++17, `-Wall -Wextra -Wpedantic`:

| Executável próprio | Resultado |
| --- | --- |
| `build-presenter-next/PresenterOriginal.exe` (regressões intermediárias) | 126 PASS, 10 FAIL esperados; exit 1 |
| `build-presenter-next/PresenterBeforeAdmission.exe` (sete argumentos) | **177 verificações PASS, 0 FAIL, exit 0**, sem warnings |
| `build-presenter-next/LakeFixPresenterAdmissionTest.exe`, `-O2`, profiling OFF | **211 verificações PASS, 0 FAIL, exit 0**, sem warnings |
| `build-presenter-next/LakeFixPresenterAdmissionProfileTest.exe`, `-O2`, profiling ON | **211 verificações PASS, 0 FAIL, exit 0**, sem warnings |

Comandos efetivamente usados (sem Lake, DLL gráfica nativa ou GPU):

```powershell
# Snapshot intermediário preservado localmente com sua árvore src/tests:
.\build-tools\python\ziglang\zig.exe c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 -Wall -Wextra -Wpedantic -O2 build-presenter-next/original/tests/presenter_test.cpp build-presenter-next/original/src/ddraw_scale.cpp build-presenter-next/original/src/profile.cpp -luser32 -municode -o build-presenter-next/PresenterOriginal.exe
& .\build-presenter-next\PresenterOriginal.exe > build-presenter-next/original-regression.log

.\build-tools\python\ziglang\zig.exe c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 -Wall -Wextra -Wpedantic -O2 build-presenter-next/before-admission/tests/presenter_test.cpp build-presenter-next/before-admission/src/ddraw_scale.cpp build-presenter-next/before-admission/src/profile.cpp -luser32 -municode -o build-presenter-next/PresenterBeforeAdmission.exe
& .\build-presenter-next\PresenterBeforeAdmission.exe > build-presenter-next/presenter-before-admission.log

.\build-tools\python\ziglang\zig.exe c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 -Wall -Wextra -Wpedantic -O2 tests/presenter_test.cpp src/ddraw_scale.cpp src/profile.cpp -luser32 -municode -o build-presenter-next/LakeFixPresenterAdmissionTest.exe
& .\build-presenter-next\LakeFixPresenterAdmissionTest.exe > build-presenter-next/presenter-admission.log

.\build-tools\python\ziglang\zig.exe c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 -Wall -Wextra -Wpedantic -O2 -DLAKE_ENABLE_PROFILING=1 -fno-emulated-tls tests/presenter_test.cpp src/ddraw_scale.cpp src/profile.cpp -luser32 -municode -o build-presenter-next/LakeFixPresenterAdmissionProfileTest.exe
& .\build-presenter-next\LakeFixPresenterAdmissionProfileTest.exe > build-presenter-next/presenter-admission-profile.log
```

Os testes incluem 30/49 slots, identidade nativa, aliases, `this`, outputs/HRESULT/LastError, limites reais de registro, injeção de falta de owner/proxy/capacidade, referências independentes e cleanup reentrante; owner correto/superfícies irmãs, cache após restore/DC, invalidação durante Blt, pins de objetos retirados, query sem publicação, commit com tuple coerente, tempestades/recuperação e overflow de diagnóstico. Logs/snapshots/binários são saídas locais ignoradas pelo Git. Isto não substitui a compilação e as fixtures da integração completa, nem ensaio gráfico real explicitamente autorizado.
