/**
在UEFI世界內，所謂BootLoader只是一個Binary Executable，透過UEFI的API去獲取系統訊息，並加載內核image到內存的程序

Target Physical Memory Layout:
0x00000000
0x00060000 - 60FFF (1 page)  KERNEL_BOOT_PARAMETER_INFORMATION (估計半頁內)
0x00100000         (n pages) kernel.bin

Details see deviceinfo.txt

本程序有三個任務：
1. 獲取系統Memory Map，以確定系統哪些記憶體可供操作系統使用
2. 獲取當前顯示模式的FrameBuffer位置，並設定好Memory Map
3. 從ESP分區讀取kernel.bin並加載至記憶體，並跳轉至kernel.bin的第一行開始執行

執行順序：
1. 先加載kernel.bin (已包括PageTables)
2. 獲取FrameBuffer，設定好PageTables將Base Address映射至虛擬地址
3. 獲取最新MemoryMap
4. 跳轉至kernel.bin
*/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h> 

#define COUNT_PAGES(bytes)  ((bytes+4095)>>12)

// 先設定好會傳給kernel的數據結構，由於要跨binary傳數據，kernel和bootloader所用的編譯器和ABI稍有不同，就會有不同的padding behavior。
// 避免拆包裹的人拿錯數據，寫明不需要padding，用__attribute__((packed))
// 另外unsigned int, unsigned long的實際長度都depends on ABI，因此改用UEFI types
// 大原則：跨binary就不能假設ABI相同，數據定義越explicit越死越好
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
    struct EFI_E820_MEMORY_DESCRIPTOR E820_Entry[0];    // C 動態陣列寫法
} __attribute__((packed));

struct KERNEL_BOOT_PARAMETER_INFORMATION {
    struct EFI_GRAPHICS_OUTPUT_INFORMATION Graphics_Info;
    struct EFI_E820_MEMORY_DESCRIPTOR_INFORMATION E820_Info;
} __attribute__((packed));

