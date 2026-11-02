# Instalador PT/EN e primeira publicação 0.7.0

## Escopo e direitos

O usuário relatou funcionamento aparentemente correto no teste local do candidato
anterior (`8949dac`, setup `6566df5d916e44f8a9b9beb2a60d5c0c`). Não informou uma
matriz de hardware/DPI/performance; esse relato não é medição independente nem
validação do instalador localizado ou do computador conectado ao processador.

Os avisos em `README.md`, `README.en.md`, `NOTICE.md`, `LICENSE`, interface e notas
do release deixam explícito: **somente patch; Lake Controller/Contour/arquivos
proprietários não acompanham a distribuição**. Requer software original legítimo e
licença quando exigida; sem afiliação/endosso do fabricante. MIT não licencia o
software nem as marcas de terceiros. O texto não promete uma garantia jurídica.
O ZIP/payload mantêm a whitelist de seis arquivos próprios; `LICENSE` inclui o
aviso bilingue, sem alterar o texto da concessão MIT.

## Implementação

- `setup_text.h`: catálogo único, IDs tipados e pares completos PT/EN para textos
  próprios da UI, resultados e nomes de papéis de objetos preservados.
- `Result::message` mantém a mensagem portuguesa anterior para compatibilidade;
  `messageEnglish`/`Message(Language)` expõem a tradução. As mesmas flags/códigos de
  erro e decisões de filesystem/registro permanecem independentes do idioma.
- `setup_options.h`: parser sem efeitos, rejeita idioma desconhecido, valor
  ausente, opções duplicadas, modos conflitantes e overflow de identidade.
- UI escolhe português se o idioma de UI do Windows for português, inglês nos
  demais casos; seletor visível **Idioma / Language** e `--lang en|pt-BR` permitem
  mudar. Não muda idioma/política global do Windows ou comportamento do Lake.
- Mensagens de sucesso/falha e aviso de direitos são atualizados ao mudar idioma.
  O texto de status tem rolagem; o aviso permanece separado e visível.
- O worker de autorremoção recebe o idioma explicitamente, mantendo as mesmas
  verificações de PID/FILETIME/token/arquivo e handshake de autorização.
- Recibos, journals, chaves, atalhos e nomes de arquivos não são traduzidos ou
  renomeados. O atalho continua **Lake Controller - Janela** nos dois idiomas.
  Botões de diálogos nativos do Windows seguem o próprio idioma do Windows.

Exemplos (somente o operador os executa, com o Lake fechado):

```powershell
.\LakeWindowedFix-Setup-v0.7.0.exe --lang en
.\LakeWindowedFix-Setup-v0.7.0.exe --upgrade --lang pt-BR
.\LakeWindowedFix-Setup-v0.7.0.exe --rollback --lang en
```

## Gate da imagem final

`--verify-localization` ocorre antes de `DefaultLocations`, busca da instalação
original ou operações do engine. Valida detecção de idioma, catálogo completo,
parsing, mensagens de payload e controles de **janelas próprias ocultas** para
Instalar/Remover/Verificar/Atualizar/Reverter. Simula troca PT→EN→PT e resultados
de erro/sucesso. Não mostra janelas, não escolhe arquivos, não instala, não cria
chaves/pastas operacionais e não inicia Lake/áudio/GPU. O gate não é uma inspeção
visual de todos os DPI/tamanhos/glyphs nem execução do worker real de remoção.

`installer_localization` passa a ser requisito do manifesto de Setup, junto de
`installer_sandbox` e `installer_payload_integrity`. Build-Installer executa os
três na candidata final stripped; CMake registra os mesmos testes quando setup ON.
Os cenários antigos de recuperação continuam exigidos; não são substituídos por
testes de texto. As fixtures fake não executam binários proprietários.

Validação local pré-integração: Zig 0.14.1 `-O2 -g -Wall -Wextra -Wpedantic -Werror`,
419 checks de localização, zero falhas; sandbox com 426 checks, zero falhas e um
skip opcional de symlink (1314, não contado como PASS). Guardas de proveniência:
30; publicação com mocks: 27, sem operações remotas. Evidências locais em
`build-i18n-probe-2`, `build-i18n-sandbox` e logs `build-i18n-*-tests.log`.

O CI anterior de `8949dac` falhou: manifesto duplicado MSVC e saída 1 no sandbox
Zig. Os artefatos antigos não incluíam os logs ocultos `.build-evidence`, portanto
a causa do sandbox não pode ser inferida daquele wrapper. CMake passa a desativar
somente a geração duplicada do manifesto MSVC nos dois executáveis que já usam
`setup.rc`; o recurso explícito `asInvoker` permanece. O CI passa a reter os logs
próprios ocultos e exibir falhas, sem alterar critérios ou pular verificações.

O novo CI de `f8b0206` confirmou o link MSVC sem manifesto duplicado e identificou
duas causas adicionais: o sandbox usava `C:\Users\RUNNER~1` em expectativas,
enquanto o engine corretamente expandia esse caminho para o nome longo; quatro
comparações de atalhos e a edição exata do caminho no recibo falhavam. A fixture
passa a expandir a raiz que criou, sem relaxar checks de atalhos/recibos, e ganha
regressão explícita com entradas 8.3 para instalar/reconhecer/remover os mesmos
objetos. O engine não é alterado por essa correção de expectativas.

O MSVC otimizado convertia o array local de BuildId em instruções imediatas,
deixando o identificador ausente das seções de dados exigidas pelo guardião de
símbolos. O identificador do setup passa a ter armazenamento estático endereçável,
lido pelo mesmo `--verify-payload`; a guarda de identidade permanece inalterada.

O candidato limpo `f8b0206` passou localmente nas 18 suítes de runtime, três de
setup e validação do pacote real; guardas: 30 de proveniência e 27 de publicação
com mocks. CMake setup ON passou 20/20. São resultados anteriores às duas correções
acima, não uma aprovação do CI falho. Evidências em `build-i18n-integrated-*`,
`build-i18n-cmake-2-*` e `build-i18n-ci-f8b0206-evidence`.

## Publicação

Primeiro release público `v0.7.0` permanece experimental e não assinado, com quatro
assets: setup, ZIP, checksums e manifesto. Não inclui símbolos, logs, fixtures,
ferramentas, presets ou arquivos proprietários. `Publish-Release.ps1` exige HEAD
limpo/testado igual a `origin/main`, mantém guardas de URL/tag e não substitui um
release existente. Notas são bilingues; links de download estão nos dois READMEs.
O release só pode ser declarado publicado após confirmação do GitHub e dos hashes
dos assets remotos. A validação real no computador de destino continua com o operador.
