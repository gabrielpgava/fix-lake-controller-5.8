# Instalação nativa recuperável — candidata 0.7.0

O operador não precisa de Make, compilador ou PowerShell. Use o setup externo
revisado, confira checksums/manifesto e mantenha o Lake fechado durante operações.
O Lake Controller **5.8 Build 007 legítimo e admitido por hash** precisa estar
instalado para instalar/atualizar o patch. Nenhum produto proprietário é fornecido.

## Operação

1. Abra `LakeWindowedFix-Setup-v0.7.0.exe`. O setup procura a instalação padrão;
   **Localizar Lake** permite selecionar outra pasta. Hash diferente é recusado.
2. Confira o destino e clique **Instalar**. Arquivos do patch e dois atalhos ficam
   no perfil do usuário; não há escrita na instalação original, elevação, scripts
   PowerShell, download ou mudança de política de segurança na máquina de destino.
3. Abra pelo atalho **Lake Controller - Janela**, somente em condições legítimas,
   isoladas e sem operação de áudio/equipamentos. O setup nunca inicia o Lake sozinho.
4. Para remover, use Aplicativos ou o setup externo. Não encerra o Lake à força.
   INI editado, logs e objetos alterados/desconhecidos são preservados e informados.
   Recibo/marcadores permanecem para recuperação/reinstalação; não apague a pasta
   recursivamente nem misture com os scripts PowerShell legados de instalação.

As pastas/chaves históricas `v0.6` são mantidas para descobrir a instalação anterior;
isso não define a versão dos executáveis novos. O manager instalado anterior não
ganha recursos novos sozinho: use o setup **novo externo** para atualizar/recuperar.

## Reparo, atualização e reversão

- Executar novamente a mesma geração repara somente ausências reconhecidas e
  mescla apenas chaves de configuração ausentes. Edições e backup do INI permanecem.
- Outro conjunto de binários exige `--upgrade` explícito no setup novo externo.
- `--rollback` recupera a geração anterior, inclusive após promoção parcial.
- Há **uma geração anterior** retida; não há histórico arbitrário, redo ou purge.
- Conflitos, journals desconhecidos e reparse points falham fechados. Preserve os
  arquivos e siga [SETUP-RECOVERY.md](SETUP-RECOVERY.md), sem limpeza automática.

## Segurança e evidência

O setup é x86 GUI/asInvoker e **não assinado**. Windows/EDR podem bloquear conforme
a política local; não desative as proteções. Integridade por hash/proveniência não
substitui Authenticode. Não há validação Lake/GPU/áudio desta candidata nem aprovação
de uso em campo. OFFLINE no aplicativo **não isola a rede**.

Sandbox integrado: **421 checks, zero falhas, 1 skip opcional de symlink** (erro
1314, não contado como PASS), sem instalação operacional. `--verify-payload` testa
os seis buffers MIT embutidos no EXE final sem GUI/instalação. O fluxo real de
Apps/autorremoção e sua confirmação/handshake não foi executado; a evidência do
engine não é alegada como cobertura desse processo. CMake com setup ON passou;
MSVC local e workflow remoto ainda exigem evidência própria.

Comandos, BuildIds, testes e pendências: [VALIDATION-07.md](VALIDATION-07.md).
Histórico 0.6 separado: [HISTORY.md](../HISTORY.md) e Git.
