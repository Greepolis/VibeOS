# The NT system services, one by one

Every `Nt*` function that is a system call in Wine 9.0's `ntdll.spec` (the ones
marked `-syscall`; the `Zw*` names are the same calls) - 237 of them - and the
step of [windows.md](windows.md) that implements each, or why none does. The
counterpart of [syscalls.md](syscalls.md) for Linux, with one difference that
the plan explains: the numbers are ours, because our own `ntdll` is the only
caller, so this list names services rather than numbers. The rows get their
numbers in `kernel/abi/nt/` when the step that implements them lands, in this
order.

A step owns a call when it implements every information class the step's tests
use; a call that starts small (`NtWriteFile` to the console in W1,
`NtQueryInformationProcess` with the classes a process's start needs) is listed
where it first appears and grows in later steps. Wine's list is the source
because it is what Wine's own `ntdll` exports as system calls - the services a
program built against Wine's headers can reach - and the oracle is Wine's tests.

| Group | Calls |
|---|---|
| W1 | 6 |
| W2 | 45 |
| W3 | 23 |
| W4 | 32 |
| W5 | 18 |
| W6 | 12 |
| W7 | 27 |
| The registry | 26 |
| Tokens and security descriptors | 18 |
| Not planned | 30 |
| **Total** | **237** |

## W1 - a process starts and prints

What our own `hello` and `ntdll`'s start-up need. `NtWriteFile` to the console only; W4 makes it a file call.

`NtQueryInformationProcess`, `NtQueryPerformanceCounter`, `NtQuerySystemInformation`, `NtQuerySystemTime`, `NtTerminateProcess`, `NtWriteFile`

## W2 - objects, handles and waits

The object manager: namespace directories and links, the four synchronisation objects, timers, keyed events and the waits over all of them.

`NtAlertThreadByThreadId`, `NtCancelTimer`, `NtClearEvent`, `NtClose`, `NtCompareObjects`, `NtCreateDirectoryObject`, `NtCreateEvent`, `NtCreateKeyedEvent`, `NtCreateMutant`, `NtCreateSemaphore`, `NtCreateSymbolicLinkObject`, `NtCreateTimer`, `NtDelayExecution`, `NtDuplicateObject`, `NtMakeTemporaryObject`, `NtOpenDirectoryObject`, `NtOpenEvent`, `NtOpenKeyedEvent`, `NtOpenMutant`, `NtOpenSemaphore`, `NtOpenSymbolicLinkObject`, `NtOpenTimer`, `NtPulseEvent`, `NtQueryDirectoryObject`, `NtQueryEvent`, `NtQueryMutant`, `NtQueryObject`, `NtQuerySemaphore`, `NtQuerySymbolicLinkObject`, `NtQueryTimer`, `NtQueryTimerResolution`, `NtReleaseKeyedEvent`, `NtReleaseMutant`, `NtReleaseSemaphore`, `NtResetEvent`, `NtSetEvent`, `NtSetInformationObject`, `NtSetTimer`, `NtSetTimerResolution`, `NtSignalAndWaitForSingleObject`, `NtWaitForAlertByThreadId`, `NtWaitForKeyedEvent`, `NtWaitForMultipleObjects`, `NtWaitForSingleObject`, `NtYieldExecution`

## W3 - memory

Private memory, sections and views. The cross-process forms of the virtual-memory calls arrive with W6, when there is a second process to name.

`NtAllocateVirtualMemory`, `NtAllocateVirtualMemoryEx`, `NtAreMappedFilesTheSame`, `NtCreateSection`, `NtFlushInstructionCache`, `NtFlushProcessWriteBuffers`, `NtFlushVirtualMemory`, `NtFreeVirtualMemory`, `NtGetWriteWatch`, `NtLockVirtualMemory`, `NtMapViewOfSection`, `NtMapViewOfSectionEx`, `NtOpenSection`, `NtProtectVirtualMemory`, `NtQuerySection`, `NtQueryVirtualMemory`, `NtReadVirtualMemory`, `NtResetWriteWatch`, `NtSetInformationVirtualMemory`, `NtUnlockVirtualMemory`, `NtUnmapViewOfSection`, `NtUnmapViewOfSectionEx`, `NtWriteVirtualMemory`

## W4 - files, pipes and completion ports

Files over the VFS, with `\??\C:\` and case-blind names; named pipes and mailslots are files to NT, and so is asynchronous I/O's completion port.

`NtCancelIoFile`, `NtCancelIoFileEx`, `NtCancelSynchronousIoFile`, `NtCreateFile`, `NtCreateIoCompletion`, `NtCreateMailslotFile`, `NtCreateNamedPipeFile`, `NtDeleteFile`, `NtDeviceIoControlFile`, `NtFlushBuffersFile`, `NtFsControlFile`, `NtLockFile`, `NtNotifyChangeDirectoryFile`, `NtOpenFile`, `NtOpenIoCompletion`, `NtQueryAttributesFile`, `NtQueryDirectoryFile`, `NtQueryEaFile`, `NtQueryFullAttributesFile`, `NtQueryInformationFile`, `NtQueryIoCompletion`, `NtQueryVolumeInformationFile`, `NtReadFile`, `NtReadFileScatter`, `NtRemoveIoCompletion`, `NtRemoveIoCompletionEx`, `NtSetEaFile`, `NtSetInformationFile`, `NtSetIoCompletion`, `NtSetVolumeInformationFile`, `NtUnlockFile`, `NtWriteFileGather`

## W5 - threads, APCs and exceptions

`NtAlertResumeThread`, `NtAlertThread`, `NtContinue`, `NtCreateThread`, `NtCreateThreadEx`, `NtGetContextThread`, `NtGetCurrentProcessorNumber`, `NtGetNextThread`, `NtOpenThread`, `NtQueryInformationThread`, `NtQueueApcThread`, `NtRaiseException`, `NtResumeThread`, `NtSetContextThread`, `NtSetInformationThread`, `NtSuspendThread`, `NtTerminateThread`, `NtTestAlert`

## W6 - processes and jobs

`NtAssignProcessToJobObject`, `NtCreateJobObject`, `NtCreateUserProcess`, `NtIsProcessInJob`, `NtOpenJobObject`, `NtOpenProcess`, `NtQueryInformationJobObject`, `NtResumeProcess`, `NtSetInformationJobObject`, `NtSetInformationProcess`, `NtSuspendProcess`, `NtTerminateJobObject`

## W7 - the rest of what kernel32's tests ask

System information in full, time, locale and NLS data, atoms, identifiers, and the debugger interface `kernel32:debugger` measures.

`NtAddAtom`, `NtAllocateLocallyUniqueId`, `NtAllocateUuids`, `NtCreateDebugObject`, `NtDebugActiveProcess`, `NtDebugContinue`, `NtDeleteAtom`, `NtFindAtom`, `NtGetNlsSectionPtr`, `NtInitializeNlsFiles`, `NtPowerInformation`, `NtQueryDefaultLocale`, `NtQueryDefaultUILanguage`, `NtQueryInformationAtom`, `NtQueryInstallUILanguage`, `NtQueryLicenseValue`, `NtQuerySystemEnvironmentValue`, `NtQuerySystemEnvironmentValueEx`, `NtQuerySystemInformationEx`, `NtRemoveProcessDebug`, `NtSetDefaultLocale`, `NtSetDefaultUILanguage`, `NtSetInformationDebugObject`, `NtSetSystemInformation`, `NtSetSystemTime`, `NtSetThreadExecutionState`, `NtWaitForDebugEvent`

## The registry - a service, not the kernel's

The plan keeps the registry out of the kernel: these rows forward to a registry service. Needed when `kernel32`'s tests reach `advapi32`; planned then.

`NtCreateKey`, `NtCreateKeyTransacted`, `NtDeleteKey`, `NtDeleteValueKey`, `NtEnumerateKey`, `NtEnumerateValueKey`, `NtFlushKey`, `NtLoadKey`, `NtLoadKey2`, `NtLoadKeyEx`, `NtNotifyChangeKey`, `NtNotifyChangeMultipleKeys`, `NtOpenKey`, `NtOpenKeyEx`, `NtOpenKeyTransacted`, `NtOpenKeyTransactedEx`, `NtQueryKey`, `NtQueryMultipleValueKey`, `NtQueryValueKey`, `NtRenameKey`, `NtReplaceKey`, `NtRestoreKey`, `NtSaveKey`, `NtSetInformationKey`, `NtSetValueKey`, `NtUnloadKey`

## Tokens and security descriptors - with the security plan

The roadmap's fourth item. Until then a process's token is a fixed answer - one user, an administrator - for whatever a test asks of it, and these calls give it.

`NtAccessCheck`, `NtAccessCheckAndAuditAlarm`, `NtAdjustGroupsToken`, `NtAdjustPrivilegesToken`, `NtCreateLowBoxToken`, `NtCreateToken`, `NtDuplicateToken`, `NtFilterToken`, `NtImpersonateAnonymousToken`, `NtOpenProcessToken`, `NtOpenProcessTokenEx`, `NtOpenThreadToken`, `NtOpenThreadTokenEx`, `NtPrivilegeCheck`, `NtQueryInformationToken`, `NtQuerySecurityObject`, `NtSetInformationToken`, `NtSetSecurityObject`

## Not planned

Each refused with `STATUS_NOT_IMPLEMENTED` and counted, as an unassigned Linux number is. Reasons per group: LPC ports are the transport of Windows' own subsystem services, which this design does not have; transactions (KTM) and drivers have no counterpart; WoW64 and LDT entries are 32-bit programs, which the plan leaves out; `NtCallbackReturn` is the window system's return from a user-mode callback; the rest administer a Windows machine.

`NtAcceptConnectPort`, `NtCallbackReturn`, `NtCommitTransaction`, `NtCompleteConnectPort`, `NtConnectPort`, `NtCreatePagingFile`, `NtCreatePort`, `NtCreateTransaction`, `NtDisplayString`, `NtInitiatePowerAction`, `NtListenPort`, `NtLoadDriver`, `NtRaiseHardError`, `NtRegisterThreadTerminatePort`, `NtReplyWaitReceivePort`, `NtRequestWaitReplyPort`, `NtRollbackTransaction`, `NtSecureConnectPort`, `NtSetDebugFilterState`, `NtSetIntervalProfile`, `NtSetLdtEntries`, `NtShutdownSystem`, `NtSystemDebugControl`, `NtTraceControl`, `NtUnloadDriver`, `NtWow64AllocateVirtualMemory64`, `NtWow64GetNativeSystemInformation`, `NtWow64IsProcessorFeaturePresent`, `NtWow64ReadVirtualMemory64`, `NtWow64WriteVirtualMemory64`
