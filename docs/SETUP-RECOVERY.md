# Setup recuperável — 0.7 (sandbox)

## Resultado e escopo

Base: `ed8756b1633649ff8a9129434326ab3bcfb47f9f`, branch `work/reliability-0.7`.
O engine e suas fixtures foram primeiro validados isoladamente em `src/setup.cpp`,
`src/setup_engine.cpp`, `src/setup_engine.h` e `tests/setup_engine_test.cpp`.
A integração posterior em scripts/CMake está descrita em
[BUILD-PROVENANCE.md](BUILD-PROVENANCE.md); as rodadas finais e commits ficam em
[VALIDATION-07.md](VALIDATION-07.md). Nunca houve instalação operacional.

Resultado isolado anterior à integração: **419 checks, zero falhas, zero warnings de compilação, 1 skip
opcional**, em **271,39 s**. Gate `--verify-payload` no EXE compilado: **exit 0**.
A primeira etapa tinha 169 checks; a segunda acrescenta promoção e rollback
transacionais. Symlink foi pulado por erro 1314, sem pedir elevação; skip não é
contado como PASS. O teste de hard link foi executado sem elevação.
O Lake não foi iniciado, fechado ou encerrado pelo teste. Nenhuma GUI foi aberta.
EXE/DLL das fixtures são texto inerte, nunca executado/carregado.

### Defeitos antes/depois

O snapshot anterior foi copiado antes das edições para
`build-setup-next/baseline/`. Sua suite histórica passa **58 checks**, mas exigia
como resultado esperado apagar o manifesto mesmo preservando o INI. Um probe
adicional compilado por stdin contra **esse engine e seu header antigos**
reproduziu três defeitos, em sandboxes separadas:

| Caso | Antes, observado no probe | Depois, coberto pela suite |
| --- | --- | --- |
| Instalar, editar INI, gerar log, remover, reinstalar | Remove com sucesso; INI/log ficam, manifesto some; reinstalação falha | Recibo permanece; reinstalação recria ausências e mantém INI/log/dados aninhados byte a byte |
| Um atalho próprio ausente | Remoção e instalação falham | Remoção ignora ausência; reparo recria os bytes exatos |
| Uma DLL própria ausente | Remoção e instalação falham | Todas as seis entradas do payload, manager e dois links têm casos individuais de remoção/reparo |
| Falha após uma exclusão | Inspeção do código: próximas tentativas exigiam todos os arquivos; manager podia desaparecer cedo | Dez checkpoints de remoção, todos retomados tanto por remoção quanto por reinstalação direta |
| Arquivo/link alterado | Bloqueava toda remoção | Alterado é preservado; demais objetos reconhecidos são removidos. Reparo recusa sobrescrevê-lo antes de escrever |
| Manifesto anterior e payload novo | Round-trip exigia hashes da compilação nova | Remoção usa hashes do próprio manifesto 0.6; atualização exige opção explícita e journal transacional |

O probe tem **3 checks/0 falhas**: nesse probe, PASS significa que o defeito
histórico foi reproduzido, não que a baseline estava correta. Logs locais:
`build-setup-next/baseline/test-output.txt`,
`build-setup-next/baseline/defect-probe-output.txt` e
`build-setup-next/test-output.txt`. Os caminhos GUID das evidências estão nesses
logs. Nenhuma limpeza recursiva é feita, nem em falha.

## Recibo e estados

- `lake-windowed-fix.setup.ini` é agora um **recibo imutável**, não um arquivo a
  apagar no final. Schema 2 guarda versão, caminhos, SHA-256 dos seis payloads e
  do manager, e os bytes serializados dos dois atalhos. Seu parser faz round-trip
  canônico e valida também as propriedades dos templates de link.
- `lake-windowed-fix.removing.ini` é uma intenção durável, vinculada ao hash do
  recibo. É criado com `CREATE_NEW` e flush **antes da primeira exclusão**. Pode
  representar remoção parcial ou concluída; ambas são retomáveis. Uma intenção
  desconhecida/alterada não é adotada.
- Ausência não amplia propriedade: só permite ignorar na remoção ou recriar uma
  entrada da lista fechada, sob recibo válido. Pasta sem recibo continua recusada,
  mesmo vazia. Logs/arquivos desconhecidos jamais são enumerados para exclusão.
- Remoção processa payloads/atalhos, depois registro e **manager por último**.
  Manifesto e intenção nunca são apagados por `Uninstall`, mesmo sem dados do
  usuário: a pasta fica com esses dois arquivos. Repetir a remoção é inofensivo.
