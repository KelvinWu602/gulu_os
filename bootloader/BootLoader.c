/**
在UEFI世界內，所謂BootLoader只是一個Binary Executable，透過UEFI的API去獲取系統訊息，並加載內核image到內存的程序

Target Physical Memory Layout:
0x00000000
0x00060000 - 60FFF (1 page)  KERNEL_BOOT_PARAMETER_INFORMATION (估計半頁內)
0x00100000         (n pages) kernel.bin

Details see deviceinfo.txt

*/

#include <Uefi.h>
#include <Library/UefiLib.h>

#define COUNT_PAGES(bytes)  ((bytes+4095)>>12)

// 先設定好會傳給kernel的數據結構，由於要跨binary傳數據，kernel和bootloader所用的編譯器和ABI稍有不同，就會有不同的padding behavior。
// 避免牛頭不對馬嘴，寫明不需要padding，用__attribute__((packed))
// 另外unsigned int, unsigned long的實際長度都depends on ABI，因此改用UEFI types
// 大原則：跨binary數據傳輸越explicit越死越好
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
    // 獲取UEFI Boot Service
    EFI_BOOT_SERVICES* gBS = SystemTable->BootServices;
    EFI_PHYSICAL_ADDRESS pages;
    EFI_STATUS status;

    // 任務1:是要將kernel.bin讀入內存，先假設kernel.bin必定位於bootloader所在分區內。

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
    if(status!=EFI_BUFFER_TOO_SMALL){
        return status;
    }
    // 分配Pool，LoaderData會在Kernel讀完參數後才回收
    status = gBS->AllocatePool(EfiLoaderData, fileInfoSize, (VOID**) &fileinfo);
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
    pages = 0x100000;
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
    
    
    gBS->CloseProtocol(loadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, ImageHandle, NULL);
    gBS->CloseProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, ImageHandle, NULL);

    // 任務2:準備一內存頁以填裝KERNEL_BOOT_PARAMETER_INFORMATION
    pages = 0x60000;
    status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 1, &pages);
    if(status!=EFI_SUCCESS){
        return status;
    }
    status = gBS->SetMem((VOID*) pages, 0x1000, 0); // 0x1000 = 4KB
    if(status!=EFI_SUCCESS){
        return status;
    }
    struct KERNEL_BOOT_PARAMETER_INFORMATION* bootParams = (struct KERNEL_BOOT_PARAMETER_INFORMATION*)pages;

    // 任務3: 準備填充顯示模式訊息
    // 查看顯示模式需要用到UEFI中的Graphics Output Protocol
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;

    // LocateProtocol的作用是返回系統中所有支援此Protocol的Handle(設備), 所以gop是一個陣列
    gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, 0, (VOID **) &gop);
    Print(L"(Current Mode) Resolution: (%d x %d), FrameBuffer Address: (%x, size: %d)\n", 
        gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution, 
        gop->Mode->FrameBufferBase, gop->Mode->FrameBufferSize
    );
    bootParams->Graphics_Info.HorizontalResolution = gop->Mode->Info->HorizontalResolution;
    bootParams->Graphics_Info.VerticalResolution = gop->Mode->Info->VerticalResolution;
    bootParams->Graphics_Info.PixelsPerScanLine = gop->Mode->Info->PixelsPerScanLine;
    bootParams->Graphics_Info.FrameBufferBase = gop->Mode->FrameBufferBase;
    bootParams->Graphics_Info.FrameBufferSize = gop->Mode->FrameBufferSize;

    gBS->CloseProtocol(gop, &gEfiGraphicsOutputProtocolGuid, ImageHandle, NULL);

    // 任務四：要Overwrite kernel.bin中設定的Page Table，將FrameBuffer的物理地址 0x8000000 map至 Virtual Address 0xffff800003000000 
    UINT64* PageTableEntry = (UINT64*) 0x103000; //level 3 Page Table (2MB Big Page is ON after kernel change page tables)
    for(int i = 0;i < (bootParams->Graphics_Info.FrameBufferSize + 0x200000 - 1) >> 21 ; i++){
        //0xffff800003000000 對應 level 3 page table 24th entry
        *(PageTableEntry + 24 + i) = bootParams->Graphics_Info.FrameBufferBase | 0x200000 * i | 0x87; // ring 3 RW
        Print(L"Page %2d, Address: %018lx, Value: %018lx\n", i, (UINT64)(PageTableEntry+24+i), *(PageTableEntry+24+i));
    }

    // 任務五: 準備填充Memory Map訊息


    // 準備Meta Data
    EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
    UINTN MemoryMapSize = 0;
    UINTN MapKey = 0;   // Idempotent Key
    UINTN DescriptorSize = 0; //以此為準，不用sizeof(EFI_MEMORY_DESCRIPTOR)
    UINT32 DescriptorVersion = 0;

    // 獲取Memory Map大小
    status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(status!=EFI_BUFFER_TOO_SMALL){
        return status;
    }
    MemoryMapSize += DescriptorSize * 5; // 預留多幾個Descriptor，因為下面要AllocatePool，萬一Pool滿左，用了新頁，就會多了兩個Descriptor。

    // 分配Memory Pool裝載Memory Map
    status = gBS->AllocatePool(EfiBootServicesData, MemoryMapSize, (VOID**)&MemoryMap);
    if(status != EFI_SUCCESS){
        return status;
    }

    // 今次帶著剛分配的Memory 作為Buffer去重新獲取Memory Map
    status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(status!=EFI_SUCCESS){
        return status;
    }

    // 每一個Memory Descriptor，都要按Memory Type 分類，和前一個同類相連Descriptor合併。
    struct EFI_E820_MEMORY_DESCRIPTOR* currentE820 = bootParams->E820_Info.E820_Entry;
    struct EFI_E820_MEMORY_DESCRIPTOR* LastE820 = NULL;
    unsigned int E820_Count = 0;
    UINT64 LastEndAddr = 0;
    for(int i = 0; i < (MemoryMapSize/DescriptorSize) ; i++){
        UINT32 MemoryType = 0;
        // 獲取第i個Descriptor 
        EFI_MEMORY_DESCRIPTOR* currentMemDesc = (EFI_MEMORY_DESCRIPTOR*) ((CHAR8*) MemoryMap + i * DescriptorSize);
        // 如果該Descriptor不佔頁則跳過
        if(currentMemDesc->NumberOfPages == 0) continue;
        // 分類
        switch(currentMemDesc->Type){
            case EfiReservedMemoryType:
            case EfiMemoryMappedIO:
            case EfiMemoryMappedIOPortSpace:
            case EfiPalCode:
                MemoryType = 2;     // ROM or reserved
                break;
            case EfiUnusableMemory:
                MemoryType = 5;     // garbage
                break;
            case EfiACPIReclaimMemory:
                MemoryType = 3;     // ACPI using
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData:
            case EfiConventionalMemory:
            case EfiPersistentMemory:
                MemoryType = 1;     // RAM
                break;
            case EfiACPIMemoryNVS:
                MemoryType = 4;
                break;
            default:
                Print(L"Invalid UEFI Memory Type: %4d\n", currentMemDesc->Type);
                continue;
        }

        if (
            (LastE820 != NULL) &&                               // 非第一個
            (LastE820->type == MemoryType) &&                  // 和前面的descriptor為同類
            (currentMemDesc->PhysicalStart == LastEndAddr)      // 和前面的descriptor為相鄰
        ) {
            // 合併為一個E820 descriptor
            LastE820 -> length += currentMemDesc->NumberOfPages << 12;
            LastEndAddr += currentMemDesc->NumberOfPages << 12;
        }else{
            // 創建新E820 descriptor
            currentE820->address = currentMemDesc->PhysicalStart;
            currentE820->length = currentMemDesc->NumberOfPages << 12;
            currentE820->type = MemoryType;
            LastEndAddr = currentMemDesc->PhysicalStart + (currentMemDesc->NumberOfPages << 12);
            LastE820 = currentE820;
            currentE820 ++;
            E820_Count ++;
        }
    }

    currentE820 = bootParams->E820_Info.E820_Entry;
    bootParams->E820_Info.E820_Count = E820_Count;
    struct EFI_E820_MEMORY_DESCRIPTOR temp;
    // E820 descriptor 按地址排序
    for(int i = 0 ; i < E820_Count ; i++){
        struct EFI_E820_MEMORY_DESCRIPTOR* e820i = currentE820 + i;
        for(int j = i+1; j < E820_Count; j++){
            struct EFI_E820_MEMORY_DESCRIPTOR* e820j = currentE820 + j;
            if(e820i->address > e820j->address){
                temp = *e820i;
                *e820i = *e820j;
                *e820j = temp;
            }
        }
    }

    // Print E820 Memory Map
    for(int i = 0; i < E820_Count; i++){
        Print(L"MemoryMap (%10lx to %10lx) %4d\n", currentE820->address, currentE820->address + currentE820->length, currentE820->type);
        currentE820++;
    }

    gBS->FreePool(MemoryMap);

    // 跳去kernel.bin!!
    gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    status = gBS->ExitBootServices(ImageHandle, MapKey);
    if(status!=EFI_SUCCESS){
        return status;
    }
    
    void (*func) (void);
    func = (void*)(0x100000);
    func();

    return EFI_SUCCESS;
}