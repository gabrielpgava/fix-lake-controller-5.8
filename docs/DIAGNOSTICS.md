# Diagnóstico assíncrono de sessão

`src/session_diagnostics.h/.cpp` é um módulo independente do renderer, integrado
em `fix.cpp`, scripts e CMake. O wrapper `Log` enfileira; VEH só copia código e
endereço; o caminho de saída tenta shutdown por 250 ms. O export de flush existe
somente na DLL `-fixture`. Evidência integrada: [VALIDATION-07.md](VALIDATION-07.md).

## Integração

Compilar como C++17 para Windows x86, `_WIN32_WINNT=0x0a00`, e linkar `bcrypt`.
`product_version.h` centraliza a versão; `Options::buildId` usa por padrão
`LAKE_DIAGNOSTICS_BUILD_ID`, cujo default é `LAKE_FIX_BUILD_ID`. O ID é copiado
durante `Start`: até 63 caracteres ASCII, substituindo espaços/controles/colchetes
por `_`. A versão do produto e o build ID aparecem separadamente no prefixo.

Exemplo de adaptação, **fora de `DllMain`**, na inicialização explícita:

```cpp
namespace diagnostics = lakefix::diagnostics;

bool CollectPresenter(void*, diagnostics::PresenterStats& out) noexcept {
    const auto s = lakefix::GetScalePresenterStats();
    out = {s.directDrawObjects, s.primarySurfaces, s.liveProxyObjects, s.ownerContexts};
    // Também pode diagnostics::Write(textoLimitadoComFlagsEContadores);
    return true; // false omite somente o registro PresenterStats desta amostra.
}

diagnostics::Options options;
options.buildId = LAKE_FIX_BUILD_ID;
options.statsCallback = CollectPresenter;
// Opcional: ranges já calculados, sem inspecionar imagens dentro do VEH.
options.mainImage = {mainBase, mainBytes};
options.rendererImage = {rendererBase, rendererBytes};
const bool started = diagnostics::Start(logFile, options);
```

O chamador fornece um arquivo de disco **síncrono**, dedicado e append-only
(`FILE_APPEND_DATA`, sem `FILE_FLAG_OVERLAPPED`). Não usar pipe, console, diretório
ou handle com outro escritor. `Start` duplica o handle, não o toma emprestado:
o chamador continua dono do original e pode fechá-lo depois da chamada. Duplicatas
compartilham o file pointer; por isso, outro escritor/seek simultâneo invalida o
contrato. A implementação verifica tipo de disco/tamanho, mas não consegue
certificar todas as flags e direitos do handle fornecido.

O módulo não escolhe diretório/nome e não cria, trunca ou apaga logs. O integrador
pode preservar `LakeWindowedFix-<pid>.log`, como exigem fixtures existentes. Não
trocar o nome pelo GUID. Bytes pré-existentes contam no limite; arquivo maior que
o orçamento de dados causa `Start` falso/`ERROR_FILE_TOO_LARGE`, sem truncamento.

```cpp
void Log(const wchar_t* text, bool /*legacyNonBlocking*/ = false) noexcept {
    (void)diagnostics::Write(text); // SEM printf, conversão, WriteFile ou ODS aqui.
}

// No VEH, após validar EXCEPTION_POINTERS e selecionar os códigos desejados:
(void)diagnostics::RecordException(code, address);
// Preservar sempre EXCEPTION_CONTINUE_SEARCH; não consumir/"corrigir" a exceção.
```

O consumer preserva o texto de `Write` e os nomes `tick=`, `pid=`, `tid=`. O formato
de exceção inclui literalmente
`First-chance exception (may be handled by application)`, código decimal/hex,
endereço e `cached-image`/`rva` dos ranges opcionais. Saída UTF-8 com CRLF, sem BOM.
Textos grandes são truncados explicitamente; CR/LF fornecidos são preservados,
portanto não tratar o arquivo como um formato de auditoria resistente a log injection.

## Produtores e worker