- Reinstalação compatível cria apenas ausências, verifica tudo e só então apaga a
  intenção. Se a confirmação falhar depois disso, a árvore completa já está
  comprometida e não é revertida para um estado sem arquivos e sem intenção.
- Alterados/desconhecidos são preservados, inclusive links com alvo, argumentos,
  diretório de trabalho ou apenas bytes diferentes. `preservedFiles` sinaliza
  remoção com objetos preservados; `recoveryPending` sinaliza falha retomável.
  Reparo com conflito retorna erro, sem afirmar que a instalação está íntegra.
- Histórico de upgrade concluído não é dependência para remover/reparar o runtime
  ativo: um backup arquivado ausente desabilita rollback, não a remoção segura
  baseada no recibo atual. Plano/marcadores pendentes ou desconhecidos continuam
  bloqueando operações concorrentes até recuperação.

Continuam em vigor: caminhos locais absolutos, proteção da pasta original,
Windows/Program Files, checagem de sobreposição/aliases, ancestrais fixados por
handles, recusa de reparse points/hard links, hashes/identidade antes da exclusão,
`CREATE_NEW`, rollback de **novas criações próprias e inalteradas** e ausência de
exclusão recursiva. Um reparse point desconhecido continua bloqueando toda a
operação, sem atravessá-lo. O Lake original pode ter sido apagado antes da
remoção; não se exige lê-lo para remover o patch.

## Migração do INI

Somente em instalação de executáveis compatíveis: compara seções/chaves sem
diferenciar maiúsculas, insere apenas as ausentes e nunca troca valores
existentes. Comentários, conteúdo desconhecido, Unicode e quebras de linha
originais são preservados. Insere dentro da seção existente, não em uma segunda
seção que o leitor Win32 poderia ignorar. A suite confirma a leitura de uma chave
adicionada com `GetPrivateProfileStringW`.

1. Cria e faz flush de `LakeWindowedFix.ini.setup-new` e
   `lake-windowed-fix.config-migration.ini` (hash do recibo, antes e depois).
2. Renomeia o INI original por handle para
   `LakeWindowedFix.ini.before-<SHA256-original>.bak`, sem substituir destino.
3. Publica o staging por handle, também sem substituir destino.
4. Apaga somente o journal verificado; **backup é mantido**.

Há um intervalo intencional sem INI no caminho original entre os dois renames.
O journal permite roll-forward, inclusive se `Uninstall` for a próxima operação.
O Lake deve permanecer fechado até recuperar. Os quatro checkpoints (preparado,
backup, publicado, journal apagado) têm teste de falha e nova invocação. Alteração
de staging/backup/INI pendente causa recusa, nunca sobrescrita. Se um backup com
esse nome já existir, uma nova migração a partir dos mesmos bytes é bloqueada,
não sobrescreve/reutiliza esse arquivo por presunção.

Suporta ASCII/UTF-8 com ou sem BOM e UTF-16LE com BOM, até 128 KiB. Codificações
ilegíveis, seções repetidas/ambíguas e conflitos são preservados com erro; não se
adivinha como migrar. INI ausente é recriado com o default do pacote. Migração
concluída é uma transação própria: se um reparo posterior falhar, suas inserções
e seu backup permanecem, enquanto apenas novas criações do reparo são revertidas.

## Atualização transacional e rollback explícitos

Use o **setup novo fora da pasta instalada**, com o Lake fechado:

```powershell
.\LakeWindowedFix-Setup-v0.7.0.exe --upgrade
.\LakeWindowedFix-Setup-v0.7.0.exe --rollback
```

Os dois comandos abrem somente a confirmação em português (Atualizar/Reverter).
Não iniciam o Lake. O fluxo comum Instalar/Reparar não atualiza binários
silenciosamente: `ERROR_REVISION_MISMATCH` / `updateBlocked` agora orienta usar
`--upgrade`. `Upgrade` descobre a pasta original pelo recibo/journal, sem depender
da detecção do caminho de instalação padrão do fabricante. `Rollback` não exige
que o Lake original ainda exista. O manager deve estar fora da instalação porque
também pode precisar trocar o EXE instalado; não se contorna o bloqueio de imagem
executável do Windows nem se encerra esse processo à força.

O módulo é isolado dentro de `src/setup_engine.cpp`, exposto pelas APIs `Upgrade`
e `Rollback`, e não muda o ABI do launcher nem o caminho persistente de dados:

1. Cria exclusivamente `.lake-windowed-upgrade` e seus arquivos com `CREATE_NEW`.
   Arquiva os bytes exatos da geração anterior (`before-N.*`), os novos
   (`after-N.*`), defaults e cópias de publicação (`next-N.*`). O índice 2 é o INI,
   com extensão `.ini.bak`; índice 10 é o manifesto. Hashes e nomes dos slots
   estão na lista fechada do engine e no `plan.ini` canônico, sem caminhos de
   exclusão fornecidos pelo journal.
2. Gera o novo manifesto schema 2 e os dois templates de atalho. Mescla o INI
   **em staging** com os novos defaults, preservando valores/linhas desconhecidos.
   Durante todo o staging, a geração antiga continua inalterada/utilizável.
   Falha antes de haver journal reverte somente as criações próprias inalteradas.
3. Faz flush do plano completo antes de iniciar a promoção. Carrega novamente e
   confere arquivos/manifestos, hashes, templates, caminhos e o resultado
   determinístico da mesclagem do INI. O conteúdo atual e todos os slots de troca
   são verificados antes da primeira alteração ativa.
4. Promove por renames **sem substituir destino**: ativo anterior vai para
   `previous-N.*`, e `next-N.*` entra no caminho estável. Processa runtime/INI/links,
   depois registro, manager e intenção de remoção, com o **manifesto por último**.
   Arquivos antigos não são apagados. Após verificar a árvore inteira grava
   `committed.ini`, vinculado ao SHA-256 do plano.
5. Se interromper entre renames, retomar `--upgrade` infere o progresso pelos
   hashes e pela presença dos slots, não por um contador mutável vulnerável a
   escrita parcial. Recria staging ausente somente a partir do arquivo arquivado
   e verificado. Conflitos são preservados e interrompem a troca.

A troca de vários arquivos **não é atomicamente visível de uma vez**. Depois da
primeira promoção, mantenha o Lake fechado até retomar ou reverter. Há intervalos
sem arquivo no caminho ativo, cobertos pelo plano/archives; não se anuncia runtime
operacional durante uma promoção parcial. Instalar/Reparar/Remover recusam esse
estado pendente, sem alterar a instalação. Nenhum arquivo é excluído por upgrade.

### Reverter

`--rollback` é permitido tanto após commit quanto diretamente após promoção
parcial. Grava primeiro `rollback.ini`, preserva os novos ativos em `undo-N.*`,
publica cópias verificadas dos `before-N.*` e restaura o manifesto anterior por
último. `reverted.ini` confirma a geração restaurada. Ausências anteriores também
são restauradas como ausências (não se inventa propriedade sobre arquivo/link
que já não existia). Nova chamada de rollback é idempotente. Após intenção de
rollback, `--upgrade` não disputa a direção da recuperação: conclua `--rollback`.

**INI atual e logs são persistentes, não uma geração executável:** rollback nunca
troca um INI existente, inclusive se foi editado depois da atualização. Se o INI
estiver ausente no intervalo de rename, recupera os bytes arquivados. As chaves
novas inseridas podem continuar no INI após voltar aos executáveis antigos; suas
edições/valores originais e o backup continuam disponíveis. Os logs e arquivos
desconhecidos não fazem parte dos slots e nunca são movidos/apagados.

### Registro e atalhos

Atalhos são arquivos com bytes arquivados e validados, tratados pelos mesmos
renames sem sobrescrita dos binários. Uma alteração posterior do link/binary
causa recusa nos dois sentidos, não é removida para forçar sucesso.

Para versão 0.6 → 0.7, prepara uma chave `.next` com os valores completos,
preserva a anterior em `.previous` via `RegRenameKey` e promove a nova ao nome
estável; rollback preserva a nova em `.upgraded` e restaura a anterior. Confere
tipos, valores e ausência de subchaves antes de cada troca e não usa exclusão
recursiva. Se o registro já estava ausente, o plano registra essa ausência.
Atualização de executáveis com a mesma versão não precisa trocar valores iguais.
Windows 10/`RegRenameKey` é o contrato existente do setup, não um recurso baixado.

Os testes de registro usam **somente** chaves novas
`HKCU\Software\LakeWindowedFix.SetupTests\{GUID}` e seus sufixos. A API recusa
outras substituições de chave e recusa injeção na chave operacional. Não emite
`SHChangeNotify` nesses testes. `RegFlushKey` não é chamado no sandbox porque
essa API força o flush do hive inteiro do usuário, não apenas da chave de teste;
a produção faz flush. Os testes comprovam recuperação de interrupção de processo,
não persistência do hive após perda de energia. As chaves GUID e arquivos de
evidência são retidos; nunca se limpa recursivamente um hive/pasta desconhecido.

