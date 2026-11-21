/**
*       在UEFI世界內，所謂BootLoader只是一個Binary Executable，透過UEFI的API去獲取系統訊息，並加載內核image到內存的程序
*/

#include <Uefi.h>
#include <Library/UefiLib.h>

#define COUNT_PAGES(bytes)  (bytes+4095)>>12


// 先設定好會傳給kernel的數據結構，由於要跨binary傳數據，kernel和bootloader所用的編譯器和ABI稍有不同，就會有不同的padding behavior。
// 為穩陣，寫明不需要padding，用__attribute__((packed))
// C standard types: unsigned int, unsigned long的實際長度都depends on ABI，改用UEFI types
// 大原則：越explicit越死越好
struct EFI_GRAPHICS_OUTPUT_INFORMATION {
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelsPerScanLine;
    UINT64 FrameBufferBase;
    UINT64 FrameBufferSize;
} __attribute__((packed));

struct EFI_E820_MEMORY_DESCRIPTOR {
    UINT64 address;
    UINT64 length;
    UINT32 type;
} __attribute__((packed));

struct EFI_E820_MEMORY_DESCRIPTOR_INFORMATION {
    UINT32 E820_Entry_Count;
    struct EFI_E820_MEMORY_DESCRIPTOR E820_Entry[0];    // 將會是陣列首元素
} __attribute__((packed));

struct KERNEL_BOOT_PARAMETER_INFORMATION {
    struct EFI_GRAPHICS_OUTPUT_INFORMATION Graphics_Info;
    struct EFI_E820_MEMORY_DESCRIPTOR_INFORMATION E820_Info;
} __attribute__((packed));

// UEFI程序入口，ImageHandle用來指代本程序，SystemTable為UEFI firmware準備的一切API入口
EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) { 
    EFI_BOOT_SERVICES* gBS = SystemTable->BootServices;
    EFI_STATUS status;
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
    status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(status!=EFI_BUFFER_TOO_SMALL){
        return status;
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
    status = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, COUNT_PAGES(MemoryMapSize), &pages);
    if(status != EFI_SUCCESS){
        return status;
    }
    MemoryMap = (EFI_MEMORY_DESCRIPTOR*)pages;

    // 今次帶著剛分配的Memory Page 作為Buffer去重新獲取Memory Map
    MemoryMapSize = COUNT_PAGES(MemoryMapSize) << 12;
    status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(status!=EFI_SUCCESS){
        return status;
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
    while((UINT8*)current_desc < (UINT8*)MemoryMap+ MemoryMapSize){
        Print(L"Mem [Type:%d] [Phys:%lx to %lx] [VirStart:%lx] [NoOfPages:%ld] [Attribute:%lx]\n", 
            current_desc->Type, 
            current_desc->PhysicalStart, 
            current_desc->PhysicalStart + (current_desc->NumberOfPages << 12) - 1, 
            current_desc->VirtualStart, 
            current_desc->NumberOfPages, 
            current_desc->Attribute
        );
        current_desc = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)current_desc + DescriptorSize);
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
    
    // 下個任務是要將kernel.bin讀入內存，先假設kernel.bin必定位於bootloader所在分區內。

    // 首先獲取當前Image的資訊，可以使用LoadedImageProtocol。
    // HandleProtocol 用於查看某特定Handle是否支援某特定Protocol。
    EFI_LOADED_IMAGE_PROTOCOL* loadedImage;
    status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, &loadedImage);
    if(status!=EFI_SUCCESS){
        return status;
    }

    // 查看設備Handle是否支援FAT
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;
    status = gBS->HandleProtocol(loadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, &fs);
    if(status!=EFI_SUCCESS){
        return status;
    }

    // 確認支援FAT後，嘗試打開根目錄 (類似Shell中運行fs0:)
    /**
    @retval EFI_SUCCESS          The device was opened.
    @retval EFI_UNSUPPORTED      This volume does not support the requested file system type.
    @retval EFI_NO_MEDIA         The device has no medium.
    @retval EFI_DEVICE_ERROR     The device reported an error.
    @retval EFI_VOLUME_CORRUPTED The file system structures are corrupted.
    @retval EFI_ACCESS_DENIED    The service denied access to the file.
    @retval EFI_OUT_OF_RESOURCES The volume was not opened due to lack of resources.
    @retval EFI_MEDIA_CHANGED    The device has a different medium in it or the medium is no
                                longer supported. Any existing file handles for this volume are
                                no longer valid. To access the files on the new medium, the
                                volume must be reopened with OpenVolume().
    **/
    EFI_FILE_PROTOCOL* fsroot;
    status = fs->OpenVolume(fs, &fsroot);
    if(status!=EFI_SUCCESS){
        return status;
    }
    
    // 開啟檔案
    EFI_FILE_HANDLE kernelFile; // EFI_FILE_PROTOCOL**
    status = fsroot->Open(fsroot, &kernelFile, (CHAR16*)L"kernel.bin", EFI_FILE_MODE_READ, 0); // Attribute = 0 is for create file only
    if(status!=EFI_SUCCESS){
        Print(L"Cannot open kernel.bin\n");
        return status;
    }

    // 拿取檔案資料
    /**
    typedef
    EFI_STATUS
    (EFIAPI *EFI_FILE_GET_INFO)(
        IN EFI_FILE_PROTOCOL        *This,
        IN EFI_GUID                 *InformationType,
        IN OUT UINTN                *BufferSize,
        OUT VOID                    *Buffer
    );
     */
    UINTN fileInfoSize = 0;
    EFI_FILE_INFO* fileinfo = NULL;
    // 試大小
    status = fsroot->GetInfo(fsroot, &gEfiFileInfoGuid, &fileInfoSize, fileinfo);
    if(status!=EFI_SUCCESS){
        return status;
    }
    // 分配Pool，LoaderData會在Kernel讀完參數後才回收
    status = gBS->AllocatePool(EfiLoaderData, fileInfoSize, (VOID*) fileinfo);
    if(status != EFI_SUCCESS){
        return status;
    }
    status = fsroot->GetInfo(fsroot, &gEfiFileInfoGuid, &fileInfoSize, fileinfo);
    if(status != EFI_SUCCESS){
        return status;
    }
    UINT64 KERNEL_BIN_SIZE = fileinfo->FileSize;
    gBS->FreePool(fileinfo);

    // 分配內存頁以裝載kernel.bin，需要裝到特定address上
    // 選擇0x100000作為內核開始位置，因為多數UEFI firmware都會在這個地址留下幾MB空間。
    // EfiLoaderCode： UEFI固件完全唔會觸碰、唔會移動、亦唔會重定位。
    EFI_PHYSICAL_ADDRESS pages = 0x100000;
    status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, COUNT_PAGES(KERNEL_BIN_SIZE), &pages);
    if(status != EFI_SUCCESS){
        return status;
    }
    
    /**
    typedef
    EFI_STATUS
    (EFIAPI *EFI_FILE_READ)(
    IN EFI_FILE_PROTOCOL        *This,
    IN OUT UINTN                *BufferSize,
    OUT VOID                    *Buffer
    );
    */
    status = kernelFile->Read(kernelFile, &KERNEL_BIN_SIZE, (VOID*) pages);
    if(status!=EFI_SUCCESS){
        Print(L"Cannot read kernel.bin into memory\n");
        return status;
    }

    kernelFile->Close(kernelFile);
    fsroot->Close(fsroot);

    return EFI_SUCCESS;
}