- Uma tentativa de inicialização por processo, sem restart. Não é um logger de
  instâncias desalocáveis. Falha inicial é permanente, legível em `Read().startError`.
- Ring estático de **256** eventos POD; texto de **639** `wchar_t` mais NUL.
  Pode existir mais um evento já retirado/em I/O no consumer. Não cresce com flood.
- Produtor faz uma única `TryAcquireSRWLockExclusive`, revalida a admissão, copia
  o payload e solta o lock antes de sinalizar o evento na transição vazio→não vazio.
  Contenção/fila cheia descartam imediatamente; não há espera, spin/retry, heap,
  TLS, formatação, `OutputDebugString` ou I/O de arquivo no enqueue.
- Counters do hot path usam um RMW lock-free de 32 bits, não loop CAS saturante.
  `LastError` é restaurado em **todas** as APIs, inclusive erros e descarte.
  Texto precisa ser legível até o NUL ou 640 caracteres. Endereços de exceção
  são somente valores opacos, nunca dereferenciados. `RecordException` não cria
  um grande buffer de texto na stack nem descobre módulos/chama formatadores Win32.
- Isso é best effort, não uma garantia hard real-time nem crash-safe: texto
  inválido, lock retido por thread abortada/corrompida e stack exhaustion não são
  recuperáveis pelo logger. Não chamar para stack overflow ou depender dele
  para um crash dump. Filtragem de códigos e política do VEH pertencem ao chamador.
- `Start` cria um worker fora do loader lock, fixa sua imagem com
  `GET_MODULE_HANDLE_EX_FLAG_PIN` e obtém GUID v4 aleatório com `BCryptGenRandom`.
  Não há COM, RPC ou rede; GUID não é derivado de PID/data/nome do arquivo.
- O header registra o FILETIME real de criação do processo (`GetProcessTimes`),
  ou `unavailable` quando a consulta falha; não o confunde com início do logger.
- Cada evento captura `GetTickCount64` e TID; o consumer formata UTC ISO-8601
  a partir de uma âncora `GetSystemTimePreciseAsFileTime` + delta monotônico.
  Resolução é a do tick do Windows, não QPC. Ajustes posteriores do relógio de
  parede não alteram essa linha temporal; UTC derivada pode divergir do relógio
  de parede após correções/suspensão/longa duração.
- Worker espera um evento, com deadline de relatório de **5 s** por padrão;
  `reportIntervalMs=0` desliga relatórios periódicos e usa espera infinita em idle.
  Valores não zero são limitados a 1–60 s. Batches de 32 registros evitam que
  tráfego contínuo impeça o relatório. Nenhum polling por frame ou timer de UI.
- Callback opcional e sink rodam **sem nenhum lock de diagnóstico**. Callback
  pode consultar o presenter e reentrar `Read`/`Write`/`RecordException`; não
  haverá chamada recursiva do próprio relatório. Callback deve ser curto, não
  lançar/falhar, e evitar dependências circulares. Callback travado impede o
  worker, não bloqueia o enqueue do renderer. `Flush`/`Shutdown` no worker falham
  imediatamente para impedir self-wait. DLLs de callbacks externos NÃO são pinadas
  automaticamente: seu código/contexto precisam sobreviver ao worker.

## Orçamento e perdas

Teto rígido de **4 MiB por arquivo**, configurável para baixo até 8192 bytes.
Dois KiB ficam reservados para um único resumo final. O consumer verifica o
tamanho UTF-8 completo antes de cada escrita; ao primeiro registro que não cabe,
fecha logicamente o orçamento de dados e descarta os próximos. Não divide
registros para aproveitar espaço restante. O footer usa somente a reserva e
também respeita o teto total. Por isso pode sobrar espaço não utilizado.

Erro, short write ou contagem inválida do sink tornam I/O terminal para a sessão:
sem retries por registro, debugger spam ou backoff no renderer. Bytes parciais
efetivamente escritos contam no teto, mas o registro é contado como perdido; uma
linha parcial pode ficar no arquivo. Se o armazenamento falhou, o footer também
pode faltar. Contadores em memória continuam disponíveis.

