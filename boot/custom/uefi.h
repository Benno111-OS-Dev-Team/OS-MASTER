/*
 * OS8 custom x86_64 UEFI boot chain support.
 */

#ifndef OS8_CUSTOM_UEFI_H
#define OS8_CUSTOM_UEFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 1
#define EFI_INVALID_PARAMETER 2
#define EFI_UNSUPPORTED 3
#define EFI_BAD_BUFFER_SIZE 4
#define EFI_BUFFER_TOO_SMALL 5
#define EFI_NOT_READY 6
#define EFI_DEVICE_ERROR 7
#define EFI_WRITE_PROTECTED 8
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NOT_FOUND 14
#define EFI_SECURITY_VIOLATION 26

#define EFI_ERROR(x) ((x) != EFI_SUCCESS)
#define EFI_STATUS_CODE(x) ((x) & 0x7fffffffffffffffULL)
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL 0x00000001
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
  {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
  {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_FILE_INFO_GUID \
  {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
  {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}
#define ACPI_20_TABLE_GUID \
  {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}
#define ACPI_TABLE_GUID \
  {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}

typedef struct {
  uint32_t Data1;
  uint16_t Data2;
  uint16_t Data3;
  uint8_t Data4[8];
} EFI_GUID;

typedef struct {
  uint64_t Signature;
  uint32_t Revision;
  uint32_t HeaderSize;
  uint32_t CRC32;
  uint32_t Reserved;
} EFI_TABLE_HEADER;

typedef enum {
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
  EfiPersistentMemory,
  EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum {
  AllocateAnyPages,
  AllocateMaxAddress,
  AllocateAddress
} EFI_ALLOCATE_TYPE;

typedef struct {
  uint32_t Type;
  uint32_t Pad;
  EFI_PHYSICAL_ADDRESS PhysicalStart;
  EFI_VIRTUAL_ADDRESS VirtualStart;
  uint64_t NumberOfPages;
  uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
  uint16_t ScanCode;
  CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
  EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, uint8_t ExtendedVerification);
  EFI_STATUS (*ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
  EFI_EVENT WaitForKey;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *Reset;
  EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
  void *TestString;
  void *QueryMode;
  void *SetMode;
  EFI_STATUS (*SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, uint64_t Attribute);
  EFI_STATUS (*ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
  void *SetCursorPosition;
  void *EnableCursor;
  void *Mode;
};

struct EFI_BOOT_SERVICES {
  EFI_TABLE_HEADER Hdr;
  void *RaiseTPL;
  void *RestoreTPL;
  EFI_STATUS (*AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                              uint64_t Pages, EFI_PHYSICAL_ADDRESS *Memory);
  EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, uint64_t Pages);
  EFI_STATUS (*GetMemoryMap)(uint64_t *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                             uint64_t *MapKey, uint64_t *DescriptorSize,
                             uint32_t *DescriptorVersion);
  EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, uint64_t Size, void **Buffer);
  EFI_STATUS (*FreePool)(void *Buffer);
  EFI_STATUS (*CreateEvent)(uint32_t Type, uint64_t NotifyTpl, void *NotifyFunction,
                            void *NotifyContext, EFI_EVENT *Event);
  EFI_STATUS (*SetTimer)(EFI_EVENT Event, int32_t Type, uint64_t TriggerTime);
  EFI_STATUS (*WaitForEvent)(uint64_t NumberOfEvents, EFI_EVENT *Event, uint64_t *Index);
  EFI_STATUS (*SignalEvent)(EFI_EVENT Event);
  EFI_STATUS (*CloseEvent)(EFI_EVENT Event);
  EFI_STATUS (*CheckEvent)(EFI_EVENT Event);
  void *InstallProtocolInterface;
  void *ReinstallProtocolInterface;
  void *UninstallProtocolInterface;
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
  void *Reserved;
  void *RegisterProtocolNotify;
  void *LocateHandle;
  void *LocateDevicePath;
  void *InstallConfigurationTable;
  EFI_STATUS (*LoadImage)(uint8_t BootPolicy, EFI_HANDLE ParentImageHandle,
                          void *DevicePath, void *SourceBuffer,
                          uint64_t SourceSize, EFI_HANDLE *ImageHandle);
  EFI_STATUS (*StartImage)(EFI_HANDLE ImageHandle, uint64_t *ExitDataSize,
                           CHAR16 **ExitData);
  EFI_STATUS (*Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus,
                     uint64_t ExitDataSize, CHAR16 *ExitData);
  void *UnloadImage;
  EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, uint64_t MapKey);
  void *GetNextMonotonicCount;
  EFI_STATUS (*Stall)(uint64_t Microseconds);
  EFI_STATUS (*SetWatchdogTimer)(uint64_t Timeout, uint64_t WatchdogCode,
                                 uint64_t DataSize, CHAR16 *WatchdogData);
  void *ConnectController;
  void *DisconnectController;
  EFI_STATUS (*OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface,
                             EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle,
                             uint32_t Attributes);
  EFI_STATUS (*CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                              EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
  void *OpenProtocolInformation;
  void *ProtocolsPerHandle;
  void *LocateHandleBuffer;
  EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
};

struct EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER Hdr;
  CHAR16 *FirmwareVendor;
  uint32_t FirmwareRevision;
  EFI_HANDLE ConsoleInHandle;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
  EFI_RUNTIME_SERVICES *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
  uint64_t NumberOfTableEntries;
  void *ConfigurationTable;
};

typedef struct {
  EFI_GUID VendorGuid;
  void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef enum {
  PixelRedGreenBlueReserved8BitPerColor,
  PixelBlueGreenRedReserved8BitPerColor,
  PixelBitMask,
  PixelBltOnly,
  PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
  uint32_t RedMask;
  uint32_t GreenMask;
  uint32_t BlueMask;
  uint32_t ReservedMask;
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
  uint64_t SizeOfInfo;
  EFI_PHYSICAL_ADDRESS FrameBufferBase;
  uint64_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
  void *QueryMode;
  void *SetMode;
  void *Blt;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

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
  EFI_MEMORY_TYPE ImageCodeType;
  EFI_MEMORY_TYPE ImageDataType;
  EFI_STATUS (*Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_READ_ONLY 0x0000000000000001ULL

struct EFI_FILE_PROTOCOL {
  uint64_t Revision;
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                     CHAR16 *FileName, uint64_t OpenMode, uint64_t Attributes);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *This);
  EFI_STATUS (*Delete)(EFI_FILE_PROTOCOL *This);
  EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *This, uint64_t *BufferSize, void *Buffer);
  EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *This, uint64_t *BufferSize, void *Buffer);
  EFI_STATUS (*GetPosition)(EFI_FILE_PROTOCOL *This, uint64_t *Position);
  EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *This, uint64_t Position);
  EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                        uint64_t *BufferSize, void *Buffer);
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  uint64_t Revision;
  EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                           EFI_FILE_PROTOCOL **Root);
};