Schema 1 / versão 0.6.0 é reconhecido pelos caminhos e hashes do **seu** manifesto
para remoção com o setup novo. Não exige igualdade com o payload novo. Atalhos
legados só possuem a prova semântica antiga (alvo/argumentos/working directory):
o manifesto antigo não registrava bytes, logo não é possível retroativamente
detectar toda alteração não semântica. Schema 2 usa comparação exata dos bytes.
Na promoção de schema 1 também exige descrição, ícone e show-command esperados;
depois de aceito, o snapshot do link anterior é protegido pelo hash do plano.

Os identificadores históricos de pasta `v0.6` e chave HKCU foram mantidos para
descobrir a instalação anterior. Recibo retido de instalação removida também pode
ser promovido com `--upgrade`, com ausências anteriores registradas para rollback.
Não se apaga um recibo manualmente nem se adota uma pasta sem recibo. O manager
0.6 já instalado não ganha essas APIs retroativamente: use o setup novo externo.

**Limite de histórico:** uma geração anterior por destino. O plano/archives são
imutáveis e retidos após rollback; ele é terminal para essa geração. Outro pacote
com hashes diferentes não sobrescreve esse histórico. Rotação/purge de gerações e
redo após rollback ainda não são oferecidos. Esse limite não bloqueia a primeira
atualização 0.6 → 0.7 nem sua retomada/rollback, mas precisa de evolução antes de
uma cadeia arbitrária de releases. Não há garbage collection automático.

`kVersion` usa `LAKE_FIX_VERSION_W` de `src/product_version.h`; a identidade do
Lake vem das macros de `src/lake_build_profile.h`, não de hashes duplicados.
Alinhamento de recursos PE, nomes de pacote,
CMake/scripts/README e proveniência do release é responsabilidade da integração;
o executável de compilação abaixo **não é um release**.

## Comandos efetivamente executados

PowerShell, raiz do repositório, Zig **0.14.1** existente. O objeto incorpora
`src/setup_manifest.S` / `asInvoker`, sem compilador de recursos externo:

```powershell
$zig = '.\build-tools\python\ziglang\zig.exe'
New-Item -ItemType Directory -Path build-setup-next -Force | Out-Null
& $zig cc -target x86-windows-gnu -c src/setup_manifest.S -o build-setup-next/setup_manifest.obj
& $zig c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 `
  -Wall -Wextra -Wpedantic -O2 tests/setup_engine_test.cpp src/setup_engine.cpp `
  build-setup-next/setup_manifest.obj -lbcrypt -luser32 -lgdi32 -lole32 -lshell32 -ladvapi32 -luuid `
  -municode -o build-setup-next/LakeFixSetupTest.exe
if ($LASTEXITCODE -ne 0) { throw 'test build failed' }
$env:TEMP = Join-Path $env:LOCALAPPDATA 'Temp\opencode'
$env:TMP = $env:TEMP
& '.\build-setup-next\LakeFixSetupTest.exe' 2>&1 |
  ForEach-Object { "$_" } | Set-Content -Encoding UTF8 build-setup-next/test-output.txt
if ($LASTEXITCODE -ne 0) { throw 'sandbox test failed' }
```

Baseline isolada, com as cópias anteriores às edições em `baseline/src` e
`baseline/tests`, compilada com os mesmos flags/bibliotecas e objeto de manifesto:

```powershell
& $zig c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 `
  -Wall -Wextra -Wpedantic -O2 build-setup-next/baseline/tests/setup_engine_test.cpp `
  build-setup-next/baseline/src/setup_engine.cpp build-setup-next/setup_manifest.obj `
  -lbcrypt -luser32 -lgdi32 -lole32 -lshell32 -ladvapi32 -luuid -municode `
  -o build-setup-next/baseline/LakeFixSetupTest.exe
& '.\build-setup-next\baseline\LakeFixSetupTest.exe'
& '.\build-setup-next\baseline\SetupDefectProbe.exe'
```

A GUI foi **compilada/linkada, não aberta**; o mesmo EXE foi executado somente
no gate `--verify-payload`. Ele usa os seis arquivos MIT do
runtime baseline isolado, não os binários proprietários. Para não misturar este
teste com scripts de proveniência em desenvolvimento, o gerador histórico foi
lido do commit de base e executado em memória, sem editar nenhum script:

```powershell
$generator = [scriptblock]::Create((git show ed8756b:scripts/New-InstallerPayload.ps1 | Out-String))
& $generator -RuntimeDirectory '.\dist-baseline-ed8756b' -OutputHeader '.\build-setup-next\installer_payload.h'
& $zig c++ -target x86-windows-gnu -std=c++17 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0a00 `
  -Wall -Wextra -Wpedantic -O2 -Isrc -Ibuild-setup-next src/setup.cpp src/setup_engine.cpp `
  build-setup-next/setup_manifest.obj -lbcrypt -luser32 -lgdi32 -lole32 -lshell32 -ladvapi32 -luuid `
  -municode '-Wl,--subsystem,windows' -o build-setup-next/SetupCompileOnly.exe
```

