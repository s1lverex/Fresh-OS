/* Fresh OS - minimal UEFI definitions (written from the UEFI 2.x spec).
 * Only what the loader needs. Compiled for the MS x64 ABI. */
#ifndef FRESH_EFI_H
#define FRESH_EFI_H

#include <stdint.h>

typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS 0
#define EFI_ERR(x) (0x8000000000000000ULL | (x))
#define EFI_LOAD_ERROR EFI_ERR(1)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ERROR(s) (((int64_t)(s)) < 0)

typedef struct {
    uint32_t d1;
    uint16_t d2, d3;
    uint8_t d4[8];
} EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* memory */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress
} EFI_ALLOCATE_TYPE;

enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory
};

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* text output */
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (*EFI_TEXT_STRING)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_TEXT_STRING OutputString;
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    void *ClearScreen;
    void *SetCursorPosition;
    void *EnableCursor;
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* boot services */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (*AllocatePages)(EFI_ALLOCATE_TYPE, uint32_t, UINTN, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
    EFI_STATUS (*GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, uint32_t *);
    EFI_STATUS (*AllocatePool)(uint32_t, UINTN, void **);
    EFI_STATUS (*FreePool)(void *);
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, UINTN);
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN);
    EFI_STATUS (*SetWatchdogTimer)(UINTN, uint64_t, UINTN, CHAR16 *);
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* loaded image */
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* file system */
struct EFI_FILE_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    void *Write;
    EFI_STATUS (*GetPosition)(struct EFI_FILE_PROTOCOL *, uint64_t *);
    EFI_STATUS (*SetPosition)(struct EFI_FILE_PROTOCOL *, uint64_t);
    void *GetInfo;
    void *SetInfo;
    void *Flush;
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ 1ULL

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* graphics output */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t, UINTN *,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t);
    void *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#endif
