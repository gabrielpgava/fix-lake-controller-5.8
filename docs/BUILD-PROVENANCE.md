# Proveniencia da candidata 0.7.0

Esta candidata e **experimental e nao tem validacao real Lake OFFLINE**. Os
testes sinteticos nao homologam hardware/audio/campo nem demonstram ganho de FPS
ou correcao de vazamento. Nenhuma funcao desta biblioteca inicia o Lake, instala
no perfil ou baixa ferramentas.

## Contrato para os scripts de build

`scripts/Build-Provenance.ps1` e uma biblioteca dot-source para Windows
PowerShell 5.1. A integracao dos compiladores pertence a `Build.ps1`,
`Build-Installer.ps1` e CMake; importar esta biblioteca nao compila nem executa
testes. **Nao existe comando para carimbar retroativamente um dist antigo.**

```powershell
. "$PSScriptRoot/Build-Provenance.ps1"
$ctx = Start-LakeBuildProvenance -RepositoryRoot $root `
    -ManifestPath "$dist/BUILD-PROVENANCE.json" -Component Runtime `
    -Version '0.7.0' -Toolchain Zig -ToolchainVersion '0.14.1' `
    -CompilerPath $zig -Configuration Release `
    -DistributionProfile DevelopmentUnsigned `
    -BuildArguments $actualBuildArguments `
    -GeneratedHeader "$build/generated/lake_build_id.h"

# Compilar incluindo o header gerado; montar os seis arquivos finais em $dist.
# O build ID do header deve ser o mesmo de $ctx.Document.BuildId.
Start-LakeBuildTests -Context $ctx -ArtifactDirectory $dist
Invoke-LakeBuildTest -Context $ctx -Name scale_geometry `
    -Program "$build/LakeFixScaleGeometryTest.exe"
# Executar e registrar TODAS as demais suites retornadas por:
# Get-LakeRequiredTests Runtime
$null = Complete-LakeBuildProvenance -Context $ctx `
    -ArtifactDirectory $dist -RequireTests
```

Regras do contrato:

- `Start-LakeBuildProvenance` acontece **antes** de compilar. Captura commit,
  dirty, fingerprint do conteudo das fontes Git (inclui untracked nao ignorado),
  versao/ABI/schema de `src/product_version.h`, compilador e seu SHA-256,
  toolchain/versao, x86, configuracao, profiling, argumentos e horario UTC.
  Os caminhos do compilador/header devem ser absolutos. `BuildArguments` deve
  conter os argumentos efetivamente escolhidos pelo integrador; nao inclua
  senhas/tokens. Uma chamada bem-sucedida invalida o manifesto completo anterior,
  gravando `State=Building`. Uma interrupcao nunca deixa os testes anteriores
  aprovando silenciosamente um novo build.
- Os scripts de build acrescentam cada chamada real de compilador/CMake por
  `Add-LakeBuildCommand` antes da execução, com argv/quoting exatos e digest
  atualizado no manifesto local. Comandos não podem ser acrescentados depois do
  snapshot de testes. Argumentos/caminhos locais não entram no manifesto público.
- `GeneratedHeader` e opcional na API, mas obrigatorio na integracao do produto.
  Define `LAKE_FIX_BUILD_ID` como os 32 hexadecimais do build. O fallback
  `development-untracked` e somente para compilacao manual, nao distribuivel.
  Grave o header em intermediarios ignorados pelo Git e force sua inclusao em
  cada componente proprio; nao altere `src/product_version.h` para cada build.
- Monte/copie os arquivos finais **antes** de `Start-LakeBuildTests`. Ela congela
  seus hashes. Nao modifique DLL, EXE, INI, LICENSE ou suporte depois disso.
  Os testes devem exercitar esses mesmos binarios (ou copias de bytes identicos).
- `Invoke-LakeBuildTest` usa subprocesso, stdout/stderr separados e exit code
  real. Stderr informativo com exit 0 nao e falha. Scripts `.ps1` usam
  `powershell.exe -NoProfile -NonInteractive -File`, sem mudar politica do sistema.
  Logs ficam em `.build-evidence/<BuildId>/` ao lado do manifesto; nao sao assets.
  Para CTest, execute cada nome com `-R ^nome$ --no-tests=error`; nao use uma
  invocacao que pode passar com zero testes.
