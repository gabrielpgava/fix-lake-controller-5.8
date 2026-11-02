# Símbolos exatos dos PEs próprios (Zig)

`scripts/Split-OwnSymbols.ps1` é uma biblioteca PowerShell 5.1: dot-source só
define funções. Não compila, baixa ferramentas, consulta Git/HEAD, recalcula
proveniência ou executa/carrega os PEs fornecidos. Nenhum Build/CMake/provenance
existente é alterado por este módulo.

## Integração dos builds

`Build.ps1`, `Build-Installer.ps1` e `Build-GraphicsCheck.ps1` usam
`Preserve-OwnSymbols.ps1` **antes** de testes/hash final/payload. Zig/MinGW separam
DWARF com LLVM objcopy; MSVC mantém o PE otimizado PDB-only byte a byte, sem fingir
que possui DWARF para remover. A opção `-ObjcopyPath` seleciona a ferramenta local;
os builds não baixam ferramentas. Zig continua fixado em 0.14.1.

Cada candidato tem diretório novo (`runtime-<BuildId>`, `setup-<BuildId>` ou
`graphics-<BuildId>`). Somente os arquivos finais testados são copiados para dist.
O teste ABI via CMake recebe `LAKE_TEST_PRODUCTION_DLL` apontando para a DLL final,
não o alvo unstripped. As fixtures sintéticas continuam separadas da produção.

Para Zig/MSVC, a ausência de PDB adjacente ao link é erro. O integrador lê o
CodeView **RSDS** e o stream Info **MSF 7.00** do PDB, compara **GUID e age**, copia
o PDB exato sob lock de leitura e registra SHA-256/tamanho no recibo
`<PE>.link-symbols.json`. Não usa o caminho privado embutido para procurar outro
PDB, não relinka e não busca um PDB de build posterior. O store recebe também a
proveniência concluída do mesmo build. MinGW sem CodeView usa só DWARF.

A suíte obrigatória `own_link_symbols` confere os dois componentes do runtime.
O setup e o diagnóstico são revalidados antes da conclusão. Testes mutantes:

```powershell
.\tests\Link-Symbols.Test.ps1 -SymbolDirectory <build>\symbols\<BuildId> `
  -BinaryDirectory <dist-ou-candidato> -BuildId <BuildId>
# -ValidateOnly confere os recibos sem criar as cópias mutantes.
```

Os testes não carregam/executam os PEs e não comprovam debug fonte em um debugger.
MSVC local permanece não executado; CI preparado não é resultado remoto.
PDBs, unstripped e sidecars ficam fora do ZIP/setup do operador. Por padrão o CI
não publica símbolos: a retenção por 7 dias exige opt-in explícito em dispatch;
símbolos podem conter caminhos privados, revisar antes de habilitar esse upload.

**Stripping não é anonimização:** os bytes das seções runtime, inclusive CodeView
em `.rdata` quando presente, são preservados. A imagem final ainda pode conter o
caminho de PDB do link. Use diretório de build controlado para distribuir; a
remoção de referências CodeView/paths não foi implementada nem alegada. O manifesto
público omite argumentos e caminhos locais, mas isso não promete removê-los do PE.

## API DWARF para o integrador

```powershell
. .\scripts\Split-OwnSymbols.ps1

# O mesmo GUID já emitido para a compilação, sem criar/recalcular outro ID.
$id = $context.Document.BuildId
$receipt = Split-LakeOwnSymbols `
    -InputFile "$build\LakeWindowedFix.dll" `
    -OutputFile "$newCandidate\LakeWindowedFix.dll" `
    -SymbolDirectory "$build\symbols\$id" `
    -BuildId $id `
    -ObjcopyPath auto
```

Executar **depois do link otimizado com informações de debug e antes de congelar
hashes, iniciar testes, gerar payload/proveniência final ou assinar**. Testar e
empacotar o `OutputFile` stripped exato; não copiar novamente o unstripped sobre
ele após os testes. A função não altera manifests nem torna um artefato antigo
distribuível. Ajustar o encadeamento do build é tarefa do integrador.

Parâmetros:

- `InputFile`: um PE32 x86 próprio, não assinado, já linkado, com seção DWARF
  embutida e literal runtime do `BuildId` exato. O arquivo original nunca muda.
- `OutputFile`: caminho **novo**, diferente da entrada e fora do symbol store.
  Não é uma operação in-place. Arquivo existente, inclusive hardlink, é recusado.
- `SymbolDirectory`: diretório local terminando exatamente no texto de `BuildId`.
  Pode já conter outros componentes desse build, mas nenhum dos três nomes
  reservados para esta entrada pode existir. Não se sobrescreve um split anterior.
- `BuildId`: GUID não vazio em formato N (32 hex) ou D (com hífens), preservado
  literalmente, inclusive caixa/formato. Não é normalizado nem substituído pelo
  ID do HEAD atual. Precisa corresponder a um literal NUL-terminated ASCII ou
  UTF-16LE em seção runtime legível, não somente DWARF, COFF ou overlay. Um
  literal `<BuildId>-fixture` não vale como o build de produção.
- `ObjcopyPath`: caminho explícito para um executável LLVM objcopy local, ou
  `auto` (padrão). Auto tenta, nessa ordem: `llvm-objcopy[.exe]` no PATH;
  `%ProgramFiles%\LLVM\bin\llvm-objcopy.exe`; e
  `build-tools\llvm-symbols-20261020\llvm-objcopy.exe` neste repositório. Candidatos
  indisponíveis/incompatíveis são ignorados em auto; explícito falha sem fallback.
  Não executa wrappers `.ps1/.cmd`, não usa Zig objcopy e não faz downloads.

Whitelist case-sensitive do basename de entrada:

```text
LakeWindowedFix.dll
LakeWindowedLauncher.exe
LakeFixGraphicsCheck.exe
LakeWindowedFix-Setup-v<major>.<minor>[.<patch>].exe
```

Não admite `Contour.dll`, Lake Controller, gráficos proprietários, fixtures/test
DLLs ou outros nomes. Nome + GUID runtime são uma barreira contra uso acidental
em arquivo estrangeiro, **não** autenticação contra alguém forjando ambos.
O script não lê imagens proprietárias para decidir isso: o nome é recusado antes.

UNC/device paths, ADS, drives de rede e reparse points nos caminhos são recusados.
Trabalhar em árvore de build local confiável, sem escritores concorrentes nos
diretórios. As verificações de caminho não eliminam todo TOCTOU contra um agente
malicioso trocando diretórios; isto não é um sandbox de segurança para ferramentas
ou filesystem hostis. Usar paths curtos: Windows PowerShell 5.1/.NET e algumas
ferramentas ainda impõem MAX_PATH; nomes internos do stage são deliberadamente
curtos, mas a biblioteca não habilita suporte global a caminhos longos.

## Saídas e validação

Para `LakeWindowedFix.dll`, o store recebe:

```text
symbols/<BuildId>/LakeWindowedFix.dll               # unstripped, SHA exato da entrada
symbols/<BuildId>/LakeWindowedFix.dll.debug         # --only-keep-debug
symbols/<BuildId>/LakeWindowedFix.dll.symbols.json   # recibo, somente nomes e hashes
```

O resultado da função é o mesmo objeto gravado no JSON, não um resumo contendo
paths privados. Campos do schema 1:

| Campos | Significado |
| --- | --- |
| `Schema`, `Kind` | `1`, `LakeOwnDwarfSymbols` |
| `BuildId`, `FileName`, `OutputName`, `DebugName` | GUID exato e basenames, sem caminhos absolutos |
| `PreSha256`, `PostSha256`, `DebugSha256` | SHA-256 da imagem completa original, stripped e sidecar |
| `PreBytes`, `PostBytes`, `DebugBytes` | Tamanhos exatos desses arquivos |
| `Machine`, `ImageBase`, `EntryRva` | Identidade/layout PE preservados |
| `CodeSha256`, `CodeSections` | Identidade do código/layout e hashes de cada seção executável |
| `ToolName`, `ToolVersion`, `ToolSha256` | `llvm-objcopy`, versão reportada e SHA-256 do executável utilizado |