typedef struct {
  uint64_t Size;
  uint64_t FileSize;
  uint64_t PhysicalSize;
  uint64_t CreateTime[3];
  uint64_t LastAccessTime[3];
  uint64_t ModificationTime[3];
  uint64_t Attribute;
  CHAR16 FileName[1];
} EFI_FILE_INFO;

void *efi_memcpy(void *dst, const void *src, uint64_t n);
void *efi_memset(void *dst, int c, uint64_t n);
int efi_memcmp(const void *a, const void *b, uint64_t n);
uint64_t efi_strlen(const char *s);
int efi_streq(const char *a, const char *b);
void efi_print(EFI_SYSTEM_TABLE *st, const char *s);
void efi_print_hex(EFI_SYSTEM_TABLE *st, uint64_t value);
EFI_STATUS efi_open_root(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, EFI_FILE_PROTOCOL **root);
EFI_STATUS efi_read_file(EFI_HANDLE image, EFI_SYSTEM_TABLE *st, const CHAR16 *path,
                         void **buffer, uint64_t *size);
void sha256(const uint8_t *data, uint64_t len, uint8_t out[32]);
int hex_to_bytes(const char *hex, uint8_t *out, uint64_t out_len);
const char *cfg_get(const char *cfg, const char *key, char *out, uint64_t out_len);
int efi_guid_eq(const EFI_GUID *a, const EFI_GUID *b);
void startup_enter_kernel(uint64_t pml4_phys, uint64_t entry, void *handoff);
void startup_enter_xnu_kernel(uint64_t pml4_phys, uint64_t entry,
                              uint64_t boot_args_phys);

#endif