- Alternativa para runners existentes: depois de executar a suite, use
  `Add-LakeBuildTestResult -Context $ctx -Name nome -ExitCode $code
  -EvidencePath $log`. O runner deve capturar o exit code real e criar o log
  **apos** `Start-LakeBuildTests`, mesmo quando nao houver stdout. Nao reutilize
  logs, nao registre uma suite que nao foi executada e nao invente exit 0.
- `Complete-LakeBuildProvenance` revalida as fontes, compilador, defaults do
  runtime, arquivos congelados e logs. Exige cada suite conhecida exatamente uma
  vez se `-RequireTests`. Sem esse switch pode registrar build SkipTests, mas
  payload, package, candidate e publication recusam evidencia incompleta/falha.
  Nao reutilize o contexto apos completar: cada build recebe um ID novo.
- O conjunto comum de runtime inclui as suites historicas e `startup_policy`,
  `session_configuration`, `component_contract`, `diagnostics_queue`, `monitor_lifecycle_policy`,
   `window_state_snapshots`, `own_link_symbols`. `Get-LakeRequiredTests Runtime` e o contrato de nomes.
  Graphics/desktop real e opcional, nao um gate comum sem desktop; nao e alegado
   como executado por este manifesto. Setup exige `installer_sandbox` e
   `installer_payload_integrity` (`--verify-payload` na imagem final, sem instalação/GUI).
- Fontes mudando durante build/testes, submodulos, caminhos de fontes nao
  suportados e reparse points falham fechados. O fingerprint inclui nomes e
  conteudo; apenas manter a mesma flag dirty nao contorna a verificacao. Saidas
  precisam estar ignoradas pelo Git para nao se tornarem entradas acidentais.

### Setup: congelar o payload ANTES de compilar

```powershell
& "$PSScriptRoot/New-InstallerPayload.ps1" -RuntimeDirectory $dist `
    -OutputHeader "$build/installer_payload.h" -AllowUnsignedDevelopment
# Gera tambem installer_payload.h.manifest.json.
$setupCtx = Start-LakeBuildProvenance -RepositoryRoot $root `
    -ManifestPath "$setupDist/BUILD-PROVENANCE.json" -Component Setup `
    -Version '0.7.0' -Toolchain Zig -ToolchainVersion '0.14.1' `
    -CompilerPath $zig -DistributionProfile DevelopmentUnsigned `
    -RuntimeManifestPath "$dist/BUILD-PROVENANCE.json" `
    -PayloadManifestPath "$build/installer_payload.h.manifest.json" `
    -InstallerFileName 'LakeWindowedFix-Setup-v0.7.0.exe' `
    -GeneratedHeader "$build/generated/setup_build_id.h" `
    -BuildArguments $actualSetupArguments
# Compilar o setup com ESSE payload e build ID; copiar EXE final para $setupDist.
Start-LakeBuildTests $setupCtx $setupDist
Invoke-LakeBuildTest $setupCtx installer_sandbox "$build/LakeFixSetupTest.exe"
$null = Complete-LakeBuildProvenance $setupCtx $setupDist -RequireTests
```

`RuntimeManifestPath` e obrigatorio no begin de Setup. O payload ja deve existir;
`PayloadManifestPath` tambem e obrigatorio no begin. `PayloadHeaderPath` pode ser
explicitado; por padrao remove-se `.manifest.json` do caminho do manifesto.
Finalize usa os caminhos congelados no contexto, ou aceita os mesmos argumentos
explicitamente. Alterar/regerar payload/header ou o manifesto do runtime durante
a compilacao faz a finalizacao falhar.

`New-InstallerPayload.ps1` aceita caminhos alternativos de config/suporte/LICENSE
para a integracao CMake, mas seus bytes precisam coincidir com os seis arquivos
testados. Nao mistura defaults atuais com runtime antigo. Header contem o ID e
hash do manifesto do runtime; o manifesto do payload registra os seis hashes e
o SHA-256 do header. Proveniencia do setup registra o hash do EXE final, o hash
do manifesto original do runtime e o manifesto/hash do payload congelado.

## Empacotamento e verificacoes separadas

```powershell
./scripts/New-ReleasePackage.ps1 -RuntimeDirectory $dist `
    -InstallerDirectory $setupDist -AllowUnsignedDevelopment
./scripts/Test-Candidate.ps1 -PackageDirectory out/releases/v0.7.0 `
    -AllowUnsignedDevelopment