`CodeSections` guarda nome resolvido, RVA, VirtualSize, RawSize, Characteristics
e SHA-256 dos **bytes raw completos**, incluindo padding, de cada seção executável
na ordem original. `CodeSha256` é SHA-256 do UTF-8 sem BOM da representação
`ConvertTo-Json -InputObject @($CodeSections) -Depth 4 -Compress`. Não é apenas um
hash de bytes concatenados: também detecta mudança de RVA/layout.

Antes de publicar, a biblioteca:

1. Mantém a entrada e o executável objcopy abertos com compartilhamento somente
   de leitura para impedir escrita/delete nesses arquivos durante o split.
2. Copia a imagem completa para stage próprio, com `FileMode.CreateNew`, e
   confirma seu SHA. Os destinos temporários debug/stripped/JSON também são
   reservados com CREATE_NEW, nunca reutilizados de outro processamento.
3. Executa `--only-keep-debug` e `--strip-debug` nessa cópia, não na entrada.
   Captura os dois streams sem pipe deadlock e limita cada subprocesso a 120 s.
4. Faz parsing DOS/COFF/PE32 com `BinaryReader`, checa limites/overlaps e resolve
   nomes `/nnn` pela string table COFF (por exemplo `/4` → `.debug_frame`). Não
   confunde um nome raw curto com a ausência de `.debug_*`.
5. Exige machine, imagebase, entry RVA, RVAs/layout das seções runtime e todos
   os seus bytes inalterados. Isso inclui `.rdata`/vtables, `.data`, relocations,
   TLS, `.eh_fram`/unwind e exceções, não só `.text`. Confere também diretórios
   runtime, flags de loader, alinhamentos e reservas/commits de stack/heap.
6. Exige sidecar com as mesmas seções DWARF, RVAs, tamanhos e bytes. A imagem
   stripped precisa ser menor e não possuir `.debug*`/`.zdebug*` reconhecidas.
   Não aceita seção executável disfarçada de debug. SizeOfImage, raw offsets,
   checksum, flags COFF de debug/símbolos removidos e diretório debug podem mudar
   por causa da remoção; seções runtime e seus bytes, não.
7. Reconfere hashes de entrada/cópia/tool e promove por renames sem overwrite:
   primeiro original/debug/JSON no store, **por último** o `OutputFile` validado.

Não injeta `.gnu_debuglink`, não rebaseia, não faz patch de código/vtable nem
relinka/recompila em modo Debug. O original guardado é a imagem **otimizada exata**
daquela compilação, não uma tentativa posterior de reproduzi-la.

As quatro publicações não formam uma transação crash-atomic. Interrupção/erro
pode deixar stage `.lake-symbols-<guid>` ou parte do store; a função falha e
preserva esses arquivos para revisão. Não remove árvores, não apaga arquivos
desconhecidos e não substitui destinos surgidos durante a operação. Repetir usa
destinos novos; jamais "consertar" automaticamente um recibo/arquivo existente.

Revalidação posterior, sem executar objcopy nem a imagem:

```powershell
Test-LakeOwnSymbolRecord `
    -RecordFile "$build\symbols\$id\LakeWindowedFix.dll.symbols.json" `
    -OutputFile "$newCandidate\LakeWindowedFix.dll" -BuildId $id
```

Confere hashes/tamanhos, GUID, layout/código/runtime, DWARF e campos do recibo.
Detecta alteração de código, `.rdata`, entry, imagebase, RVA, debug ou original,
mesmo se alguém atualizar somente o hash do stripped/debug no JSON. O recibo não
é assinado: quem substituir todos os arquivos e reescrever todo o recibo pode
forjar um conjunto. `ToolSha256` é atestado no split; revalidação não autentica
uma nova instalação da ferramenta nem verifica todas as DLLs dependentes dela.

## Ferramenta local e limites dos símbolos

Ferramenta observada: LLVM-mingw **20261020**, `llvm-objcopy 23.1.4`.
Não muda o compilador: continua **Zig 0.14.1**. O arquivo previamente obtido e
verificado pelo integrador tem SHA-256 de archive:

```text
fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd
```

SHA-256 medido do `llvm-objcopy.exe` local, distinto do hash do archive:

```text
a00a9c7331c76532eb421f0bc84b2dfdbd3033ea619f4a7ce5ab16d71fc36765
```

A biblioteca não baixa nem revalida o archive via GitHub; calcula e registra o
hash da ferramenta efetivamente selecionada. PATH/ProgramFiles podem fornecer
outra versão LLVM compatível, que será identificada no recibo. Para reproduzir
essa seleção, fornecer o caminho explícito já verificado.

**DWARF presente não implica DWARF completo.** Os PEs Zig existentes medidos
neste workspace continham `.debug_frame`, mas não `.debug_info`/`.debug_line`, e
tinham PDBs adjacentes/CodeView. O `.debug` preserva exatamente o que existe: não
fabrica nomes/linhas fonte. O stage também preserva a imagem unstripped inteira,
incluindo CodeView. O integrador descrito acima preserva os PDBs exatos do mesmo
link, inclusive do Zig. O splitter DWARF isolado não os converte, renomeia nem
promete que `.debug_frame` sozinho permita debug fonte. PE sem DWARF embutido é
recusado pelo splitter; MSVC/PDB usa o caminho separado, sem alteração do PE.

Symbols/unstripped/PDB podem conter nomes e caminhos privados do build. Não os
publicar automaticamente nem incluir no runtime ZIP/instalador. Usar diretórios
`build-*` ignorados pelo Git e política externa de retenção. O JSON não contém
source paths, linha de comando do compilador, HEAD, stderr nem caminhos locais;
mensagens de erro/logs locais podem contê-los e também não devem ser distribuídos.

## Testes e resultados reais

```powershell
& .\tests\Symbols.Test.ps1
# Opcional: seleção explícita, sem compilar/executar o produto:
& .\tests\Symbols.Test.ps1 -InputDirectory .\build-validation-zig `
    -BuildId 02f369e1b3bc4b27b0b3c737e95715b8 `
    -ObjcopyPath .\build-tools\llvm-symbols-20261020\llvm-objcopy.exe
```

Auto usa cópias de PEs próprios de `build-validation-zig`, ou `build-integration`
quando o primeiro não existe. O teste descobre um GUID runtime único somente
para selecionar a fixture; o splitter sempre recebe/verifica o parâmetro exato.
Todos os testes mutantes operam em `build-symbols-next/tests-<guid>` novo. Nomes
"proprietários" usados em testes são **cópias dos bytes próprios** com nomes
proibidos, nunca produtos proprietários reais. Nenhum input é executado ou
carregado, nem testes/runtime/callbacks da DLL são modificados.

Suíte completa neste workspace, Windows PowerShell 5.1, LLVM 23.1.4:
**38 cenários passaram, zero falhas**. Fixtures otimizadas Zig já existentes,
BuildId `02f369e1b3bc4b27b0b3c737e95715b8`:

| PE | Original | Stripped | `.debug` | Entry RVA |
| --- | ---: | ---: | ---: | ---: |
| LakeWindowedFix.dll | 515584 | 497664 | 19456 | 97216 |
| LakeWindowedLauncher.exe | 468480 | 449024 | 20992 | 23968 |

Hashes de código/layout preservados:

```text
DLL       e30b817a877a83c6b402511ee4470ce1bc31604e75a31ba623c2118a3b19a84f
Launcher  207201cd0b3dda66da3976efe2f8b207c58ad0d8a076d63efe58b24d585cb867
```

Cobertura: arquivo menor/sem debug, `/nnn`, paths com espaços, identidade exata, basename estrangeiro,
destinos existentes preservados, tampering de código/data/header/RVA/debug/JSON,
offsets malformados, assinatura existente, sidecar falso, mutação injetada na
saída do objcopy antes da publicação, reparse point e ausência de efeitos ao
importar. Logs locais em `build-symbols-next/results-symbols.log` e
`build-symbols-next/results-symbols-explicit.log`. Os PEs de origem permaneceram
com seus hashes iniciais. Não houve compilação, GPU, execução de produto, Lake,
hardware, instalação ou rede. Não valida comportamento do runtime stripped nem
debug fonte/PDB; isso continua pertencendo ao pipeline do integrador.