`Read(snapshot)` é try-only: false deixa a saída inteira intacta. Campos da fila
(`enqueued`, `completed`, `queueDepth`, `highWater`, `inFlight`) são copiados juntos;
os demais são observações atômicas individuais. Após os produtores pararem e o
worker terminar, sem wrap:

```text
tentativas = enqueued + rejectedInactive + rejectedInvalid + droppedBusy + droppedFull
enqueued = completed + queueDepth + (inFlight ? 1 : 0)
completed = writtenRecords + droppedFileLimit + droppedIo
```

`truncated` é subconjunto dos admitidos, não descarte completo. Header, stats,
resumos periódicos e footer usam `metadataWritten`/`metadataDropped`, não os
contadores de payload. `bytesWritten` inclui conteúdo anterior; `writeCalls`
conta chamadas reais ao sink. `lastIoError`, `ioFailed` e `fileLimitReached`
explicam perdas. Contadores de 32 bits são módulo 2^32, com `countersWrapped`
sticky; tickets `enqueued`/`completed` são 64 bits protegidos pelo lock da fila.
O resumo final contém perdas de payload/metadados observadas antes de sua própria
escrita; enqueues rejeitados depois desse snapshot não podem aparecer nele.

**Retenção entre sessões é responsabilidade do integrador**, na inicialização
ou ferramenta externa, nunca no enqueue/VEH/renderer. Este módulo limita um
arquivo, não o tamanho total do diretório ao longo de múltiplas sessões. Não faz
scan/delete automático nem pressupõe autorização para remover logs existentes.

## Flush, encerramento e lifetime

`Flush(timeoutMs)` captura um ticket de admissão e espera progresso por evento.
True significa header tentado e registros até esse ticket **processados**, mesmo
que descartados por I/O/teto; consultar o snapshot para persistência/perdas.
False pode significar timeout, contenção de um try-lock, outro flush em andamento,
estado inadequado ou chamada pelo próprio worker. Não fecha admissão nem exige
que produtores concorrentes parem. Uma fixture pode tentar um flush bounded e
retry bounded da consulta/leitura; não ler imediatamente após `Write` como se
fosse síncrono. Exports de fixture, se desejados, devem ficar somente no target
`LAKE_FIX_TESTING`, não neste módulo.

`Shutdown(timeoutMs)` fecha admissão antes de drenar, tenta o footer, fecha somente
a duplicata do arquivo e espera o evento de saída. A checagem vazio+stopping é
revalidada sob lock para não abandonar a última admissão concorrente. Pode ser
chamado novamente para confirmar a saída depois de um timeout. True é a fronteira
de lifetime para liberar contexto; não é garantia de ausência de perdas.

As duas esperas limitam o timeout solicitado a **30 s**, inclusive `INFINITE`;
zero é uma sonda sem espera. Isso limita a espera pedida ao kernel, **não** o
tempo real sob suspensão/desescalonamento do processo. Não há `FlushFileBuffers`,
garantia de durabilidade em disco, cancelamento de `WriteFile` síncrono, nem forma
de matar seguramente um callback travado. Após false, worker/arquivo/contexto
podem continuar ativos indefinidamente. Não liberar dados nem descarregar código.

Não chamar Start/Flush/Shutdown em `DllMain`; não chamar Flush/Shutdown no
renderer ou VEH. `PIN` é irreversível até a saída do processo, inclusive após
shutdown bem-sucedido (e caso CreateThread falhe depois do pin). Não existe
suporte a unload. Os três handles de evento e storage estático ficam até o fim
do processo, inclusive para permitir um produtor preemptado sinalizar tardiamente.
Terminação abrupta/crash/ExitProcess podem encerrar o worker antes do footer;
`DllMain` não tenta compensar isso com espera/I/O.

## Teste isolado x86

A rodada isolada original abaixo pode ser reproduzida diretamente. Os builds
integrados atuais também executam essa suíte. Na raiz, PowerShell:

```powershell
New-Item -ItemType Directory -Force build-diagnostics-next
& .\build-tools\python\ziglang\zig.exe c++ -target x86-windows-gnu -std=c++17 -O2 `
  -Wall -Wextra -Wpedantic -Werror -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 `
  -DLAKE_DIAGNOSTICS_TESTING=1 -municode tests/diagnostics_test.cpp `
  src/session_diagnostics.cpp -lbcrypt -o build-diagnostics-next/diagnostics_test.exe
& .\build-diagnostics-next\diagnostics_test.exe
```

O executável exige x86 em compile time e executa cada sessão em um filho próprio,
com watchdog de 15 s. Somente filhos que ele criou podem ser terminados pelo
watchdog. Gates de eventos tornam flood/falha/reentrada determinísticos, sem
sleeps ou suposições de velocidade de disco. O teste de deadline periódico usa
um evento real, sem assert de tempo exato. Hooks `StartForTesting`,
`RequestReportForTesting` e `WithQueueLockForTesting` só existem quando ambos os
sources são compilados com `LAKE_DIAGNOSTICS_TESTING`; não habilitar na DLL final.

Sink injetado exercita I/O lenta, erro, short write e contagem inválida, sem tocar
arquivos operacionais. Dois cenários adicionais usam exclusivamente arquivos
próprios `diagnostics-owned-<pid>.tmp`, `CREATE_NEW` + delete-on-close, sob o
diretório do executável em `build-diagnostics-next`. Validam append real, direitos
somente `FILE_APPEND_DATA | SYNCHRONIZE`, duplicação/fechamento de handles e teto
incluindo conteúdo anterior. Não inspecionam nem substituem arquivos existentes.
O tripwire de `operator new` confirma ausência de heap C++ no flood, complementado
por auditoria do hot path; não intercepta heap Win32/CRT nem inicialização BCrypt.

### Resultados da rodada isolada inicial

Windows x86, Zig **0.14.1**, `-O2 -Wall -Wextra -Wpedantic -Werror`:

- Build do teste e compilação separada de `session_diagnostics.cpp` **sem** macro
  de teste: sucesso, sem warnings.
- **13 cenários**, **153 verificações internas**, mais 13 watchdogs: zero falhas.
  Cinco repetições completas adicionais (65 cenários isolados) também passaram.
  PE Machine confirmado como `0x014c` (x86). Logs em
  `build-diagnostics-next/results-x86.log`, `final-x86.log` e `repeat-1.log`
  a `repeat-5.log`.
- Flood: ring fixo em 256, 80.002 tentativas excedentes contabilizadas, zero
  alocações C++, produtores terminaram com consumer ainda bloqueado. Nesta
  medição inicial `GetTickCount64` registrou 0 ms para o flood e 31 ms para
  `Flush(25)`; na rodada final registrou 16 ms para ambos. Resolução insuficiente
  para inferir latência individual ou duração exata da espera. Não são benchmarks
  de tempo real nem promessa de latência máxima.
- Teto de 8192 bytes: 2 registros Unicode escritos, 30 descartados pelo teto,
  arquivo lógico de 4858 bytes incluindo header e resumo final nesta medição
  (4862 bytes na rodada anterior; PID/TID alteram o tamanho do prefixo).
- Deadline real: intervalo solicitado 1 ms limitado a 1000 ms; primeiro callback
  observado em 1015 ms (1000 ms na rodada anterior), sem chamada de renderer/frame.
- Erro, short write e contagem inválida: duas chamadas reais ao sink (header e
  primeira tentativa de payload); oito payloads concluídos como perdidos,
  nenhum retry por payload subsequente.

Esses resultados não validam Lake, GPU, hardware, hooks do integrador, export de
flush da fixture, unload de DLL, falha física de disco ou durabilidade após crash.
Nenhum recurso de Lake/hardware/instalação/rede foi usado.

A integração acrescentou a asserção do FILETIME de criação do processo:
**154 checks em 13 cenários**, mais 13 watchdogs, zero falhas nos builds normais
e de profiling. Rodadas/BuildIds em [VALIDATION-07.md](VALIDATION-07.md).
