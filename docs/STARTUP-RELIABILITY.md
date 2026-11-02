# Confiabilidade do startup do launcher

Escopo: `src/launcher.cpp`, políticas compartilhadas em `src/startup_policy.h`,
contrato em `src/component_contract.h` e fixtures próprias em
`tests/startup_policy_test.cpp`. Não constitui validação do Lake real.

## Comportamento

- `StopBeforeEntry` retorna resultado tipado: pronto, filho encerrado, erro da
  espera, timeout ou outra falha. `EXIT_PROCESS_DEBUG_EVENT.dwExitCode` é
  preservado inclusive para zero, 259 (`STILL_ACTIVE` como código efetivo) e
  códigos de exceção. Um EXIT já disponível é consultado mesmo no deadline.
- `WaitForDebugEvent` com erro real conserva seu Win32; só seus códigos de
  timeout (`ERROR_SEM_TIMEOUT`/`ERROR_TIMEOUT`) ou deadline efetivamente vencido
  são classificados como timeout. Cleanup não substitui o erro primário.
- Qualquer falha de startup em produção tenta gravar
  `logs/LakeWindowedFix-<pid>-startup.log`, **também sem `--wait`**. Antes da
  criação, `pid=0` e `launcher_pid` distingue as invocações. Há fallback em
  `%TEMP%/LakeWindowedFix` após falha de criação, abertura ou escrita local.
  Escritas são append-only. Falha de ambos os destinos é comunicada no stderr.
- O registro diferencia código natural observado de terminação solicitada pelo
  launcher (`exit_code_available`, `launcher_terminated_child`). Ausência de
  código não é saída zero. Código zero não prova janela/gráficos corretos.
- Todos os snapshots usam o mesmo retry: **oito tentativas**, intervalo de
  10 ms, **somente** para `ERROR_BAD_LENGTH`. Esgotamento conserva esse erro;
  outros erros não são repetidos nem renomeados como timeout.
- Mutex nomeado `Local\LakeWindowedFix.Startup.v1.<SID-binário-hex>` é comum a
  instalações do mesmo usuário/sessão. SID vem de `TokenUser`, não de variável
  de ambiente. A aquisição é limitada a 15 s; abandono concede posse, mas não
  dispensa a enumeração. Nenhuma nova dependência de link foi necessária:
  advapi32 é resolvido explicitamente a partir de System32.
- O mutex abrange enumeração por nome, verificação dos arquivos, criação,
  handshake, init e resume. É liberado **antes** de esperar o encerramento.
  A enumeração ainda detecta instância iniciada fora de um launcher. Ela não
  abre memória, suspende, termina ou injeta nessa instância.
- EXE e Contour continuam sujeitos aos hashes admitidos de produção. Seus
  handles `GENERIC_READ`/`FILE_SHARE_READ` ficam retidos até terminar o startup.
  A DLL própria também fica retida durante preflight/load/query/init. Caminhos
  canônicos e pins dos diretórios ancestrais impedem a troca normal por
  escrita, exclusão ou rename entre verificação e carregamento. CWD e Contour
  derivam do caminho canônico do EXE retido, não de um alias mutável de app-dir.
- O INT3 salva byte e proteção originais; até uma tentativa parcial de armá-lo
  passa por restauração. O restore usa a proteção da **primeira** alteração,
  não um eventual RWX deixado por uma falha intermediária. Rewind altera apenas
  EIP do `CONTEXT_CONTROL` lido, preservando flags, stack e segmentos.
- Falha não libera a execução de um filho com breakpoint/contexto danificado.
  A terminação de produção usa somente o handle retornado pelo próprio
  `CreateProcess`, nunca PID descoberto por enumeração. Filho já encerrado não
  recebe solicitação de terminação. Falhas de cleanup aparecem separadamente.

## Contrato com a DLL (integração obrigatória)

`component_contract.h` usa os macros centrais de `product_version.h`:
`LAKE_FIX_VERSION`, `LAKE_FIX_ABI_VERSION` e
`LAKE_FIX_COMPONENT_BUILD_ID`. Este último inclui `-fixture` exclusivamente em
builds `LAKE_FIX_TESTING`. O build stamp efetivo deve ser idêntico no launcher
e na DLL. Não existe opção de runtime para ignorar hash ou handshake.

Exports sem decoração, inclusive no x86:

```cpp
extern "C" DWORD WINAPI LakeWindowedQueryContract(void* output);
extern "C" DWORD WINAPI LakeWindowedInitialize(void* request);
```

1. O preflight mapeia a DLL retida com `DONT_RESOLVE_DLL_REFERENCES` somente para
   obter RVAs. Ambos os exports precisam estar dentro da imagem (exports
   encaminhados para outra imagem são recusados).
2. Após `LoadLibraryW` remoto, são comparados base/tamanho da imagem e
   caminho canônico/identidade de arquivo do módulo realmente carregado.
