# Build e release com Make

Estes comandos sao para **desenvolvedores**. O operador baixa o instalador e usa **duplo clique > Instalar > atalho**; nao precisa de Make, compilador, GitHub CLI ou PowerShell.

## Pre-requisitos no Windows

- GNU Make (`make.exe`), Git e Windows PowerShell 5.1 ou superior.
- Zig **0.14.1 completo**, existente no PATH ou nas pastas locais `build-tools/zig-0.14.1/zig-x86_64-windows-0.14.1/` ou `build-tools/python/ziglang/`. Nada e baixado automaticamente; copias incompletas sao recusadas. Para outro caminho, use `ZIG=C:/tools/zig/zig.exe`.
- Somente para publicar: GitHub CLI (`gh.exe`) autenticado, com permissoes de push e criacao de releases no repositorio `origin`. Nao grave tokens no Makefile, no INI ou no Git.

## Comandos

```powershell
make help
make build
make test
make profile
make installer
make package
```

- `build` compila o runtime x86 e executa as suites de regressao, incluindo profiling habilitado/desabilitado; `test` e um alias para esse build testado. As oito suites/1.159 verificacoes documentadas anteriormente pertencem a baseline historica.
- `profile` compila/testa somente um runtime de diagnostico separado em `dist-profile-v0.6.0/`, com intermediarios em `build-zig-profile/`. Nao inicia o Lake, instala ou publica nada. O fluxo normal build/package deixa profiling desligado. Veja [OPTIMIZATION.md](OPTIMIZATION.md).
- `installer` tambem compila e testa o instalador nativo em pastas sinteticas, sem instalar no perfil real.
- `package` tambem gera os arquivos de distribuicao por uma lista fechada de nomes. Nunca inclui a pasta inteira de build/dist, logs, dumps, presets, ferramentas, evidencias privadas ou binarios proprietarios.
- Depois do empacotamento, executa **17 testes das guardas de publicacao**, com Git e GitHub simulados: nenhuma tag, release ou chamada remota real e feita pelos testes. Incluem arvore suja, hashes/checksums/ZIP alterados, metadados privados inesperados, URLs divergentes e tags conflitantes.

Saidas separadas das distribuicoes historicas:

```text
dist-release-v0.6.0/     runtime
dist-setup-v0.6.0/       instalador
out/releases/v0.6.0/    assets publicaveis
```

Os assets sao:

1. `LakeWindowedFix-Setup-v0.6.0.exe` — opcao simples para o operador.
2. `LakeWindowedFix-v0.6.0-win32.zip` — seis arquivos MIT, alternativa portatil.
3. `SHA256SUMS.txt`.
4. `RELEASE-MANIFEST.json` — versao, commit de origem, estado limpo/sujo e hashes dos assets.

Pacotes de desenvolvimento podem ser gerados com alteracoes locais, mas ficam marcados como `SourceDirty=true` e **nao podem ser publicados** pelo script.

## Publicar no GitHub

Depois de revisar, testar, commitar e enviar o codigo:

```powershell
git push origin main
make release-check VERSION=0.6.0
make release VERSION=0.6.0
```

`release-check` recompila/testa/empacota e verifica o commit, os hashes, o ZIP e o remoto **sem criar tag ou release**. Nao precisa de GitHub CLI.

`release` exige fontes limpas, pacote do mesmo commit e `origin/main` nesse commit. Cria a tag anotada `v0.6.0` se ainda nao existir, envia apenas essa tag e cria um **prerelease experimental**, sem marca-lo como latest. Requer GitHub CLI autenticado; `GH=C:/caminho/gh.exe` seleciona outro caminho.

Para criar um rascunho revisavel em vez de publicar imediatamente:

```powershell
make release-draft VERSION=0.6.0
```

Nao ha force-push, substituicao de tags ou sobrescrita de release/assets existentes. Se um passo de rede falhar, confira o estado no GitHub antes de repetir; uma tag ja criada e aceita somente se apontar para o mesmo commit.

O preflight verifica a URL de fetch e todas as URLs efetivas de push do `origin`; GitHub CLI fica explicitamente vinculado a `github.com`, sem seguir outro `GH_HOST`. Manifesto, assets e ZIP sao verificados antes de publicar. O quarto asset (manifesto) tambem deve ser um arquivo regular, sem link/reparse.

`VERSION` deve corresponder a versao do projeto em `CMakeLists.txt`. A 0.6 permanece experimental: simplificar a entrega nao equivale a homologar hardware, audio ou uso em campo. Leia [SETUP.md](SETUP.md) e [PERFORMANCE.md](PERFORMANCE.md).
