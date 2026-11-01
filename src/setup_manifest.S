// Native COFF RT_MANIFEST resource for GNU/LLVM toolchains, without an external
// resource compiler. Keep its requestedExecutionLevel equal to setup.manifest.
// Explicit asInvoker prevents Windows' legacy setup-name elevation heuristics.
.section .rsrc,"dr"
.p2align 2
    .long 0, 0
    .short 0, 0, 0, 1
    .long 24, 0x80000018
    .long 0, 0
    .short 0, 0, 0, 1
    .long 1, 0x80000030
    .long 0, 0
    .short 0, 0, 0, 1
    .long 0x409, 72
    .rva manifest_data
    .long manifest_end - manifest_data, 0, 0
manifest_data:
    .ascii "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><assemblyIdentity version=\"0.6.0.0\" processorArchitecture=\"x86\" name=\"LakeWindowedFix.Setup\" type=\"win32\"/><trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges><requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/></requestedPrivileges></security></trustInfo></assembly>"
manifest_end:
.p2align 2