3. A query recebe `ComponentContract` zerado, com `size` preenchido. Deve
   rejeitar tamanho errado com `ERROR_REVISION_MISMATCH`; caso contrário,
   copiar `ExpectedComponentContract()` e retornar `ERROR_SUCCESS`. Não pode
   instalar hooks nem inicializar estado. `DllMain` deve continuar inerte.
4. O launcher compara **todos** os bytes dos 136 bytes do contrato: magic,
   tamanho, ABI (atualmente 2), machine x86, ponteiros de 4 bytes, versão,
   build/flavor e campos reservados zerados. Mismatch retorna 1306
   (`ERROR_REVISION_MISMATCH`) sem chamar init.
5. Init recebe `InitializeRequest` de **65.756 bytes**, com `size`, `abi`,
   contrato esperado, `ComponentFileIdentity`, SHA-256 binário de 32 bytes e
   caminho UTF-16 canônico do Contour (offset **204**, capacidade 32.768).
   Identidade inclui volume serial, file index high/low, tamanho high/low e
   last-write high/low. A DLL precisa validar e **copiar** o que usará depois:
   o bloco remoto é liberado após o retorno do worker.
6. A DLL deve abrir/reter seu próprio handle de Contour sem compartilhamento de
   escrita/exclusão; comparar o módulo efetivo por caminho/file ID/hash antes
   de usar RVAs, inclusive em loads posteriores. Os hashes admitidos pela DLL
   de produção não podem vir de uma allowlist fornecida no request.

O lado launcher e a integração em `fix.cpp` implementam esse contrato. A DLL
valida o módulo efetivamente retornado pelo loader, mantém o arquivo admitido
retido e fixa o renderer antes de publicar seu par coerente `(base,size)`.
Um Contour homônimo estrangeiro é recusado. A declaração antiga do init com
argumento ignorado não é aceita pelo handshake novo.

A própria DLL é fixada **antes** de publicar callbacks/IAT/VEH, independentemente
do sucesso do logger. Não há unload/unhook ativo. Se faltar seed/layout, a admissão
da apresentação retorna falha sem Blt nativo; a UI agenda o fechamento nativo
depois dos modais de startup, preservando confirmação/veto. **É outro caminho** o
guard de memória/valores de escrita do layout: sua recusa é fatal, chama
`ExitProcess(ERROR_INVALID_DATA)` (13), inclusive ao admitir um canvas recriado,
antes de retornar ao layout incompatível. Não é watchdog de áudio/frame/modal,
nem fechamento normal sujeito a veto. As políticas e a UI adiada têm fixtures;
a recusa desse guard em memória proprietária não foi provocada/executada. Falta
validação no renderer e nas janelas proprietárias.

O ID cabe em 63 caracteres mais NUL (incluindo sufixo de fixture); a versão
cabe em 31 mais NUL. Tamanhos excessivos falham na compilação. Sem identidade
configurada, o preflight falha fechado. `development-untracked` permite somente
concordância de builds manuais: não representa provenance de release.

## Testes reproduzíveis sem Lake

Validação observada em 2026-11-01: Zig **0.14.1**, target
**x86-windows-gnu**, produção e fixtures com `-Wall -Wextra -Wpedantic -Werror`;
**84 verificações, zero falhas**. O launcher de produção foi compilado, não
executado contra Lake.

A rodada isolada original abaixo usou compilação direta, Windows PowerShell,
na raiz do repositório; outputs isolados em `build-launcher-next`. A integração
atual em scripts/CMake e suas rodadas finais constam em [VALIDATION-07.md](VALIDATION-07.md):

```powershell
$zig = '.\build-tools\python\ziglang\zig.exe'
$out = '.\build-launcher-next'
New-Item -ItemType Directory -Force $out | Out-Null
$common = @('c++','-target','x86-windows-gnu','-std=c++17',
  '-DUNICODE','-D_UNICODE','-D_WIN32_WINNT=0x0a00',
  '-Wall','-Wextra','-Wpedantic','-Werror','-O2')
function Compile-Checked([string[]]$Arguments) {
  & $zig @common @Arguments
  if ($LASTEXITCODE) { throw "compile failed: $LASTEXITCODE" }
}
@'
EXPORTS
    LakeWindowedQueryContract=LakeWindowedQueryContract@4
    LakeWindowedInitialize=LakeWindowedInitialize@4
'@ | Set-Content -Encoding ascii "$out\StartupComponent.def"
@'
EXPORTS
    LakeWindowedInitialize=LakeWindowedInitialize@4
'@ | Set-Content -Encoding ascii "$out\StartupComponentMissing.def"
Compile-Checked @('src/launcher.cpp','-lbcrypt','-municode',
  '-o',"$out\LakeWindowedLauncher.exe")
Compile-Checked @('-DLAKE_FIX_TESTING','src/launcher.cpp','-lbcrypt','-municode',
  '-o',"$out\LakeFixInjectionTest.exe")
$test = @('-DLAKE_FIX_TESTING','tests/startup_policy_test.cpp')
Compile-Checked ($test + @('-lbcrypt','-municode','-o',"$out\StartupPolicyTest.exe"))
Compile-Checked ($test + @('-DLAKE_STARTUP_CHILD','-municode','-o',"$out\StartupChild.exe"))
Compile-Checked ($test + @('-DLAKE_STARTUP_CHILD','-DLAKE_STARTUP_ALWAYS_EARLY_EXIT',
  '-municode','-o',"$out\StartupEarlyChild.exe"))
Compile-Checked ($test + @('-DLAKE_STARTUP_COMPONENT','-shared',
  "$out\StartupComponent.def",'-o',"$out\StartupComponentGood.dll"))
Compile-Checked ($test + @('-DLAKE_STARTUP_COMPONENT','-DLAKE_STARTUP_BAD_CONTRACT',
  '-shared',"$out\StartupComponent.def",'-o',"$out\StartupComponentMismatch.dll"))
Compile-Checked ($test + @('-DLAKE_STARTUP_COMPONENT','-DLAKE_STARTUP_MISSING_QUERY',
  '-shared',"$out\StartupComponentMissing.def",'-o',"$out\StartupComponentMissing.dll"))
& "$out\StartupPolicyTest.exe"
if ($LASTEXITCODE) { throw "tests failed: $LASTEXITCODE" }
```