// UEFI程序入口，ImageHandle用來指代本程序，SystemTable為UEFI firmware準備的一切API入口
EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) { 
    // 獲取UEFI Boot Service
    EFI_BOOT_SERVICES* gBS = SystemTable->BootServices;
    EFI_PHYSICAL_ADDRESS Pages;
    EFI_STATUS Status;
    
    // 本程序假設kernel.bin必定位於本程序所在分區內。
    // 首先使用LoadedImageProtocol獲取本程序的DeviceHandle(硬碟/USB)。
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImageProtocol;
    Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImageProtocol);
    if(Status!=EFI_SUCCESS){
        return Status;
    }
    // 查看儲存設備分區是否支援FAT
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FS;
    Status = gBS->HandleProtocol(LoadedImageProtocol->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&FS);
    if(Status!=EFI_SUCCESS){
        return Status;
    }

    // 確認支援FAT後，嘗試打開根目錄 (類似Shell中運行fs0:)
    EFI_FILE_PROTOCOL* FSRoot;
    Status = FS->OpenVolume(FS, &FSRoot);
    if(Status!=EFI_SUCCESS){
        return Status;
    }
    
    // 開啟kernel.bin檔案
    EFI_FILE_HANDLE KernelBin; // EFI_FILE_PROTOCOL**
    Status = FSRoot->Open(FSRoot, &KernelBin, (CHAR16*)L"kernel.bin", EFI_FILE_MODE_READ, 0); 
    if(Status!=EFI_SUCCESS){
        return Status;
    }
    Print(L"Open kernel.bin\t\t\t\tSuccess\n");

    // 拿取檔案資料
    EFI_FILE_INFO* FileInfo = NULL;
    UINTN FileInfoSize = 0;
    Status = KernelBin->GetInfo(KernelBin, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    if(Status!=EFI_BUFFER_TOO_SMALL){
        return Status;
    }
    // 分配Pool，先放在EfiBootServicesData，即使忘記FreePool也能在ExitBootServices後自動回收
    Status = gBS->AllocatePool(EfiBootServicesData, FileInfoSize, (VOID**) &FileInfo);
    if(Status != EFI_SUCCESS){
        return Status;
    }
    Status = KernelBin->GetInfo(KernelBin, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    if(Status != EFI_SUCCESS){
        return Status;
    }
    // 獲取檔案大小以計算需要多少內存頁裝填kernel.bin
    UINT64 KERNEL_BIN_SIZE = FileInfo->FileSize;
    gBS->FreePool(FileInfo);
    Print(L"File Name: %s\tSize: %d\tNo Of Pages: %d\n", FileInfo->FileName, KERNEL_BIN_SIZE, COUNT_PAGES(KERNEL_BIN_SIZE));

    // 分配內存頁以裝載kernel.bin，需要裝到特定address上
    // 選擇0x100000作為內核開始位置，因為多數UEFI firmware都會在這個地址留下幾MB空間。
    // EfiLoaderCode： UEFI固件完全唔會觸碰、唔會移動、亦唔會重定位。
    Pages = 0x100000;
    Status = gBS->AllocatePages(AllocateAddress, EfiLoaderCode, COUNT_PAGES(KERNEL_BIN_SIZE), &Pages);
    if(Status != EFI_SUCCESS){
        return Status;
    }
    Status = KernelBin->Read(KernelBin, &KERNEL_BIN_SIZE, (VOID*) Pages);
    if(Status!=EFI_SUCCESS){
        return Status;
    }
    Print(L"Load kernel.bin\t\t\t\tSuccess\n");
    Print(L"kernel.bin occupied physical address %016lx to %016lx, requires %d 4KB Pages\n", Pages, (CHAR8*)Pages + (COUNT_PAGES(KERNEL_BIN_SIZE) << 12), COUNT_PAGES(KERNEL_BIN_SIZE));

    // 關閉資源
    KernelBin->Close(KernelBin);
    FSRoot->Close(FSRoot);
    gBS->CloseProtocol(LoadedImageProtocol->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, ImageHandle, NULL);
    gBS->CloseProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, ImageHandle, NULL);

    // 準備一內存頁以填裝KERNEL_BOOT_PARAMETER_INFORMATION
    Pages = 0x60000;
    Status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, 1, &Pages);
    if(Status!=EFI_SUCCESS){
        return Status;
    }
    gBS->SetMem((VOID*) Pages, 0x1000, 0); // 0x1000 = 4KB
    struct KERNEL_BOOT_PARAMETER_INFORMATION* BootParams = (struct KERNEL_BOOT_PARAMETER_INFORMATION*)Pages;

    // 準備填充顯示模式訊息，查看顯示模式需要用到UEFI中的Graphics Output Protocol
    EFI_GRAPHICS_OUTPUT_PROTOCOL* GOP = NULL;

    // LocateProtocol的作用是返回系統中所有支援此Protocol的Handle(設備), 所以GOP是一個陣列
    gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, 0, (VOID **) &GOP);
    Print(L"Get FrameBuffer\t\t\t\tSuccess\n");
    Print(L"(Current Mode) Resolution: (%d x %d)\n", GOP->Mode->Info->HorizontalResolution, GOP->Mode->Info->VerticalResolution);
    Print(L"FrameBuffer occupied physical address %016lx to %016lx, size: %d B\n", GOP->Mode->FrameBufferBase, GOP->Mode->FrameBufferBase + GOP->Mode->FrameBufferSize, GOP->Mode->FrameBufferSize);
    BootParams->Graphics_Info.HorizontalResolution = GOP->Mode->Info->HorizontalResolution;
    BootParams->Graphics_Info.VerticalResolution = GOP->Mode->Info->VerticalResolution;
    BootParams->Graphics_Info.PixelsPerScanLine = GOP->Mode->Info->PixelsPerScanLine;
    BootParams->Graphics_Info.FrameBufferBase = GOP->Mode->FrameBufferBase;
    BootParams->Graphics_Info.FrameBufferSize = GOP->Mode->FrameBufferSize;

    gBS->CloseProtocol(GOP, &gEfiGraphicsOutputProtocolGuid, ImageHandle, NULL);

    // Overwrite kernel.bin中設定的Page Table，將FrameBuffer的物理地址 0x8000000 map至 Virtual Address 0xffff800003000000 
    UINT64* PageTableEntry = (UINT64*) 0x103000; //level 3 Page Table (2MB Big Page is ON after kernel change page tables)
    for(int i = 0;i < (BootParams->Graphics_Info.FrameBufferSize + 0x200000 - 1) >> 21 ; i++){
        //0xffff800003000000 對應 level 3 page table 24th entry
        *(PageTableEntry + 24 + i) = BootParams->Graphics_Info.FrameBufferBase | 0x200000 * i | 0x87; // ring 3 RW
    }

    // 準備填充Memory Map訊息
    EFI_MEMORY_DESCRIPTOR* MemoryMap = NULL;
    UINTN MemoryMapSize = 0;
    UINTN MapKey = 0;   // Idempotent Key
    UINTN DescriptorSize = 0; //以此為準，不用sizeof(EFI_MEMORY_DESCRIPTOR)
    UINT32 DescriptorVersion = 0;

    // 獲取Memory Map大小
    Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(Status!=EFI_BUFFER_TOO_SMALL){
        return Status;
    }
    MemoryMapSize += DescriptorSize * 5; // 預留多幾個Descriptor，因為下面要AllocatePool，萬一Pool滿左，用了新頁，就會多了兩個Descriptor。

    // 分配Memory Pool裝載Memory Map
    Status = gBS->AllocatePool(EfiBootServicesData, MemoryMapSize, (VOID**)&MemoryMap);
    if(Status != EFI_SUCCESS){
        return Status;
    }

    // 今次帶著剛分配的Memory 作為Buffer去重新獲取Memory Map
    Status = gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    if(Status!=EFI_SUCCESS){
        return Status;
    }

    // 每一個Memory Descriptor，都要按Memory Type 分類，和前一個同類相連Descriptor合併。
    struct EFI_E820_MEMORY_DESCRIPTOR* currentE820 = BootParams->E820_Info.E820_Entry;
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

    currentE820 = BootParams->E820_Info.E820_Entry;
    BootParams->E820_Info.E820_Entry_Count = E820_Count;
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
        Print(L"E820 MemoryMap (%016lx to %016lx), Type = %4d\n", currentE820->address, currentE820->address + currentE820->length, currentE820->type);
        currentE820++;
    }

    gBS->FreePool(MemoryMap);

    // 跳去kernel.bin!!
    Print(L"Jump to the kernel. Leap of Fate!!! \n"); // Print 一定要在GetMemoryMap前，因為Print本身有機會影響Memory Map，kind of like 量子力學測不準定理
    gBS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);
    Status = gBS->ExitBootServices(ImageHandle, MapKey);
    if(Status!=EFI_SUCCESS){
        return Status;
    }
    
    void (*func) (void);
    func = (void*)(0x100000);
    func();

    return EFI_SUCCESS;
}