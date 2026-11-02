# Contribuir

1. Trabalhe em código próprio sob MIT. Não acrescente binários, recursos, dumps, firmware ou código descompilado integral do Lake.
2. Faça build **x86** e rode os testes sintéticos em `scripts/Build.ps1`/CTest. Não execute automaticamente o Lake em CI.
3. Para outro build, anote SHA-256 e confira imports, nomes de classe, parser e contrato de resize antes de alterar a allowlist. Preserve os limites da evidência estática.
4. Não faça hooks globais, alterações permanentes no EXE, bypass de licenciamento nem sobrescreva hooks de terceiros.
5. Preserve forwarding, calling conventions WINAPI x86, cleanup de falhas, tempos limite e inicialização fora de `DllMain`. Não instale hooks em uma worker concorrendo com a criação de janela.
6. Testes gráficos reais devem registrar SO, GPU/driver, DPI, monitores, flags, resultado de Alt+Tab/minimize/resize e regressões de input/diálogos. Declare explicitamente o que não foi testado.
7. Relatórios privados devem ficar em `private/`, `evidence/` ou `dumps/`, ignorados pelo Git. Revise `git status` e `git diff --cached` antes de publicar.

Abra issues com passos reprodutíveis e logs mínimos sem presets privados, endereços de dispositivos ou credenciais. Nenhum acesso a hardware/áudio é necessário para contribuir com a fixture Win32.

Na 0.7, launcher e DLL de teste têm flavor `-fixture`, compilados separadamente;
não instale a DLL `LakeWindowedFix.Test.dll` nem crie bypass de hashes em produção.
O build registra seu snapshot antes da compilação e liga testes aos hashes finais.
Não altere fontes durante esse build nem reutilize logs de uma revisão anterior.
Fixtures, GPU própria e Lake real são camadas distintas. Veja
[a matriz](docs/IMPLEMENTATION.md) e [a proveniência](docs/BUILD-PROVENANCE.md).
