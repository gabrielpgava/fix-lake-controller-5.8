#pragma once
#include <windows.h>
#include <cstddef>
#include <cwchar>

namespace lakefix::setup {
enum class Language { Portuguese, English };
inline Language LanguageFromId(LANGID id) noexcept {
    return PRIMARYLANGID(id) == LANG_PORTUGUESE ? Language::Portuguese : Language::English;
}
inline const wchar_t* LanguageArgument(Language language) noexcept {
    return language == Language::Portuguese ? L"pt-BR" : L"en";
}
inline bool ParseLanguage(const wchar_t* value, Language& language) noexcept {
    if (!value) return false;
    if (_wcsicmp(value, L"pt-BR") == 0 || _wcsicmp(value, L"pt") == 0) language = Language::Portuguese;
    else if (_wcsicmp(value, L"en") == 0) language = Language::English;
    else return false;
    return true;
}

// One catalog for all own installer UI, engine results and preserved-object
// labels. Never translate receipt keys, file names, registry identities or
// shortcut names: existing installations must remain discoverable in either UI.
#define LAKE_SETUP_TEXTS(X) \
 X(None, L"", L"") \
 X(Title, L"Lake Controller - Instalar patch de janela", L"Lake Controller - Windowed patch setup") \
 X(LanguageLabel, L"Idioma / Language", L"Idioma / Language") \
 X(BrowseTitle, L"Selecione a pasta original do Lake Controller 5.8 Build 007", L"Select the original Lake Controller 5.8 Build 007 folder") \
 X(Locate, L"Localizar Lake...", L"Locate Lake...") \
 X(Install, L"Instalar / Reparar", L"Install / Repair") \
 X(Remove, L"Remover", L"Uninstall") \
 X(Upgrade, L"Atualizar", L"Upgrade") \
 X(Rollback, L"Reverter", L"Roll back") \
 X(Cancel, L"Cancelar", L"Cancel") \
 X(Close, L"Fechar", L"Close") \
 X(Finish, L"Concluir", L"Finish") \
 X(RemoveDescription, L"Remover o patch deste usuário?\r\n\r\nO Lake original e seus presets não serão removidos.\r\nINI editado, logs e arquivos/atalhos alterados serão preservados.\r\nRecibo e marcador permanecem para recuperação/reinstalação.\r\n\r\n", L"Uninstall the patch for this user?\r\n\r\nThe original Lake application and presets will not be removed.\r\nEdited INI, logs and modified files/shortcuts are preserved.\r\nThe receipt and marker remain for recovery/reinstallation.\r\n\r\n") \
 X(RollbackDescription, L"Restaurar a geração anterior?\r\n\r\nUse o setup externo, com o Lake fechado.\r\nINI atual, logs e arquivos pessoais serão mantidos.\r\nHá apenas uma geração anterior disponível.\r\n\r\n", L"Restore the previous generation?\r\n\r\nUse the external setup with Lake closed.\r\nThe current INI, logs and personal files are kept.\r\nOnly one previous generation is available.\r\n\r\n") \
 X(UpgradePrefix, L"Atualizar o patch para a versão ", L"Upgrade the patch to version ") \
 X(UpgradeDescription, L"?\r\n\r\nOs arquivos são preparados e verificados antes da troca.\r\nMantenha o Lake fechado até concluir.\r\nUse --rollback para reverter uma troca interrompida.\r\n\r\n", L"?\r\n\r\nFiles are staged and verified before replacement.\r\nKeep Lake closed until the operation finishes.\r\nUse --rollback to reverse an interrupted upgrade.\r\n\r\n") \
 X(NoApplication, L"Lake Controller 5.8 Build 007 compatível não encontrado.\r\n\r\nInstale o software original legítimo ou clique em Localizar Lake.\r\nNenhum arquivo do fabricante será substituído.", L"A supported Lake Controller 5.8 Build 007 installation was not found.\r\n\r\nInstall the legitimate original software or click Locate Lake.\r\nNo manufacturer file will be replaced.") \
 X(ApplicationFound, L"Lake compatível encontrado:\r\n", L"Supported Lake installation found:\r\n") \
 X(InstallDescription, L"\r\n\r\nInstalação por usuário, com atalhos na área de trabalho e no menu Iniciar.\r\nInstalar também repara ausências de uma geração compatível.\r\nPara outra geração, execute --upgrade; para reverter, --rollback.\r\n\r\nVersão ", L"\r\n\r\nPer-user installation, with desktop and Start menu shortcuts.\r\nInstall also repairs missing files from a compatible generation.\r\nFor another generation use --upgrade; to reverse it use --rollback.\r\n\r\nVersion ") \
 X(Experimental, L" experimental. Teste isolado, sem áudio/equipamentos em operação. OFFLINE não isola a rede.", L" is experimental. Test in isolation without operational audio/equipment. OFFLINE does not isolate the network.") \
 X(ReadOnly, L"\r\n\r\nVerificação somente-leitura: nada será instalado.", L"\r\n\r\nRead-only check: nothing will be installed.") \
 X(Busy, L"Aguarde... Os arquivos originais do Lake não serão alterados.", L"Please wait... The original Lake files will not be changed.") \
 X(WindowsError, L"\r\nCódigo Windows: ", L"\r\nWindows error code: ") \
 X(RemovalWorkerFailed, L"Não foi possível preparar a remoção. Nenhum arquivo foi removido.", L"Could not prepare uninstallation. No file was removed.") \
 X(RightsNotice, L"SOMENTE PATCH - Lake Controller, Contour e arquivos proprietários NÃO estão incluídos. Requer instalação original legítima e licença quando exigida. Projeto independente, não oficial nem endossado pelo fabricante. Instalador experimental, não assinado; respeite a política de segurança do Windows.", L"PATCH ONLY - Lake Controller, Contour and proprietary files are NOT included. Requires a legitimate original installation and a license where required. Independent, unofficial project, not endorsed by the manufacturer. Experimental unsigned installer; follow Windows security policy.") \
 X(CannotCheckStopped, L"Não foi possível verificar se o Lake Controller está fechado.", L"Could not check whether Lake Controller is closed.") \
 X(CloseLake, L"Feche o Lake Controller antes de instalar ou remover o modo janela.", L"Close Lake Controller before installing or uninstalling the windowed patch.") \
 X(SelectOriginalFolder, L"Selecione a pasta original do Lake Controller em um caminho local absoluto, sem links.", L"Select the original Lake Controller folder using an absolute local path without links.") \
 X(InvalidBuildIdentity, L"A identificação da versão suportada é inválida.", L"The supported-build identity is invalid.") \
 X(CannotReadApplication, L"Não foi possível ler Lake Controller.exe na pasta selecionada.", L"Could not read Lake Controller.exe in the selected folder.") \
 X(CannotReadRenderer, L"Não foi possível ler Contour.dll na pasta selecionada.", L"Could not read Contour.dll in the selected folder.") \
 X(CannotVerifyApplication, L"Não foi possível verificar Lake Controller.exe.", L"Could not verify Lake Controller.exe.") \
 X(CannotVerifyRenderer, L"Não foi possível verificar Contour.dll.", L"Could not verify Contour.dll.") \
 X(UnsupportedBuild, L"Esta versão do Lake Controller não é suportada. Nenhum arquivo original foi alterado.", L"This Lake Controller build is not supported. No original file was changed.") \
 X(OriginalVerified, L"Versão original verificada (somente leitura).", L"Original build verified (read-only).") \
 X(EditedIni, L"LakeWindowedFix.ini (editado)", L"LakeWindowedFix.ini (edited)") \
 X(DesktopShortcut, L"atalho da area de trabalho", L"desktop shortcut") \
 X(StartShortcut, L"atalho do menu Iniciar", L"Start menu shortcut") \
 X(ModifiedRegistration, L"registro de remocao alterado", L"modified uninstall registration") \
 X(PreservedObjects, L"\r\nObjetos conhecidos preservados: ", L"\r\nPreserved known objects: ") \
 X(UnknownUpgradeMarker, L"Marcador de atualização desconhecido. Nada foi substituído.", L"Unknown upgrade marker. Nothing was replaced.") \
 X(RollbackTerminal, L"Rollback já concluído. O histórico foi preservado; reaplicar/rotacionar esta geração ainda não é suportado.", L"Rollback is already complete. History was preserved; reapplying/rotating this generation is not supported yet.") \
 X(CompleteRollbackFirst, L"Rollback solicitado. Conclua com --rollback antes de outra operação.", L"Rollback was requested. Complete --rollback before another operation.") \
 X(CompletedStateChanged, L"O estado concluído foi alterado. Os arquivos atuais e o histórico foram preservados.", L"The completed state was modified. Current files and history were preserved.") \
 X(AlreadyRolledBack, L"A versão anterior já foi restaurada. Configuração e logs mantidos.", L"The previous version is already restored. Configuration and logs were kept.") \
 X(AlreadyUpgraded, L"A atualização já foi concluída.", L"The upgrade is already complete.") \
 X(ModifiedActiveOrStage, L"Há arquivo/atalho/registro alterado no estado ativo ou no staging. Foi preservado; promoção/rollback recusado.", L"An active or staged file/shortcut/registration was modified. It was preserved; promotion/rollback was refused.") \
 X(CannotRecreateShortcutFolder, L"Não foi possível recriar a pasta de um atalho ausente.", L"Could not recreate the folder for a missing shortcut.") \
 X(RollbackRequested, L"Pedido de rollback registrado. Execute --rollback novamente para continuar.", L"Rollback request recorded. Run --rollback again to continue.") \
 X(SwapInterrupted, L"Troca interrompida. Mantenha o Lake fechado; retome --upgrade ou solicite --rollback.", L"Replacement interrupted. Keep Lake closed; resume --upgrade or request --rollback.") \
 X(RegistrySwapInterrupted, L"Troca do registro interrompida; manager e recibos permanecem recuperáveis.", L"Registry replacement interrupted; the manager and receipts remain recoverable.") \
 X(VersionConfirmationInterrupted, L"Confirmação de versão interrompida. Use --upgrade/--rollback para recuperar.", L"Version confirmation interrupted. Use --upgrade/--rollback to recover.") \
 X(PromotedVerificationFailed, L"A verificação da versão promovida falhou. Histórico anterior mantido.", L"Verification of the promoted version failed. Previous history was kept.") \
 X(CommitConfirmationInterrupted, L"A troca terminou, mas a confirmação foi interrompida. Execute a mesma operação novamente.", L"Replacement finished, but confirmation was interrupted. Run the same operation again.") \
 X(RollbackComplete, L"Versão anterior restaurada. INI atual, logs e histórico da atualização preservados.", L"Previous version restored. Current INI, logs and upgrade history were preserved.") \
 X(UpgradeComplete, L"Atualização concluída. Configuração migrada sem trocar edições; versão anterior preservada para --rollback.", L"Upgrade complete. Configuration was migrated without replacing edits; the previous version was preserved for --rollback.") \
 X(PayloadVerified, L"Os seis payloads embutidos têm nomes e SHA-256 válidos.", L"The six embedded payloads have valid names and SHA-256 hashes.") \
 X(InvalidEmbeddedPayload, L"Payload embutido inválido: nome, contagem ou SHA-256 não confere.", L"Invalid embedded payload: name, count or SHA-256 does not match.") \
 X(InvalidUpgradePayload, L"Payload da atualização inválido.", L"Invalid upgrade payload.") \
 X(InvalidUpgradeDestinations, L"Destinos da atualização inválidos.", L"Invalid upgrade destinations.") \
 X(CannotReadJournalApplication, L"Não foi possível ler o Lake registrado no journal.", L"Could not read the Lake path recorded in the journal.") \
 X(CannotReadReceiptApplication, L"Não foi possível ler a pasta original do recibo.", L"Could not read the original application folder from the receipt.") \
 X(InvalidUpgradePath, L"Caminho da atualização inválido.", L"Invalid upgrade path.") \
 X(ExternalUpgradeRequired, L"Execute o setup novo fora da pasta instalada/original para atualizar; mantenha o Lake fechado.", L"Run the new setup outside the installed/original folder to upgrade; keep Lake closed.") \
 X(CannotReadNewSetup, L"Não foi possível ler o setup novo.", L"Could not read the new setup.") \
 X(InvalidUpgradeManager, L"O gerenciador da atualização é inválido.", L"The upgrade manager is invalid.") \
 X(CannotPrepareShortcuts, L"Não foi possível preparar os atalhos.", L"Could not prepare shortcuts.") \
 X(CannotPinUpgradePaths, L"Não foi possível proteger os caminhos da atualização.", L"Could not protect upgrade paths.") \
 X(UnsafeUpgradeFolder, L"A pasta da atualização contém caminho inseguro.", L"The upgrade folder contains an unsafe path.") \
 X(CannotInspectStaging, L"Não foi possível verificar o staging.", L"Could not inspect staging.") \
 X(InvalidPreviousReceipt, L"Recibo anterior inválido ou pertencente a outro Lake.", L"The previous receipt is invalid or belongs to another Lake installation.") \
 X(UpgradePreparationInterrupted, L"Preparação da atualização interrompida. A versão ativa não foi substituída; execute --upgrade novamente.", L"Upgrade preparation interrupted. The active version was not replaced; run --upgrade again.") \
 X(CannotPinUpgradeJournal, L"Não foi possível proteger o journal da atualização.", L"Could not protect the upgrade journal.") \
 X(UnknownUpgradeFiles, L"Journal/arquivos da atualização desconhecidos ou alterados. Nada foi adotado.", L"Upgrade journal/files are unknown or modified. Nothing was adopted.") \
 X(DifferentUpgradePackage, L"Este histórico pertence a outro pacote. Use o mesmo setup para retomar ou --rollback para reverter.", L"This history belongs to another package. Use the same setup to resume or --rollback to reverse it.") \
 X(ExternalRollbackRequired, L"Execute --rollback pelo setup externo, fora da pasta instalada.", L"Run --rollback using the external setup, outside the installed folder.") \
 X(CannotCheckShortcuts, L"Não foi possível verificar os atalhos.", L"Could not check shortcuts.") \
 X(CannotPinRollbackPaths, L"Não foi possível proteger os caminhos do rollback.", L"Could not protect rollback paths.") \
 X(UnsafeRollbackPath, L"Rollback recusado: caminho inseguro.", L"Rollback refused: unsafe path.") \
 X(NoValidUpgradeHistory, L"Não há histórico de atualização íntegro para reverter.", L"There is no intact upgrade history to roll back.") \
 X(InvalidInstallerFiles, L"Os arquivos do instalador são inválidos ou estão corrompidos.", L"Installer files are invalid or corrupted.") \
 X(AbsoluteOriginalPathRequired, L"A pasta original deve ser um caminho local absoluto, sem '..'.", L"The original folder must use an absolute local path without '..'.") \
 X(InvalidInstallDestinations, L"As pastas de instalação e atalhos devem ser distintas, sem links e fora da pasta original, do Windows e de Program Files.", L"Installation and shortcut folders must be distinct, without links, and outside the original, Windows and Program Files folders.") \
 X(PendingUpgradeBeforeRepair, L"Há atualização/rollback pendente. Use --upgrade ou --rollback antes de instalar/reparar.", L"An upgrade/rollback is pending. Use --upgrade or --rollback before installing/repairing.") \
 X(InvalidInstallerPath, L"O caminho do instalador é inválido.", L"The installer path is invalid.") \
 X(OwnManagerRequired, L"O gerenciador deve ser o próprio instalador, fora da pasta original do Lake. Nenhum arquivo proprietário será copiado.", L"The manager must be this installer, outside the original Lake folder. No proprietary file will be copied.") \
 X(CannotReadSelf, L"Não foi possível ler o próprio instalador.", L"Could not read the installer itself.") \
 X(CannotVerifySelf, L"Não foi possível verificar o próprio instalador.", L"Could not verify the installer itself.") \
 X(ProprietaryManagerRefused, L"Um arquivo original do Lake não pode ser usado como gerenciador de instalação.", L"An original Lake file cannot be used as the installation manager.") \
 X(CannotPrepareWindowsShortcuts, L"Não foi possível preparar os atalhos do Windows.", L"Could not prepare Windows shortcuts.") \
 X(CannotPinInstallPaths, L"Não foi possível proteger as pastas contra alterações durante a instalação.", L"Could not protect folders against modification during installation.") \
 X(UnownedFolder, L"A pasta já existe sem um recibo de propriedade válido. Nada foi adotado ou sobrescrito.", L"The folder already exists without a valid ownership receipt. Nothing was adopted or overwritten.") \
 X(DifferentOriginalInstallation, L"Esta instalação pertence a outra pasta original do Lake. Nada foi alterado.", L"This installation belongs to another original Lake folder. Nothing was changed.") \
 X(ExplicitUpgradeRequired, L"Este pacote é diferente da instalação anterior. Use --upgrade explicitamente para atualizar com histórico e rollback.", L"This package differs from the previous installation. Explicitly use --upgrade to update with history and rollback.") \
 X(CannotPinExistingFiles, L"Não foi possível proteger os arquivos existentes. Nada foi sobrescrito.", L"Could not protect existing files. Nothing was overwritten.") \
 X(ModifiedRepairRefused, L"Reparo recusado: há arquivo, atalho ou registro alterado/desconhecido. Foi preservado; somente ausências podem ser recriadas.", L"Repair refused: a file, shortcut or registration is modified/unknown. It was preserved; only missing objects may be recreated.") \
 X(RemovalMarkerMismatch, L"O marcador de remoção não corresponde ao recibo. Foi preservado.", L"The removal marker does not match the receipt. It was preserved.") \
 X(UnsafeConfigMigration, L"A configuração não pôde ser migrada com segurança. Originais/backup e journal foram preservados; execute novamente para recuperar.", L"Configuration could not be migrated safely. Originals/backup and journal were preserved; run again to recover.") \
 X(CannotVerifyRecoveredConfig, L"Não foi possível verificar a configuração recuperada.", L"Could not verify recovered configuration.") \
 X(CannotRepairMissingFile, L"O reparo não pôde recriar uma ausência. Somente os novos arquivos inalterados serão revertidos.", L"Repair could not recreate a missing object. Only newly created unchanged files will be rolled back.") \
 X(CannotRepairShortcut, L"O reparo não pôde criar um atalho ausente. Nenhum atalho existente foi substituído.", L"Repair could not create a missing shortcut. No existing shortcut was replaced.") \
 X(CannotRepairRegistration, L"Não foi possível recriar o registro ausente; os novos arquivos serão revertidos.", L"Could not recreate missing registration; newly created files will be rolled back.") \
 X(RepairVerificationFailed, L"A verificação do reparo falhou; apenas novas criações inalteradas serão revertidas.", L"Repair verification failed; only newly created unchanged objects will be rolled back.") \
 X(ReinstallConfirmationInterrupted, L"Os arquivos foram reinstalados, mas a confirmação final foi interrompida. Execute novamente.", L"Files were reinstalled, but final confirmation was interrupted. Run again.") \
 X(RepairComplete, L"Instalação recuperada. Apenas arquivos/atalhos ausentes foram recriados; edições e dados pessoais foram preservados.", L"Installation recovered. Only missing files/shortcuts were recreated; edits and personal data were preserved.") \
 X(AlreadyInstalled, L"O modo janela já está instalado. A configuração existente foi mantida.", L"The windowed patch is already installed. Existing configuration was kept.") \
 X(CannotInspectInstallFolder, L"Não foi possível verificar a pasta de instalação.", L"Could not inspect the installation folder.") \
 X(CannotCreateInstallFolder, L"Não foi possível criar uma pasta exclusiva para o modo janela.", L"Could not create an exclusive folder for the windowed patch.") \
 X(CannotWriteOwnFiles, L"Não foi possível gravar os arquivos próprios. A instalação foi cancelada sem sobrescrever arquivos existentes.", L"Could not write the patch's own files. Installation was canceled without overwriting existing files.") \
 X(CannotCopyManager, L"Não foi possível copiar o gerenciador de instalação.", L"Could not copy the installation manager.") \
 X(CannotCreateShortcuts, L"Não foi possível criar os atalhos. Um atalho já existente nunca será substituído.", L"Could not create shortcuts. An existing shortcut is never replaced.") \
 X(CannotWriteReceipt, L"Não foi possível gravar o recibo de propriedade da instalação.", L"Could not write the installation ownership receipt.") \
 X(CannotRegisterUninstall, L"Não foi possível registrar a remoção para este usuário. Um registro preexistente não foi substituído.", L"Could not register uninstallation for this user. A pre-existing registration was not replaced.") \
 X(FinalVerificationFailed, L"A verificação final falhou. Somente arquivos próprios ainda inalterados podem ser revertidos.", L"Final verification failed. Only the patch's own unchanged files may be rolled back.") \
 X(InstallComplete, L"Modo janela instalado. Use o atalho Lake Controller - Janela. O Lake original não foi alterado nem iniciado.", L"Windowed patch installed. Use the Lake Controller - Janela shortcut. The original Lake application was not changed or launched.") \
 X(InvalidOwnFileIdentity, L"A identificação dos arquivos próprios é inválida.", L"The identity of the patch's own files is invalid.") \
 X(InvalidRemovalFolders, L"As pastas de remoção são inválidas ou se sobrepõem.", L"Uninstall folders are invalid or overlap.") \
 X(PendingUpgradeBeforeRemoval, L"Há atualização/rollback pendente. Recupere com --upgrade ou --rollback antes de remover.", L"An upgrade/rollback is pending. Recover with --upgrade or --rollback before uninstalling.") \
 X(CannotCheckRemovalManager, L"Não foi possível verificar o local do gerenciador de remoção.", L"Could not check the uninstall manager's location.") \
 X(ExternalRemovalRequired, L"A remoção deve ser executada pela cópia temporária do instalador, fora da pasta instalada.", L"Uninstallation must run from the installer's temporary copy, outside the installed folder.") \
 X(CannotCheckWindowsShortcuts, L"Não foi possível verificar os atalhos do Windows.", L"Could not check Windows shortcuts.") \
 X(CannotPinRemovalFolders, L"Não foi possível proteger as pastas contra alterações durante a remoção. Nada foi removido.", L"Could not protect folders against modification during uninstallation. Nothing was removed.") \
 X(InvalidRemovalReceipt, L"A remoção foi recusada: recibo de propriedade inválido ou caminho inseguro. Nada foi removido.", L"Uninstallation refused: invalid ownership receipt or unsafe path. Nothing was removed.") \
 X(UnknownRemovalMarker, L"O marcador de remoção é desconhecido e foi preservado. Nada foi removido.", L"The removal marker is unknown and was preserved. Nothing was removed.") \
 X(PendingConfigMigration, L"Há uma migração de configuração pendente. O backup e os journals foram mantidos; recupere antes de remover.", L"A configuration migration is pending. Backup and journals were kept; recover before uninstalling.") \
 X(CannotPinRemovalFiles, L"Não foi possível proteger os arquivos para remoção. Nada foi removido.", L"Could not protect files for uninstallation. Nothing was removed.") \
 X(InstallationChanged, L"A instalação mudou durante a verificação. Nada foi removido.", L"The installation changed during verification. Nothing was removed.") \
 X(CannotWriteRemovalJournal, L"Não foi possível gravar o journal de remoção. Nada foi removido.", L"Could not write the removal journal. Nothing was removed.") \
 X(RemovalBeforeDeletionInterrupted, L"Remoção interrompida antes das exclusões. Execute novamente para retomar.", L"Uninstallation was interrupted before deletion. Run again to resume.") \
 X(PartialRemovalInterrupted, L"Remoção parcial: recibo, journal e gerenciador foram mantidos. Execute novamente; ausências não bloqueiam a recuperação.", L"Partial uninstallation: receipt, journal and manager were kept. Run again; missing objects do not block recovery.") \
 X(CannotVerifyRemovalRegistration, L"O registro não pôde ser verificado. O gerenciador e o journal foram mantidos.", L"Registration could not be verified. The manager and journal were kept.") \
 X(RemovalRegistrationPreserved, L"O registro de remoção foi preservado; o gerenciador permite tentar novamente.", L"Uninstall registration was preserved; the manager allows another attempt.") \
 X(FinalRemovalInterrupted, L"A etapa final da remoção foi interrompida. Recibo e journal permitem repetir com segurança.", L"The final uninstall step was interrupted. Receipt and journal allow a safe retry.") \
 X(PartialRemovalComplete, L"Modo janela removido parcialmente: arquivos/atalhos/registro alterados foram preservados. Recibo e journal permitem nova tentativa ou reinstalação compatível.", L"Windowed patch partially removed: modified files/shortcuts/registration were preserved. Receipt and journal allow retry or compatible reinstallation.") \
 X(RemovalComplete, L"Modo janela removido. INI editado, logs e arquivos pessoais foram preservados. O recibo e o marcador permanecem para reinstalação/recuperação; o Lake original não foi alterado.", L"Windowed patch removed. Edited INI, logs and personal files were preserved. Receipt and marker remain for reinstallation/recovery; the original Lake application was not changed.")

#define LAKE_SETUP_ENUM(id, pt, en) id,
enum class TextId { LAKE_SETUP_TEXTS(LAKE_SETUP_ENUM) Count };
#undef LAKE_SETUP_ENUM
struct BilingualText { const wchar_t* portuguese; const wchar_t* english; };
#define LAKE_SETUP_ENTRY(id, pt, en) {pt, en},
inline constexpr BilingualText kSetupTexts[] = { LAKE_SETUP_TEXTS(LAKE_SETUP_ENTRY) };
#undef LAKE_SETUP_ENTRY
#undef LAKE_SETUP_TEXTS
inline constexpr std::size_t kSetupTextCount = static_cast<std::size_t>(TextId::Count);
static_assert(sizeof(kSetupTexts) / sizeof(kSetupTexts[0]) == kSetupTextCount, "Complete bilingual catalog");
inline const wchar_t* Text(TextId id, Language language) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= kSetupTextCount) return language == Language::Portuguese ? L"Mensagem desconhecida." : L"Unknown message.";
    return language == Language::Portuguese ? kSetupTexts[index].portuguese : kSetupTexts[index].english;
}
} // namespace lakefix::setup