## Limites da evidência

- A chave HKCU operacional nunca é acessada nos testes; somente chaves GUID
  exclusivas, sem notificações globais. O handshake do worker não foi executado.
- Falhas são retornos determinísticos após checkpoints, sem rollback da intenção
  durável; a recuperação é uma nova chamada ao engine. A injeção não usa
  `TerminateProcess`, corte de energia ou simulação de disco/EDR defeituoso. Dados de staging/journals recebem
  flush; durabilidade de metadados de rename sob perda de energia não foi provada.
- Não promete recuperação automática de crash no primeiro install antes de existir
  recibo válido. Registros incompletos/desconhecidos e conflitos são bloqueados
  conservadoramente. Recibos locais não são assinaturas/autenticação contra um
  atacante com as mesmas permissões; escritores concorrentes no registro não
  constituem uma transação isolada do Windows Registry.
- A rodada isolada abaixo não teve MSVC/CMake. A rodada CMake integrada posterior
  está em `VALIDATION-07.md`. Não houve publicação, instalação real, hardware,
  áudio ou validação em campo. Passar nesta suite não homologa o runtime.

## Gate puro do payload embutido

```powershell
$gate = Start-Process -FilePath '.\build-setup-next\SetupCompileOnly.exe' `
  -ArgumentList '--verify-payload' -Wait -PassThru
if ($gate.ExitCode -ne 0) { throw "payload integrity failed: $($gate.ExitCode)" }
```

`--verify-payload` é decidido logo após decodificar a linha de comando, **antes**
de DPI, pastas padrão, COM, inventário de processos, detecção do Lake ou janelas.
Só verifica a whitelist exata de seis entradas e calcula SHA-256 dos buffers
`kPayloadFiles[i].data/size`, comparando com os hashes embutidos. Retorna 0 somente
quando todos conferem; CRC divergente retorna `ERROR_CRC` (23), nomes/contagem ou
hash malformado `ERROR_INVALID_DATA` (13), falha criptográfica seu código não-zero.
Não lê arquivos do Lake/instalação nem toca registro ou rede. O teste de API
injeta bytes/nomes/contagem errados; o gate do EXE final deve ser chamado pelo
`Build-Installer` para registrar evidência do próprio binário compilado. Não é
substituto para `installer_sandbox`, GPU, Lake ou hardware.

## Rodadas da promoção/rollback

Uma rodada intermediária completou **403 checks/0 falhas/1 skip opcional** em
252,08 s (`build-setup-next/upgrade-test-output.txt`, `test-duration.txt`). Ela
exercita todos os **36 checkpoints de upgrade e 14 de rollback** de arquivos,
incluindo interrupções durante staging e retorno direto após cada promoção
parcial. Com registro GUID exclusivo, o trace completo tem 39/16 checkpoints;
a injeção adicional concentra-se nas etapas específicas do registro, na intenção,
preparação e confirmação, sem repetir novamente toda a matriz de arquivos.

A primeira tentativa completa foi abortada pelo timeout de 240 s durante a
matriz que repetia flush do hive. As rodadas finais usam a política de sandbox
descrita acima: nenhum flush global e cobertura específica das trocas de chave.
Esse é um teste extenso: o timeout histórico de **60 s** de `installer_sandbox`
foi ampliado para **900 s** na integração CMake. O teste nativo isolado anterior
foi executado sem esse limite.

A rodada final usa o mesmo comando Zig, alterando a saída para
`-o build-setup-next/LakeFixSetupTestFinal.exe`, e executa esse arquivo redirecionando
para `build-setup-next/upgrade-final-output.txt`; duração em
`build-setup-next/test-final-duration.txt`. Acrescenta o caso de diretório de atalho
ausente e injeção em cada checkpoint do rollback partindo do intervalo sem INI.
Resultado: **419 checks, zero falhas, 1 skip opcional, 271,39 s**, sem contar
symlink indisponível como sucesso. O gate do EXE de compilação retornou **0**, registrado em
`build-setup-next/payload-integrity-output.txt`.
