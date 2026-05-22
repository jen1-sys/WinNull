#include <fltKernel.h>

#pragma comment(lib, "FltMgr.lib")

extern "C" DRIVER_INITIALIZE DriverEntry;

// =========================
// WinNull CDO (\\.\WinNull)
// =========================

#define WINNULL_DEVICE_NAME L"\\Device\\WinNull"
#define WINNULL_DOS_NAME    L"\\DosDevices\\WinNull"   // user-mode opens \\\\.\\WinNull

static PDEVICE_OBJECT gControlDeviceObject = nullptr;

// =========================
// Minifilter globals/rules
// =========================

static PFLT_FILTER gFilterHandle = nullptr;

// Match rules:
//   - path contains "\WinNull\"
//   - or path ends with ".wnull"
static const UNICODE_STRING gRuleFolder = RTL_CONSTANT_STRING(L"\\WinNull\\");
static const UNICODE_STRING gRuleSuffix = RTL_CONSTANT_STRING(L".wnull");

// CDO open/unload coordination
static volatile LONG gCdoOpenHandleCount = 0;
static volatile LONG gUnloadInProgress   = 0;

// =========================
// Stream-handle context cache (perf win)
// =========================

#define WINNULL_TAG 'lNuW'

typedef struct _WINNULL_STREAMHANDLE_CONTEXT
{
    BOOLEAN Intercept; // TRUE => null read/write + metadata virtualization
} WINNULL_STREAMHANDLE_CONTEXT, *PWINNULL_STREAMHANDLE_CONTEXT;

static VOID
WinNullContextCleanup(
    _In_ PFLT_CONTEXT Context,
    _In_ FLT_CONTEXT_TYPE ContextType
)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ContextType);
}

// ---------- Common helpers ----------

static __forceinline NTSTATUS
WinNullCompleteIrp(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information
)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static __forceinline FLT_PREOP_CALLBACK_STATUS
WinNullCompletePreOp(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information
)
{
    Data->IoStatus.Status = Status;
    Data->IoStatus.Information = Information;
    return FLT_PREOP_COMPLETE;
}

// ---------- CDO dispatch (direct sink path \\.\WinNull) ----------

