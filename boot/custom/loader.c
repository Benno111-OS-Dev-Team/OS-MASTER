#include "uefi.h"

static EFI_SYSTEM_TABLE *g_st;
static EFI_HANDLE g_image;

static EFI_STATUS fatal(const char *code, const char *message, EFI_STATUS status) {
  efi_print(g_st, "\nOS8 loader error ");
  efi_print(g_st, code);
  efi_print(g_st, ": ");
  efi_print(g_st, message);
  efi_print(g_st, " status=");
  efi_print_hex(g_st, status);
  efi_print(g_st, "\n");
  return status ? status : EFI_LOAD_ERROR;
}

static int verify_buffer(const void *buffer, uint64_t size, const char *expected_hex) {
  uint8_t actual[32];
  uint8_t expected[32];
  if (!hex_to_bytes(expected_hex, expected, sizeof(expected))) return 0;
  sha256((const uint8_t *)buffer, size, actual);
  return efi_memcmp(actual, expected, sizeof(actual)) == 0;
}

static void poll_startup_keys(uint64_t timeout_ms, int *ctrl_b, char *letters, uint64_t letters_len) {
  uint64_t elapsed = 0;
  uint64_t letter_count = 0;
  EFI_INPUT_KEY key;
  *ctrl_b = 0;
  if (letters_len) letters[0] = 0;

  while (elapsed < timeout_ms) {
    while (g_st->ConIn && !EFI_ERROR(g_st->ConIn->ReadKeyStroke(g_st->ConIn, &key))) {
      CHAR16 c = key.UnicodeChar;
      if (c == 2) {
        *ctrl_b = 1;
      } else if (c >= 1 && c <= 26 && letter_count + 1 < letters_len) {
        letters[letter_count++] = (char)('a' + c - 1);
        letters[letter_count] = 0;
      }
    }
    g_st->BootServices->Stall(100000);
    elapsed += 100;
  }
}

static EFI_STATUS propagate_boot_device(EFI_HANDLE startup_handle) {
  EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_LOADED_IMAGE_PROTOCOL *loader_loaded = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *startup_loaded = NULL;
  EFI_STATUS status;

  status = g_st->BootServices->OpenProtocol(g_image, &loaded_guid,
                                            (void **)&loader_loaded, g_image,
                                            NULL,
                                            EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) return status;

  status = g_st->BootServices->OpenProtocol(
      startup_handle, &loaded_guid, (void **)&startup_loaded, g_image, NULL,
      EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  if (EFI_ERROR(status)) return status;

  startup_loaded->DeviceHandle = loader_loaded->DeviceHandle;
  return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  EFI_STATUS status;
  void *cfg_data = NULL;
  uint64_t cfg_size = 0;
  void *startup = NULL;
  uint64_t startup_size = 0;
  EFI_HANDLE startup_handle = NULL;
  char startup_hash[80];
  char startup_path_ascii[128];
  char timeout_ascii[32];
  char keys[32];
  int ctrl_b = 0;
  uint64_t timeout_ms = 1500;
  static CHAR16 startup_path[128] = L"\\EFI\\OS8\\STARTUPX64.EFI";

  g_st = st;
  g_image = image;
  st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);
  if (st->ConOut) st->ConOut->ClearScreen(st->ConOut);

  efi_print(st, "OS8 Custom EFI Loader\n");
  efi_print(st, "Firmware Secure Boot has already authenticated this image.\n");
  efi_print(st, "Checking battery state... unavailable, continuing with warning BATTERY-0002\n");

  status = efi_read_file(image, st, L"\\EFI\\OS8\\os8boot.cfg", &cfg_data, &cfg_size);
  if (EFI_ERROR(status)) {
    return fatal("CONFIG-0001", "The system boot configuration is invalid.", status);
  }

  if (cfg_get((const char *)cfg_data, "startup_path", startup_path_ascii, sizeof(startup_path_ascii))) {
    uint64_t i = 0;
    while (startup_path_ascii[i] && i + 1 < sizeof(startup_path) / sizeof(startup_path[0])) {
      startup_path[i] = (CHAR16)startup_path_ascii[i];
      i++;
    }
    startup_path[i] = 0;
  }
  if (cfg_get((const char *)cfg_data, "input_timeout_ms", timeout_ascii, sizeof(timeout_ascii))) {
    timeout_ms = 0;
    for (uint64_t i = 0; timeout_ascii[i] >= '0' && timeout_ascii[i] <= '9'; i++) {
      timeout_ms = timeout_ms * 10 + (uint64_t)(timeout_ascii[i] - '0');
    }
  }

  efi_print(st, "Collecting startup keys...\n");
  poll_startup_keys(timeout_ms, &ctrl_b, keys, sizeof(keys));
  if (ctrl_b) efi_print(st, "Ctrl+B requested; default startup partition remains selected.\n");
  if (keys[0]) {
    efi_print(st, "Startup option keys captured: ");
    efi_print(st, keys);
    efi_print(st, "\n");
  }

  status = efi_read_file(image, st, startup_path, &startup, &startup_size);
  if (EFI_ERROR(status)) {
    return fatal("STARTUP-0001", "The startup executable could not be loaded.", status);
  }
  if (!cfg_get((const char *)cfg_data, "startup_sha256", startup_hash, sizeof(startup_hash)) ||
      !verify_buffer(startup, startup_size, startup_hash)) {
    return fatal("STARTUP-0002", "The startup executable is not trusted.", EFI_SECURITY_VIOLATION);
  }

  efi_print(st, "Startup executable verified. Transferring control...\n");
  status = st->BootServices->LoadImage(0, image, NULL, startup, startup_size,
                                       &startup_handle);
  if (EFI_ERROR(status)) {
    return fatal("STARTUP-0001", "The startup executable could not be prepared.", status);
  }
  status = propagate_boot_device(startup_handle);
  if (EFI_ERROR(status)) {
    return fatal("STARTUP-0001", "The startup executable boot device could not be inherited.", status);
  }
  status = st->BootServices->StartImage(startup_handle, NULL, NULL);
  return fatal("STARTUP-0003", "The startup executable returned unexpectedly.", status);
}
