# Validação e limites

## Ambiente desta implementação

- Host: Windows 11 Home Single Language, `10.0.26300`, 64-bit.
- Toolchain disponível usada: Zig 0.14.1, target `x86-windows-gnu`, C++17.
- Inspeção: Python 3.12.8, pefile 2024.8.26, Capstone 5.0.7.
- MSVC/MinGW e Windows 10: instruções fornecidas; execução não observada neste ambiente.
- O agente não executou os binários Lake. Após o relato do usuário, houve consulta passiva às janelas de uma instância que ele abriu, sem interagir com rede/hardware ou alterar/encerrar o processo. O funcionamento gráfico/áudio real da 0.2 ainda não foi testado.

## Testes automáticos

Resultado efetivamente observado em 2026-10-24:

### Atualização 0.2

- Regressão nova reproduz o pedido nativo `PostMessage(0x8032)` → `ShowWindow(main, 1)` antes do loop externo: **duas falhas confirmadas na DLL 0.1**.
- Build 0.2 Release x86 Zig: **46 verificações PASS, 0 falhas**, incluindo as duas regressões, visibilidade/hide/minimizar/restaurar, fallback externo, loop interno filtrado pelo HWND do cliente e descoberta de instância existente sem acesso à memória.
- Os originais e a pasta `dist/` da 0.1 não foram substituídos. A saída nova está em `dist-v0.2/` porque a DLL antiga continuava em uso.
- A consulta às janelas do PID histórico 5836 confirmou host oculto e cliente com `WS_VISIBLE` mas invisível por ancestral oculto. **Não houve lançamento/reteste automático do Lake real.**
- Logs em `private/regression-v01.txt`, `private/build-validation-v02.txt` e `private/window-state-v01-failure.json` (todos ignorados).

### Registro anterior, 0.1

| Verificação | Resultado |
| --- | --- |
| Build Release x86, Zig 0.14.1 | Aprovado, sem warnings na execução final |
| Fixture Win32/injeção + autoteste SHA-256 | **35 verificações PASS, 0 falhas** |
| Launcher de produção com pasta sem hashes originais | Recusou com exit 3, sem iniciar alvo |
| Parser PowerShell (quatro scripts) | Sem erros de sintaxe |
| Compilação sintática do script Python | Aprovada |
| Inspeção PE de `dist/` | Launcher/DLL x86; export `LakeWindowedInitialize` presente |
| Reinspeção dos quatro arquivos originais | SHA-256 idênticos aos iniciais |
| MSVC, MinGW GCC, Windows 10 e renderer Lake real | **Não executados** |

O log local está em `private/build-validation.txt` (ignorado). Os binários gerados ficam em `dist/` (também ignorado); apenas fontes/documentação devem ser versionados.

O build padrão compila uma aplicação Win32 inteiramente própria (`LakeFixFixture.exe`), uma **`Contour.dll` sintética** de teste e um launcher de teste (`LakeFixInjectionTest.exe`). O segundo reutiliza o fonte de injeção de produção, mas só inicia o nome fixo da fixture. O launcher de produção mantém sua allowlist SHA-256 e não contém esse modo de teste. A DLL sintética não contém código Lake, nunca é instalada por este projeto e **não deve ser copiada para a instalação real**.

Cobertura esperada da fixture:

- ABI x86, injeção antes do entry point, export WINAPI sem decoração externa.
- SHA-256 de vetor conhecido (`abc`) aceito e hash incorreto recusado.
- Criação ANSI e Unicode, popup convertido para renderer filho + host padrão.
- Janela não relacionada permanece intacta.
- Posicionamento inicial enfileirado; título/bordas/resize/minimum size.
- Bombeamento reentrante de `PeekMessage` antes do primeiro `GetMessage` não habilita resize prematuro.
- Coalescência de `WM_SIZE` durante arraste, sem rebuild com tamanho zero/minimizado.
- `WM_KILLFOCUS` externo não alcança o bounce legado simulado; ativação continua entregue.
- Guardas de `SetWindowLong`, `SetWindowPos` e chamadas obtidas com `GetProcAddress`.
- `ChangeDisplaySettingsExW` neutralizado, mas `CDS_TEST` encaminhado. O dispositivo de teste é propositalmente inexistente para não alterar resolução mesmo se um teste regredir.
- Destruição e recriação de janelas.
- `WM_CLOSE` da moldura respeita veto e cleanup do main.
- Carregamento tardio da DLL gráfica sintética, com imports interceptados antes de sua factory ser usada; erros de `LoadLibrary` preservados.

Esses testes **não** criam superfícies DirectDraw/Direct3D7. Não validam mouse/touch no Lake, dependências legadas, perda de dispositivo, latência de áudio ou correção do travamento real. Logs/estado final de execução devem acompanhar cada versão; não tratar “compilou” como “testado no Lake”.

Os scripts PowerShell também foram analisados pelo parser da versão local sem erros de sintaxe. Os instaladores não foram executados contra a pasta real protegida do Lake.

## Matriz gráfica/manual ainda pendente

Faça os testes em uma instalação licenciada, com presets/configuração de teste e sem processadores de áudio em produção. Registre resultados **separados** para Windows 10 e Windows 11, GPU/driver, monitores e DPI.

| Caso | Resultado esperado | Estado |
| --- | --- | --- |
| Abrir pelo launcher | Host com título/bordas; nenhuma troca de resolução | Pendente |
| Mover entre posições do desktop | Janela se move; mouse permanece alinhado | Pendente |
| Resize até mínimo e maximizar/restaurar | UI é reconstruída para o novo cliente; não recursiona | Pendente |
| Arraste contínuo de borda | Sem rebuild por pixel; redesenho após soltar | Pendente |
| Alt+Tab 100 vezes | Outra janela recebe foco; volta sem travar | Pendente |
| Minimizar/restaurar 50 vezes | Sem buffers 0×0; repaint e input normais | Pendente |
| Botão interno “minimize” | Minimiza o host, não apenas o renderer filho | Pendente |
| Diálogos de arquivos, avisos e texto | Modalidade, teclado e foco funcionam | Pendente |
| Touch, IME e acessibilidade | Sem regressões de input | Pendente |
| 100%, 125%, 150%, 200% DPI | Coordenadas e bordas verificadas, não presumidas | Pendente |
| Vários monitores / DPI misto | Mover e restaurar; testar clipping | Pendente |
| RDP, hotplug, bloqueio de sessão | Registrar perda/recuperação de superfícies | Pendente, sem garantia |
| Fechar por X e pela UI interna | Threads encerram; sem processo órfão | Pendente |
| Reabrir pelo EXE original | Comportamento original, hashes idênticos | Pendente |
| Rede/áudio fora de produção | Descoberta/controle normal; sem alterações involuntárias | Pendente |

Se o processo travar, capture evidência **com autorização** e guarde em `dumps/` ignorado. Não publique presets, endereços ou dumps completos. Não faça firmware update nem altere adaptadores como parte de um teste gráfico.

## Critério para chamar a correção de validada

Somente após a matriz essencial (abrir, mover, resize, Alt+Tab, minimizar, diálogos e fechar) passar em **cada** combinação de SO/GPU/driver alegada. Antes disso o status é **experimental**, mesmo com todos os testes Win32 sintéticos aprovados.
