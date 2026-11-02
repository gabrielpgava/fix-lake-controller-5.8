# Build e candidato com Make — 0.7.1

Comandos para desenvolvedores. O operador usa o setup, sem Make/PowerShell/compilador.
A candidata é experimental, **não assinada e sem ensaio Lake OFFLINE desta revisão**.

## Pré-requisitos Windows

- Git, GNU Make, PowerShell 5.1, Python 3 para os snapshots.
- Zig **0.14.1 completo**, local; `ZIG=C:/tools/zig/zig.exe` seleciona o executável.
- LLVM objcopy local para símbolos próprios:
  `OBJCOPY=C:/tools/LLVM/bin/llvm-objcopy.exe`, ou descoberta descrita em
  [SYMBOLS.md](SYMBOLS.md). Não substitui o compilador; builds não baixam ferramentas.
- MSVC x86/CMake/SDK são alternativa via scripts, não uma execução local já comprovada.

## Fluxo local (não publica)

```powershell
make build          # runtime x86 Release e 19 suítes
make profile        # runtime diagnóstico separado, nunca payload de setup
make installer      # runtime testado + setup fase dois, sandbox e payload
make package        # lista fechada de assets e guards mock
make release-check  # candidato permitido na branch de trabalho
```

Saídas padrão: `dist-release-v0.7.1/`, `dist-setup-v0.7.1/`,
`dist-profile-v0.7.1/`, `out/releases/v0.7.1/`.
`VERSION` vem de `src/product_version.h`; qualquer override deve coincidir.
`-SkipTests`, Debug e profiling não geram candidato distribuível.

O pacote tem exatamente quatro assets:

1. `LakeWindowedFix-Setup-v0.7.1.exe`.
2. `LakeWindowedFix-v0.7.1-win32.zip` — exatamente seis arquivos próprios MIT.
3. `SHA256SUMS.txt`.
4. `RELEASE-MANIFEST.json` — schema 2, proveniência dos builds/testes/payload.

Scripts do setup/package/candidate exigem `-AllowUnsignedDevelopment` explícito;
os alvos Make de desenvolvimento o selecionam. Não alegam assinatura nem modificam
políticas do Windows. Fontes sujas são identificadas, nunca relabeladas com HEAD
posterior. Símbolos/PDBs, fixtures, Contour, logs e ferramentas não entram nos assets.

O candidato é promovido por diretório com journal/backup; conflitos desconhecidos
são preservados. Não apague locks/diretórios recursivamente para forçar uma promoção.
Procedimento de recuperação: [BUILD-PROVENANCE.md](BUILD-PROVENANCE.md).

## Publicação é outra operação

`make publish-check` executa o preflight read-only separado. Exige fontes/pacote
limpos e `package.SourceCommit == HEAD == origin/main`, além de URLs/tags/hashes.
**Não faça merge nem envie main apenas para aprovar um candidato de trabalho.**

`make release` / `make release-draft` são operações remotas explícitas, não parte
desta entrega e não autorizadas por um candidate check. Exigem GitHub CLI autenticado
e revisão independente. Não há force-push, substituição de tag/release/assets,
credenciais de assinatura ou job de publicação no workflow. Futura assinatura
verdadeira depende do pipeline/certificado ainda pendentes.

Resultados reais e limites: [VALIDATION-07.md](VALIDATION-07.md).