As DLLs acima são fixtures inertes próprias, não a DLL de correção do Lake.
O runner cria diretórios exclusivos, copia arquivos com fail-if-exists e limpa
somente os artefatos que criou. Logs compartilhados de fallback TEMP são
append-only e nunca apagados. Filhos sintéticos têm waits limitados; eventual
terminate usa somente handles dos filhos que o próprio teste criou.

Cobertura: políticas puras compartilhadas com produção; cada byte do contrato;
distinção produção/fixture; retry esgotado/transitório/não transitório; hash
conhecido e allowlist rejeitada; bloqueio de write/delete/rename/rename do pai;
INT3 byte/proteção e contexto x86; EXIT real em callback TLS com códigos 0, 23,
259 e `0xC0000005`; EXIT já pendente no deadline; timeout real; erro de espera
injetado pela fronteira de API (não um erro natural do kernel); mutex abandonado
e dois processos concorrentes; descoberta externa sem abrir memória; query
remota bem-sucedida; build mismatch remoto sem init; export ausente; launcher
real de fixture sem `--wait` em mismatch/early-exit; fallback após log local
bloqueado preservando o conteúdo anterior.

## Limitações e riscos ainda não resolvidos

- Nenhum Lake real foi executado; isto não prova renderização, áudio, tela,
  compatibilidade com drivers, comportamento do loader de plugins ou estabilidade
  prolongada. Integração Contour da DLL deve ser validada separadamente.
- Mutex só coordena launchers participantes do mesmo usuário/sessão. Um Lake
  iniciado diretamente pode aparecer **depois** da enumeração; não existe lock
  cooperativo no software original. Outras sessões/usuários não compartilham
  esse mutex, embora a enumeração por nome seja conservadora no sistema todo.
- Cada estágio remoto possui limite de 15 s; isso não é um orçamento global de
  15 s para todo o startup. O custo de hashing/leitura de disco/snapshot não é
  cancelável por esse deadline.
- Handle/pins protegem operações normais do filesystem, não um kernel hostil,
  drivers, memória de outro processo privilegiado ou mappings de escrita
  preexistentes. File ID não é assinatura digital. O handshake detecta mistura
  de componentes; não autentica uma DLL maliciosa que forje os mesmos campos.
- A query só ocorre depois de `LoadLibraryW`, portanto depois de `DllMain`.
  Inércia de `DllMain` é parte obrigatória do contrato, não algo que o launcher
  possa provar. O preflight local não executa DllMain/dependências.
- Toolhelp usa `szExePath` limitado a `MAX_PATH` para conferir o módulo remoto.
  Caminho que não puder ser comparado falha fechado; suporte a paths remotos
  maiores não foi demonstrado. UNC/reparse points, diferentes níveis de
  integridade e ACLs personalizadas exigem testes adicionais.
- Restore/detach/terminate também podem falhar. Há best effort limitado e erro
  de cleanup separado; não se promete rollback perfeito sob falha do kernel.
  O worker remoto com timeout mantém seu argumento alocado até teardown do
  filho próprio, em vez de liberar memória ainda potencialmente em uso.
- Sem `--wait`, após startup bem-sucedido o launcher não observa todo o ciclo
  de vida. Relatório de startup não é monitor de crashes tardios.

## Referências oficiais consultadas

- [CreateToolhelp32Snapshot](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot):
  `ERROR_BAD_LENGTH`, mudanças concorrentes do loader e recomendação de retry.
  O teto local evita espera ilimitada; esgotamento não significa processo ausente.
- [WaitForDebugEvent](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-waitfordebugevent):
  resultado/GetLastError, afinidade de thread e propriedade dos handles de evento.
- [Debugging Events](https://learn.microsoft.com/en-us/windows/win32/debug/debugging-events):
  threads paradas durante evento, EXIT e necessidade de ContinueDebugEvent.
- [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew):
  compartilhamento permanece em vigor até fechar o handle; delete inclui rename.
