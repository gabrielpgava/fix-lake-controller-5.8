# Instalar sem comandos

O objetivo deste pacote e separar a complexidade de desenvolvimento da operacao: **o usuario nao precisa compilar, executar PowerShell ou editar INI**.

## Fluxo do usuario

1. Abra **`LakeWindowedFix-Setup-v0.6.0.exe`** do pacote de release com duplo clique.
2. Confira o Lake detectado e clique em **Instalar**.
3. Use o atalho **Lake Controller - Janela**, criado na area de trabalho e no menu Iniciar.

O Lake Controller **5.8 Build 007 original precisa estar instalado**. Na instalacao padrao ele e encontrado automaticamente; para uma pasta diferente, o botao **Localizar Lake** abre o seletor de pastas. Uma versao diferente e recusada, nao "adaptada" silenciosamente.

O instalador nao abre o Lake automaticamente. A selecao de rede e as confirmacoes normais do programa original continuam sendo feitas pelo operador; nao escolhemos adaptadores ou recuperamos presets automaticamente.

## O que muda no computador

- Arquivos do patch em **`%LOCALAPPDATA%\LakeWindowedFix\v0.6`**, apenas para o usuario atual.
- Atalhos na area de trabalho e no menu Iniciar desse usuario.
- Entrada propria em **Configuracoes > Aplicativos**, para remover o patch.
- **Nenhum arquivo proprietario e incluido ou substituido.** Nao escreve na pasta do Lake, nao exige administrador e nao altera rede, firmware, presets, registro de compatibilidade ou politica de seguranca.
- O instalador nao baixa dependencias nem executa scripts PowerShell na maquina de destino.

Feche o Lake antes de instalar ou remover. O instalador nao encerra processos por conta propria. Arquivos/atalhos existentes que nao puderem ser reconhecidos como nossos sao preservados, com mensagem de erro em vez de sobrescrita.

## Remover

Use **Configuracoes > Aplicativos > Lake Controller - Janela** e escolha desinstalar. A remocao confere os arquivos/atalhos registrados; configuracao INI editada e arquivos desconhecidos sao preservados. O Lake original permanece instalado.

Para remover o proprio executavel de gerenciamento, um auxiliar verificado e copiado para uma pasta temporaria exclusiva. Ele espera a instancia original do desinstalador sair normalmente, sem mata-la; o auxiliar temporario fica para a limpeza normal de temporarios do Windows. Nao usa comandos de shell ou exclusao recursiva.

## Limites

**Instalacao simples nao significa homologacao para campo.** O runtime 0.6 permanece experimental e com validacao OFFLINE limitada, conforme [PERFORMANCE.md](PERFORMANCE.md). O instalador nao torna o programa somente-leitura quando ele estiver online.

O executavel ainda nao e assinado digitalmente: Windows/EDR podem pedir confirmacao ou bloquear conforme a politica local. Verifique origem e SHA-256; nao desative as protecoes para instalar. O pacote nao inclui o Lake original nem seus runtimes legados.

## Gerar o instalador — somente desenvolvedores

Com o runtime 0.6 compilado e seu INI padrao intacto:

```powershell
.\scripts\Build-Installer.ps1
```

Saida separada: **`dist-installer-v0.6\LakeWindowedFix-Setup-v0.6.exe`**, `LICENSE` e `SHA256SUMS.txt`. O instalador incorpora somente os seis arquivos MIT do runtime; nenhuma `Contour.dll` real ou sintetica entra no pacote.

Alternativa CMake/MSVC: configurar com `-DLAKE_BUILD_SETUP=ON`. O alvo opcional `LakeWindowedSetup` gera o payload a partir dos binarios do build e da configuracao padrao; `installer_sandbox` testa em pastas sinteticas, sem instalar no perfil real.

O build/teste do instalador nao modifica a instalacao do Lake nem executa o aplicativo. Resultados: **58 checks, zero falhas, zero warnings**, com um grupo opcional de symlink pulado por falta de privilegio; nao se pediu elevacao para faze-lo. Foram exercitados instalacao, atalhos, repeticao, configuracao editada, recusas, rollback e remocao em pastas sinteticas, com registro/notificacoes globais desligados.

O executavel final foi verificado como **x86 GUI/asInvoker** e aberto em `--check`, que so le a instalacao original e mostra o diagnostico. Detectou o Build007, fechou com codigo0 e nao criou pasta/registro de instalacao no perfil real. O caminho de HKCU e a remocao automatica da propria instalacao nao foram exercitados no perfil real; o handshake de identidade/autorizacao do auxiliar foi revisado estaticamente. MSVC/CMake permanecem alternativas nao executadas neste ambiente. Os resultados de runtime 0.6 nao aprovam automaticamente o instalador nem o uso com hardware.