static NTSTATUS
WinNullCdoDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MajorFunction)
    {
    case IRP_MJ_CREATE:
    {
        // Block new opens while unload is in progress.
        if (InterlockedCompareExchange(&gUnloadInProgress, 0, 0) != 0) {
            return WinNullCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
        }

        // Count open handles.
        InterlockedIncrement(&gCdoOpenHandleCount);

        // Close race: unload may have started just after increment.
        if (InterlockedCompareExchange(&gUnloadInProgress, 0, 0) != 0) {
            InterlockedDecrement(&gCdoOpenHandleCount);
            return WinNullCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
        }

        return WinNullCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    case IRP_MJ_CLOSE:
        // Decrement handle count on CLOSE (not CLEANUP).
        InterlockedDecrement(&gCdoOpenHandleCount);
        return WinNullCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MJ_CLEANUP:
    case IRP_MJ_FLUSH_BUFFERS:
        return WinNullCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MJ_READ:
        // /dev/null semantics: successful EOF
        return WinNullCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case IRP_MJ_WRITE:
        // Discard data, report all bytes written
        return WinNullCompleteIrp(Irp, STATUS_SUCCESS, stack->Parameters.Write.Length);

    default:
        return WinNullCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS
WinNullCreateControlDevice(
    _In_ PDRIVER_OBJECT DriverObject
)
{
    UNICODE_STRING devName = RTL_CONSTANT_STRING(WINNULL_DEVICE_NAME);
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(WINNULL_DOS_NAME);

    PDEVICE_OBJECT deviceObject = nullptr;

    NTSTATUS status = IoCreateDevice(
        DriverObject,
        0,
        &devName,
        FILE_DEVICE_NULL,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    // No buffering flags needed (we never touch user buffers)
    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    gControlDeviceObject = deviceObject;
    return STATUS_SUCCESS;
}

static VOID
WinNullDeleteControlDevice()
{
    UNICODE_STRING symLink = RTL_CONSTANT_STRING(WINNULL_DOS_NAME);

    (void)IoDeleteSymbolicLink(&symLink);

    if (gControlDeviceObject != nullptr) {
        IoDeleteDevice(gControlDeviceObject);
        gControlDeviceObject = nullptr;
    }
}

// ---------- Path matching helpers (minifilter side) ----------

static BOOLEAN
WinNullContainsInsensitive(
    _In_ PCUNICODE_STRING Haystack,
    _In_ PCUNICODE_STRING Needle
)
{
    if (!Haystack || !Needle) return FALSE;
    if (!Haystack->Buffer || !Needle->Buffer) return FALSE;
    if (Needle->Length == 0) return TRUE;
    if (Haystack->Length < Needle->Length) return FALSE;

    const USHORT hChars = (USHORT)(Haystack->Length / sizeof(WCHAR));
    const USHORT nChars = (USHORT)(Needle->Length / sizeof(WCHAR));

    for (USHORT i = 0; i + nChars <= hChars; ++i) {
        BOOLEAN match = TRUE;
        for (USHORT j = 0; j < nChars; ++j) {
            if (RtlUpcaseUnicodeChar(Haystack->Buffer[i + j]) !=
                RtlUpcaseUnicodeChar(Needle->Buffer[j])) {
                match = FALSE;
                break;
            }
        }
        if (match) return TRUE;
    }

    return FALSE;
}

static __forceinline BOOLEAN
WinNullSuffixInsensitive(
    _In_ PCUNICODE_STRING Name,
    _In_ PCUNICODE_STRING Suffix
)
{
    return RtlSuffixUnicodeString(Suffix, Name, TRUE);
}

static BOOLEAN
WinNullShouldInterceptByName(
    _In_ PCUNICODE_STRING Name
)
{
    if (!Name || !Name->Buffer) return FALSE;

    if (WinNullContainsInsensitive(Name, &gRuleFolder)) return TRUE;
    if (WinNullSuffixInsensitive(Name, &gRuleSuffix))  return TRUE;

    return FALSE;
}

static BOOLEAN
WinNullShouldInterceptByDataNameQuery(
    _In_ PFLT_CALLBACK_DATA Data
)
{
    PFLT_FILE_NAME_INFORMATION nameInfo = nullptr;

    NTSTATUS status = FltGetFileNameInformation(
        Data,
        FLT_FILE_NAME_OPENED | FLT_FILE_NAME_QUERY_DEFAULT,
        &nameInfo
    );

    if (!NT_SUCCESS(status) || nameInfo == nullptr) {
        return FALSE; // fail-open
    }

    (void)FltParseFileNameInformation(nameInfo);
    const BOOLEAN intercept = WinNullShouldInterceptByName(&nameInfo->Name);

    FltReleaseFileNameInformation(nameInfo);
    return intercept;
}

static VOID
WinNullTrySetInterceptContext(
    _In_ PCFLT_RELATED_OBJECTS FltObjects
)
{
    PWINNULL_STREAMHANDLE_CONTEXT ctx = nullptr;

    NTSTATUS status = FltAllocateContext(
        FltObjects->Filter,
        FLT_STREAMHANDLE_CONTEXT,
        sizeof(WINNULL_STREAMHANDLE_CONTEXT),
        NonPagedPoolNx,
        (PFLT_CONTEXT*)&ctx
    );

    if (!NT_SUCCESS(status) || ctx == nullptr) {
        return;
    }

    ctx->Intercept = TRUE;

    (void)FltSetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        FLT_SET_CONTEXT_KEEP_IF_EXISTS,
        ctx,
        nullptr
    );

    FltReleaseContext(ctx);
}

static BOOLEAN
WinNullIsIntercepted(
    _In_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects
)
{
    // Fast path: cached per-handle decision
    PWINNULL_STREAMHANDLE_CONTEXT ctx = nullptr;

    NTSTATUS status = FltGetStreamHandleContext(
        FltObjects->Instance,
        FltObjects->FileObject,
        (PFLT_CONTEXT*)&ctx
    );

    if (NT_SUCCESS(status) && ctx != nullptr) {
        const BOOLEAN intercept = (ctx->Intercept != FALSE);
        FltReleaseContext(ctx);
        return intercept;
    }

    // Slow path (rare): no context (e.g. handle existed before filter load)
    if (WinNullShouldInterceptByDataNameQuery(Data)) {
        WinNullTrySetInterceptContext(FltObjects);
        return TRUE;
    }

    return FALSE;
}

// ---------- CREATE callbacks for caching ----------

static FLT_PREOP_CALLBACK_STATUS
WinNullPreCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}

static FLT_POSTOP_CALLBACK_STATUS
WinNullPostCreate(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    if (!NT_SUCCESS(Data->IoStatus.Status)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (FlagOn(Data->Iopb->OperationFlags, SL_OPEN_PAGING_FILE)) {
        return FLT_POSTOP_FINISHED_PROCESSING;
    }

    if (WinNullShouldInterceptByDataNameQuery(Data)) {
        WinNullTrySetInterceptContext(FltObjects);
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

// ---------- Minifilter callbacks ----------

static FLT_PREOP_CALLBACK_STATUS
WinNullPreRead(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (!WinNullIsIntercepted(Data, FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    // /dev/null semantics: 0 bytes
    return WinNullCompletePreOp(Data, STATUS_SUCCESS, 0);
}

static FLT_PREOP_CALLBACK_STATUS
WinNullPreWrite(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (!WinNullIsIntercepted(Data, FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    const ULONG len = Data->Iopb->Parameters.Write.Length;
    return WinNullCompletePreOp(Data, STATUS_SUCCESS, (ULONG_PTR)len);
}

static FLT_PREOP_CALLBACK_STATUS
WinNullPreQueryInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (!WinNullIsIntercepted(Data, FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    const FILE_INFORMATION_CLASS fic =
        Data->Iopb->Parameters.QueryFileInformation.FileInformationClass;

    PVOID buffer = Data->Iopb->Parameters.QueryFileInformation.InfoBuffer;
    const ULONG length = Data->Iopb->Parameters.QueryFileInformation.Length;

    if (buffer == nullptr) {
        return WinNullCompletePreOp(Data, STATUS_INVALID_PARAMETER, 0);
    }

    __try
    {
        switch (fic)
        {
        case FileStandardInformation:
            if (length < sizeof(FILE_STANDARD_INFORMATION)) {
                return WinNullCompletePreOp(Data, STATUS_BUFFER_TOO_SMALL, 0);
            } else {
                auto* info = (PFILE_STANDARD_INFORMATION)buffer;
                RtlZeroMemory(info, sizeof(*info));
                info->NumberOfLinks = 1;
                info->Directory = FALSE;
                info->DeletePending = FALSE;
                return WinNullCompletePreOp(Data, STATUS_SUCCESS, sizeof(FILE_STANDARD_INFORMATION));
            }

        case FileNetworkOpenInformation:
            if (length < sizeof(FILE_NETWORK_OPEN_INFORMATION)) {
                return WinNullCompletePreOp(Data, STATUS_BUFFER_TOO_SMALL, 0);
            } else {
                auto* info = (PFILE_NETWORK_OPEN_INFORMATION)buffer;
                RtlZeroMemory(info, sizeof(*info));
                info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
                return WinNullCompletePreOp(Data, STATUS_SUCCESS, sizeof(FILE_NETWORK_OPEN_INFORMATION));
            }

        default:
            return FLT_PREOP_SUCCESS_NO_CALLBACK;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return WinNullCompletePreOp(Data, GetExceptionCode(), 0);
    }
}

static FLT_PREOP_CALLBACK_STATUS
WinNullPreSetInformation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Outptr_result_maybenull_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(CompletionContext);

    if (!WinNullIsIntercepted(Data, FltObjects)) {
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }

    const FILE_INFORMATION_CLASS fic =
        Data->Iopb->Parameters.SetFileInformation.FileInformationClass;

    switch (fic)
    {
    case FileEndOfFileInformation:
    case FileAllocationInformation:
        // Keep WinNull files effectively size 0:
        // claim success but do not let filesystem change EOF/allocation.
        return WinNullCompletePreOp(Data, STATUS_SUCCESS, 0);

        // Optional later (leave disabled for now unless needed):
        // case FileValidDataLengthInformation:
        //     return WinNullCompletePreOp(Data, STATUS_SUCCESS, 0);

    default:
        return FLT_PREOP_SUCCESS_NO_CALLBACK;
    }
}

// ---------- Unload ----------

static NTSTATUS
WinNullFilterUnload(
    _In_ FLT_FILTER_UNLOAD_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Flags);

    InterlockedExchange(&gUnloadInProgress, 1);

    if (InterlockedCompareExchange(&gCdoOpenHandleCount, 0, 0) != 0) {
        InterlockedExchange(&gUnloadInProgress, 0);
        return STATUS_DEVICE_BUSY;
    }

    WinNullDeleteControlDevice();

    if (gFilterHandle != nullptr) {
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = nullptr;
    }

    return STATUS_SUCCESS;
}

// ---------- Minifilter registration ----------

static const FLT_CONTEXT_REGISTRATION gContextRegistration[] =
{
    {
        FLT_STREAMHANDLE_CONTEXT,
        0,
        WinNullContextCleanup,
        sizeof(WINNULL_STREAMHANDLE_CONTEXT),
        WINNULL_TAG
    },
    { FLT_CONTEXT_END }
};

static const FLT_OPERATION_REGISTRATION gCallbacks[] =
{
    { IRP_MJ_CREATE,            0, WinNullPreCreate,           WinNullPostCreate },
    { IRP_MJ_READ,              0, WinNullPreRead,             nullptr },
    { IRP_MJ_WRITE,             0, WinNullPreWrite,            nullptr },
    { IRP_MJ_QUERY_INFORMATION, 0, WinNullPreQueryInformation, nullptr },
    { IRP_MJ_SET_INFORMATION,   0, WinNullPreSetInformation,   nullptr },
    { IRP_MJ_OPERATION_END }
};

static const FLT_REGISTRATION gFilterRegistration =
{
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    gContextRegistration,
    gCallbacks,
    WinNullFilterUnload,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

extern "C"
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    // Dispatch table for the control device object (CDO).
    // Minifilter I/O interception is handled through Filter Manager callbacks, not these entries.
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) {
        DriverObject->MajorFunction[i] = WinNullCdoDispatch;
    }

    // Minifilters use FilterUnloadCallback, not DriverUnload.
    DriverObject->DriverUnload = nullptr;

    NTSTATUS status = FltRegisterFilter(DriverObject, &gFilterRegistration, &gFilterHandle);
    if (!NT_SUCCESS(status)) {
        gFilterHandle = nullptr;
        return status;
    }

    // Create \\.\WinNull control device in the same binary.
    status = WinNullCreateControlDevice(DriverObject);
    if (!NT_SUCCESS(status)) {
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = nullptr;
        return status;
    }

    status = FltStartFiltering(gFilterHandle);
    if (!NT_SUCCESS(status)) {
        WinNullDeleteControlDevice();
        FltUnregisterFilter(gFilterHandle);
        gFilterHandle = nullptr;
        return status;
    }

    return STATUS_SUCCESS;
}
