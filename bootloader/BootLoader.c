/**
*       在UEFI世界內，所謂BootLoader只是一個Binary Executable，透過UEFI的API去獲取系統訊息，並加載內核image到內存的程序
*/

#include <Uefi.h>
#include <Library/UefiLib.h>

#define COUNT_PAGES(bytes)  (bytes+4095)>>12


// Define data structure to be passed to Kernel
// struct  {
    
// };

// struct EFI_GRAPHICS_OUTPUT_INFORMATION {
//     unsigned int HorizontalResolution;
//     unsigned int VerticalResolution;
//     unsigned int PixelsPerScanLine;
//     unsigned int FrameBufferBase;
//     unsigned int FrameBufferSize;
// };


// UEFI程序入口，ImageHandle用來指代本程序，SystemTable為UEFI firmware準備的一切API入口
EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) { 
    EFI_BOOT_SERVICES* gBS = SystemTable->BootServices;
    // 查看內存佈局需要用到UEFI Boot Service中的GetMemoryMap()

    /**
    typedef
    EFI_STATUS
    (EFIAPI *EFI_GET_MEMORY_MAP)(
    IN OUT UINTN                       *MemoryMapSize,
    OUT    EFI_MEMORY_DESCRIPTOR       *MemoryMap,
    OUT    UINTN                       *MapKey,
    OUT    UINTN                       *DescriptorSize,
    OUT    UINT32                      *DescriptorVersion
    );
    */
    UINTN MemoryMapSize = 0;
    EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;
    // 首先填寫MemoryMapSize = 0, GetMemoryMap變成read only，只會刷新MemoryMapSize
    EFI_STATUS GetMemoryMapStatus = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(GetMemoryMapStatus!=EFI_BUFFER_TOO_SMALL){
        return GetMemoryMapStatus;
    }

    /**
    typedef
    EFI_STATUS
    (EFIAPI *EFI_ALLOCATE_PAGES)(
      IN     EFI_ALLOCATE_TYPE            Type,
      IN     EFI_MEMORY_TYPE              MemoryType,
      IN     UINTN                        Pages,
      IN OUT EFI_PHYSICAL_ADDRESS         *Memory
      );
    */

    // 在Boot Service Data Segment中分配Memory Page裝載Memory Map Descriptors
    EFI_PHYSICAL_ADDRESS pages;
    EFI_STATUS AllocateMemoryPageDescriptorStatus = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, COUNT_PAGES(MemoryMapSize), &pages);
    if(AllocateMemoryPageDescriptorStatus != EFI_SUCCESS){
        return AllocateMemoryPageDescriptorStatus;
    }
    MemoryMap = (EFI_MEMORY_DESCRIPTOR*)pages;

    // 今次帶著剛分配的Memory Page 作為Buffer去重新獲取Memory Map
    MemoryMapSize = COUNT_PAGES(MemoryMapSize) << 12;
    GetMemoryMapStatus = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(GetMemoryMapStatus!=EFI_SUCCESS){
        return GetMemoryMapStatus;
    }

    /**
    typedef struct {
        UINT32                  Type;
        EFI_PHYSICAL_ADDRESS    PhysicalStart;
        EFI_VIRTUAL_ADDRESS     VirtualStart;
        UINT64                  NumberOfPages;
        UINT64                  Attribute;
    } EFI_MEMORY_DESCRIPTOR;
    */
    EFI_MEMORY_DESCRIPTOR* current_desc = (EFI_MEMORY_DESCRIPTOR*)MemoryMap;
    // MemoryMap + MemoryMapSize = MemoryMap 結束處，所以當current_desc 小於結束地址，就要不斷增加
    // 伏位: UEFI spec寫明唔可以 current_desc ++， 因為sizeof(EFI_MEMORY_DESCRIPTOR) 未必係DescriptorSize
    while((char*)current_desc < (char*)MemoryMap+ MemoryMapSize){
        Print(L"Mem [Type:%d] [Phys:%x to %x] [VirStart:%x] [NoOfPages:%d] [Attribute:%x]\n", 
            current_desc->Type, 
            current_desc->PhysicalStart, 
            current_desc->PhysicalStart + 4096 * current_desc->NumberOfPages - 1, 
            current_desc->VirtualStart, 
            current_desc->NumberOfPages, 
            current_desc->Attribute
        );
        current_desc = (EFI_MEMORY_DESCRIPTOR*)((char*)current_desc + DescriptorSize);
    }
    // 釋放MemoryMap空間
    // 伏位：AllocatePages 對應 FreePages，之前用錯FreePool會導致hang機。
    gBS->FreePages((EFI_PHYSICAL_ADDRESS)MemoryMap, COUNT_PAGES(MemoryMapSize));

    Print(L"Successfully obtained Memory Map.\n");

    // 查看顯示模式需要用到UEFI中的Graphics Output Protocol
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;

    // LocateProtocol的作用是返回系統中所有支援此Protocol的Handle(設備), 所以gop是一個陣列
    gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, 0, (VOID **) &gop);
    UINTN InfoSize = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info = 0;
    for(int i = 0; i < gop->Mode->MaxMode; i++) {
        gop->QueryMode(gop, i, &InfoSize, &Info);
        Print(L"(%d), Resolution: (%d x %d)\n", i, Info->HorizontalResolution, Info->VerticalResolution);
        gBS->FreePool(Info);
    }
    Print(L"(Current Mode) Resolution: (%d x %d), FrameBuffer (%x, size: %d)\n", 
        gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution, 
        gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize
    );
    gBS->CloseProtocol(gop, &gEfiGraphicsOutputProtocolGuid, ImageHandle, NULL);
    
    return EFI_SUCCESS;
}