```

`Version` pode ser omitida: vem da proveniencia da compilacao, nao de HEAD nem
do CMake atual. Os dois diretorios devem conter `BUILD-PROVENANCE.json`
finalizados, compativeis e testados. Setup e runtime devem ter a mesma versao,
ABI/schema, fingerprint/commit/dirty, x86, Release sem profiling e perfil de
distribuicao. Seu payload deve ser exatamente o runtime do ZIP. Nao leia o HEAD
atual para "corrigir" um dist antigo; reconstrua quando nao houver proveniencia.

O ZIP tem **exatamente** estes seis nomes, com SHA-256 conferido tambem dentro
do ZIP: `LakeWindowedLauncher.exe`, `LakeWindowedFix.dll`, `LakeWindowedFix.ini`,
`LICENSE`, `Measure-LakeResources.ps1`, `LakeResourceMetrics.psm1`. Nao sao feitas
copias recursivas de dist. `Contour.dll`, fixtures, ferramentas, logs e conteudo
proprietario nunca entram por descoberta de diretorios.

O diretorio promovido tem **exatamente quatro assets**: ZIP, Setup EXE com
versao completa, `SHA256SUMS.txt` e `RELEASE-MANIFEST.json` schema 2. O manifesto
publico contem resumos estritamente permitidos da proveniencia, incluindo hashes
dos manifestos locais, testes e payload. **Argumentos e caminhos/logs locais nao
sao publicados**: somente seu digest. Campos desconhecidos, notas arbitrarias,
JSON nao canonico/duplicado, arquivos adicionais ou reparse points sao recusados.
Schema 1 antigo nao e convertido automaticamente nem sobrescrito.

- `Test-Candidate.ps1`: verifica localmente arquivos, ZIP, checksums, proveniencia,
  vinculos e testes. Aceita candidato dirty com opt-in unsigned; nao chama Git,
  GitHub CLI, remotos ou tags, nao instala nem publica. E o alvo para o integrador
  ligar a `make release-check`.
- `Publish-Release.ps1 -DryRun`: preflight **de publicacao**. Alem das mesmas
  validacoes, exige fontes limpas, pacote clean e
  `package.SourceCommit == HEAD == origin/main` (consulta remota somente leitura).
  Mantem guardas de URLs fetch/push e tags conflitantes. Nao invoca gh nem cria
  tags/assets. Nao substitui a verificacao local de candidato.
- Publicar de verdade continua opt-in separado: sem force/update/delete, sem
  substituir release existente, GitHub fixado em `github.com`, prerelease
  experimental, `--latest=false`. Unsigned exige `-AllowUnsignedDevelopment`
  tambem aqui; nao ha alegacao de assinatura.

## Promocao transacional e recuperacao

O pacote e montado/verificado num diretorio staging **irmao** do destino,
portanto no mesmo volume. Um lock exclusivo impede duas promocoes cooperativas.
Antes de tocar no destino, todos os assets existentes precisam ser um pacote
reconhecido e intacto, sem arquivos desconhecidos. Depois:

1. Gravar journal `.nome.transaction-<id>.json` e lock `.nome.release-lock`.
2. Renomear o diretorio antigo inteiro para `.nome.previous-<id>`.
3. Renomear o staging inteiro para o destino, registrar `Committed`.

Falha controlada restaura o diretorio antigo inteiro (`RolledBack`); nao existe
substituicao sequencial de assets. Backup/journal sao preservados, nunca limpos
recursivamente. A troca nao promete caminho continuamente disponivel: entre as
duas renomeacoes o destino pode estar ausente, mas nunca contem metade de cada
release.

Interrupcao abrupta pode deixar lock/journal e diretorios. A proxima execucao
recusa o lock; nao tenta adivinhar qual copia apagar. Para recuperar, com nenhum
empacotador ativo, leia os caminhos do journal, valide cada copia por
`Test-Candidate.ps1`, preserve tudo que nao reconhecer e restaure por renomeacao
do backup verificado se o destino estiver ausente. Depois de resolver o estado,
remova **apenas** o lock exato revisado. `RecoveryRequired` exige revisao manual;
nao use limpeza recursiva ou rename sobre arquivos desconhecidos. Journal pode
estar um passo atras apos encerramento abrupto: confira os diretorios reais.

## Assinatura: gate pendente, nao uma assinatura ficticia

Símbolos próprios são separados/preservados **antes** de congelar hashes/testes e
do payload: [SYMBOLS.md](SYMBOLS.md). Os PDBs são do mesmo link otimizado, ligados
por GUID/age e hashes; não se recompila em Debug para tentar recuperá-los depois.
Os recibos privados e os símbolos não são assets do pacote do operador.

Nesta implementacao somente `DevelopmentUnsigned` e suportado.
`DistributionProfile Signed` e recusado, mesmo com opt-in unsigned. **Assinatura
automatica, selecao/pinning de certificado, timestamp e verificacao de trust
ainda nao foram integrados nem testados com certificado real.** Hashes e esta
proveniencia sao verificacoes de integridade/atestacao pelo runner, nao prova
criptografica de autoria nem substituto de Authenticode/CI confiavel.

Foi consultada a referencia oficial
[SignTool (Microsoft)](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool).
Para futura integracao com certificado legitimamente provisionado pelo operador:

```text
signtool sign /sha1 <thumbprint-aprovado> /fd SHA256 /tr <URL-RFC3161-aprovada> /td SHA256 arquivo.exe
signtool verify /pa /all /tw arquivo.exe
```

Sao exemplos de **pipeline futuro**, nao comandos executados por estes scripts.
Use SignTool existente do Windows SDK, nao baixe ferramentas automaticamente,
nao grave PFX/senha/token/argumentos secretos no repositorio, manifesto ou logs.
Verificacao deve exigir exit **0**, rejeitando inclusive warnings (exit 2),
conferir o signer aprovado e timestamp/politica alvo; a referencia recomenda
`/o <versao-do-SO>` conforme o alvo. O pinning e as politicas precisam ser
implementados antes de habilitar o perfil Signed. Nao use catalogos instalados,
certificados inventados, bypass de trust ou alteracao da politica do Windows.

A assinatura modifica os bytes: assine runtime **antes** do snapshot de testes
e do payload; assine setup antes de seu snapshot/finalize. Revalide o binario
assinado e registre seus hashes finais. Assinar depois do finalize faz os gates
atuais falharem, como esperado. O fluxo Signed permanece bloqueado ate existir
implementacao e evidencia reais dessa cadeia.

## Testes sem rede/instalacao

```powershell
./tests/Provenance.Test.ps1
./tests/Release-Package.Test.ps1
# Ou reaproveitar somente como entrada de leitura um pacote real ja valido:
./tests/Release-Package.Test.ps1 -PackageDirectory out/releases/v0.7.0
```

Fixtures sinteticas ficam somente em subdiretorios GUID de `private/`, com PE
minimos deliberadamente nao executaveis. Nao sao releases reais nem sao
executadas/carregadas. Git e gh sao mocks que falham para qualquer comando nao
esperado. Os guards anteriores de publicacao continuam cobertos, mais manifesto
ausente, evidencia faltante/incompativel, bytes alterados, mistura de payload,
privacidade do manifesto, candidato sem remoto e injecao de falha de rename para
exercitar rollback/journal. Nao se cria tag/release remota nem instalacao no perfil.
