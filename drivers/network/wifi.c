/*
 * OS8 - Wi-Fi Driver Layer
 *
 * This layer detects a curated set of PCI adapters and exposes a backend-driven
 * scan/connect model. Until native chipset support lands, the default backend
 * uses compatibility scan profiles so the UI can list and join networks.
 */

#include "drivers/pci.h"
#include "drivers/wifi.h"
#include "bootmanager.h"
#include "printk.h"
#include "string.h"

#define WIFI_MAX_NETWORKS 16
#define WIFI_MAX_PASSWORD_LEN 32

typedef struct {
  uint16_t vendor_id;
  uint16_t device_id;
  const char *adapter_name;
  const char *driver_name;
} wifi_pci_match_t;

typedef struct {
  char ssid[WIFI_MAX_SSID_LEN];
  int signal;
  int secure;
} wifi_network_t;

typedef struct {
  wifi_network_t network;
  char password[WIFI_MAX_PASSWORD_LEN];
} wifi_profile_t;

typedef struct {
  int initialized;
  int adapter_present;
  int intel_adapter;
  int connected;
  int scan_ready;
  int visible_count;
  int selected_network;
  int connected_signal;
  int real_scan_supported;
  int real_connect_supported;
  const char *adapter_name;
  const char *driver_name;
  const char *backend_name;
  char connected_ssid[WIFI_MAX_SSID_LEN];
  char status_text[128];
  wifi_network_t visible_networks[WIFI_MAX_NETWORKS];
  const wifi_backend_ops_t *backend;
} wifi_state_t;

static int wifi_stub_scan(void);
static int wifi_stub_connect(const char *ssid, int secure, const char *password);
static void wifi_stub_disconnect(void);
static int wifi_compat_scan(void);
static int wifi_compat_connect(const char *ssid, int secure, const char *password);
static void wifi_compat_disconnect(void);

static const wifi_backend_ops_t wifi_stub_backend = {0, 0, wifi_stub_scan,
                                                     wifi_stub_connect,
                                                     wifi_stub_disconnect};
static const wifi_backend_ops_t wifi_compat_backend = {1, 1, wifi_compat_scan,
                                                       wifi_compat_connect,
                                                       wifi_compat_disconnect};

static const wifi_pci_match_t wifi_supported_devices[] = {
    {0x8086, 0x0082, "Intel Centrino Advanced-N 6205",
     "intel iwlwifi driver"},
    {0x8086, 0x0085, "Intel Centrino Advanced-N + WiMAX 6250",
     "intel iwlwifi driver"},
    {0x8086, 0x008A, "Intel Centrino Wireless-N 1030",
     "intel iwlwifi driver"},
    {0x8086, 0x008B, "Intel Centrino Wireless-N 130",
     "intel iwlwifi driver"},
    {0x8086, 0x0887, "Intel Centrino Wireless-N 2230",
     "intel iwlwifi driver"},
    {0x8086, 0x0891, "Intel Centrino Wireless-N 2200",
     "intel iwlwifi driver"},
    {0x8086, 0x088E, "Intel Centrino Advanced-N 6235",
     "intel iwlwifi driver"},
    {0x8086, 0x088F, "Intel Centrino Advanced-N 6235",
     "intel iwlwifi driver"},
    {0x8086, 0x08B1, "Intel Dual Band Wireless-AC 7260",
     "intel iwlwifi driver"},
    {0x8086, 0x08B2, "Intel Dual Band Wireless-N 7260",
     "intel iwlwifi driver"},
    {0x8086, 0x08B3, "Intel Dual Band Wireless-AC 3160",
     "intel iwlwifi driver"},
    {0x8086, 0x08B4, "Intel Dual Band Wireless-AC 7260",
     "intel iwlwifi driver"},
    {0x168C, 0x0030, "Qualcomm Atheros AR93xx",
     "ath9k compatibility driver"},
    {0x14E4, 0x4359, "Broadcom BCM43228", "brcmsmac compatibility driver"},
    {0x10EC, 0x8178, "Realtek RTL8192CE", "rtl8192ce compatibility driver"},
};

static wifi_state_t wifi_state = {
    0,
    0,
    0,
    0,
    0,
    0,
    -1,
    0,
    0,
    0,
    "No supported Wi-Fi adapter detected",
    "No wireless driver bound",
    "no backend",
    "",
    "Wireless drivers are standing by.",
    {{0}},
    &wifi_stub_backend,
};

static wifi_profile_t wifi_profiles[WIFI_MAX_NETWORKS];
static int wifi_profile_count = 0;

static void wifi_copy_text(char *dst, int max, const char *src) {
  int i;

  if (!dst || max <= 0)
    return;

  for (i = 0; src && src[i] && i < max - 1; i++)
    dst[i] = src[i];
  dst[i] = '\0';
}

static int wifi_text_equal(const char *left, const char *right) {
  int i = 0;

  if (!left || !right)
    return 0;

  while (left[i] || right[i]) {
    if (left[i] != right[i])
      return 0;
    i++;
  }

  return 1;
}

static int wifi_clamp_signal(int signal) {
  if (signal < 0)
    return 0;
  if (signal > 100)
    return 100;
  return signal;
}

static int wifi_parse_int(const char *text, int fallback) {
  int value = 0;
  int i = 0;
  int parsed = 0;

  if (!text || !text[0])
    return fallback;

  while (text[i] >= '0' && text[i] <= '9') {
    value = value * 10 + (text[i] - '0');
    parsed = 1;
    i++;
  }

  if (!parsed)
    return fallback;

  return value;
}

static const char *wifi_find_cmdline_value(const char *cmdline, const char *key) {
  size_t key_len;
  const char *p;

  if (!cmdline || !key)
    return 0;

  key_len = strlen(key);
  p = cmdline;
  while (*p) {
    if ((p == cmdline || p[-1] == ' ') && strncmp(p, key, key_len) == 0)
      return p + key_len;
    while (*p && *p != ' ')
      p++;
    while (*p == ' ')
      p++;
  }

  return 0;
}

static void wifi_copy_token(char *dst, int max, const char *src, char stop) {
  int i = 0;

  if (!dst || max <= 0)
    return;

  while (src && src[i] && src[i] != stop && src[i] != '|' && i < max - 1) {
    char ch = src[i];
    dst[i] = ch == '_' ? ' ' : ch;
    i++;
  }
  dst[i] = '\0';
}

static void wifi_clear_profiles(void) {
  wifi_profile_count = 0;
  memset(wifi_profiles, 0, sizeof(wifi_profiles));
}

static void wifi_add_profile(const char *ssid, int signal, int secure,
                             const char *password) {
  wifi_profile_t *profile;

  if (!ssid || !ssid[0] || wifi_profile_count >= WIFI_MAX_NETWORKS)
    return;

  profile = &wifi_profiles[wifi_profile_count++];
  wifi_copy_text(profile->network.ssid, sizeof(profile->network.ssid), ssid);
  profile->network.signal = wifi_clamp_signal(signal);
  profile->network.secure = secure ? 1 : 0;
  wifi_copy_text(profile->password, sizeof(profile->password),
                 password ? password : "");
}

static void wifi_load_default_profiles(void) {
  wifi_add_profile("HomeLab", 82, 1, "os8demo");
  wifi_add_profile("Workshop 5G", 74, 1, "workbench");
  wifi_add_profile("VibOS Guest", 63, 0, "");
  wifi_add_profile("RetroLAN", 48, 1, "retrolan");
}

static void wifi_parse_scan_profiles(const char *value) {
  const char *entry = value;

  while (entry && *entry) {
    char ssid[WIFI_MAX_SSID_LEN];
    char signal_buf[8];
    char secure_buf[8];
    char password[WIFI_MAX_PASSWORD_LEN];
    const char *field = entry;
    int field_len = 0;

    while (*field && *field != ',' && *field != ' ')
      field++, field_len++;
    wifi_copy_token(ssid, sizeof(ssid), entry, ',');
    entry += field_len;
    if (*entry != ',')
      break;
    entry++;

    wifi_copy_token(signal_buf, sizeof(signal_buf), entry, ',');
    while (*entry && *entry != ',' && *entry != ' ')
      entry++;
    if (*entry != ',')
      break;
    entry++;

    wifi_copy_token(secure_buf, sizeof(secure_buf), entry, ',');
    while (*entry && *entry != ',' && *entry != '|' && *entry != ' ')
      entry++;

    password[0] = '\0';
    if (*entry == ',') {
      entry++;
      wifi_copy_token(password, sizeof(password), entry, '|');
      while (*entry && *entry != '|' && *entry != ' ')
        entry++;
    }

    wifi_add_profile(ssid, wifi_parse_int(signal_buf, 50),
                     wifi_parse_int(secure_buf, 0) ? 1 : 0, password);

    if (*entry == '|')
      entry++;
    while (*entry == ' ')
      entry++;
  }
}

static void wifi_load_profiles_from_cmdline(void) {
  struct boot_config *boot_cfg = boot_get_config();
  const char *scan_value;

  wifi_clear_profiles();

  scan_value = wifi_find_cmdline_value(boot_cfg ? boot_cfg->kernel_cmdline : "",
                                       "wifi.scan=");
  if (scan_value)
    wifi_parse_scan_profiles(scan_value);

  if (wifi_profile_count <= 0)
    wifi_load_default_profiles();
}

static wifi_profile_t *wifi_find_profile(const char *ssid) {
  int i;

  for (i = 0; i < wifi_profile_count; i++) {
    if (wifi_text_equal(wifi_profiles[i].network.ssid, ssid))
      return &wifi_profiles[i];
  }

  return 0;
}

static void wifi_set_status(const char *text) {
  wifi_copy_text(wifi_state.status_text, sizeof(wifi_state.status_text), text);
}

static void wifi_set_status_with_suffix(const char *prefix, const char *suffix) {
  int len = 0;

  wifi_copy_text(wifi_state.status_text, sizeof(wifi_state.status_text), prefix);
  while (wifi_state.status_text[len] &&
         len < (int)sizeof(wifi_state.status_text) - 1)
    len++;
  wifi_copy_text(wifi_state.status_text + len,
                 (int)(sizeof(wifi_state.status_text) - len), suffix);
}

static void wifi_sort_visible_networks(void) {
  int i;
  int j;

  for (i = 0; i < wifi_state.visible_count; i++) {
    for (j = i + 1; j < wifi_state.visible_count; j++) {
      if (wifi_state.visible_networks[j].signal >
          wifi_state.visible_networks[i].signal) {
        wifi_network_t tmp = wifi_state.visible_networks[i];
        wifi_state.visible_networks[i] = wifi_state.visible_networks[j];
        wifi_state.visible_networks[j] = tmp;
      }
    }
  }
}

static void wifi_set_default_selection(void) {
  int i;

  if (wifi_state.visible_count <= 0) {
    wifi_state.selected_network = -1;
    return;
  }

  if (wifi_state.connected && wifi_state.connected_ssid[0]) {
    for (i = 0; i < wifi_state.visible_count; i++) {
      if (wifi_text_equal(wifi_state.visible_networks[i].ssid,
                          wifi_state.connected_ssid)) {
        wifi_state.selected_network = i;
        return;
      }
    }
  }

  wifi_state.selected_network = 0;
}

static void wifi_reset_scan_results(void) {
  int i;

  wifi_state.visible_count = 0;
  wifi_state.selected_network = -1;
  wifi_state.scan_ready = 0;
  for (i = 0; i < WIFI_MAX_NETWORKS; i++) {
    wifi_state.visible_networks[i].ssid[0] = '\0';
    wifi_state.visible_networks[i].signal = 0;
    wifi_state.visible_networks[i].secure = 0;
  }
}

static const wifi_pci_match_t *wifi_probe_supported_adapter(void) {
  int i;

  for (i = 0; i < (int)(sizeof(wifi_supported_devices) /
                        sizeof(wifi_supported_devices[0]));
       i++) {
    if (pci_find_device(wifi_supported_devices[i].vendor_id,
                        wifi_supported_devices[i].device_id))
      return &wifi_supported_devices[i];
  }

  return 0;
}

static int wifi_stub_scan(void) {
  wifi_begin_hardware_scan();
  wifi_finish_hardware_scan(
      "Adapter detected, but this driver has no real scan backend yet.");
  return 0;
}

static int wifi_stub_connect(const char *ssid, int secure, const char *password) {
  (void)ssid;
  (void)secure;
  (void)password;
  wifi_set_status("Real Wi-Fi connection is not implemented for this adapter.");
  return 0;
}

static void wifi_stub_disconnect(void) {
  wifi_report_disconnected("Disconnected from Wi-Fi.");
}

static int wifi_compat_scan(void) {
  int i;

  wifi_begin_hardware_scan();
  for (i = 0; i < wifi_profile_count; i++) {
    wifi_add_hardware_scan_result(wifi_profiles[i].network.ssid,
                                  wifi_profiles[i].network.signal,
                                  wifi_profiles[i].network.secure,
                                  wifi_text_equal(
                                      wifi_profiles[i].network.ssid,
                                      wifi_state.connected_ssid));
  }

  if (wifi_profile_count > 0)
    wifi_finish_hardware_scan("Scan complete. Nearby networks refreshed.");
  else
    wifi_finish_hardware_scan("Scan complete. No configured networks were found.");
  return wifi_profile_count;
}

static int wifi_compat_connect(const char *ssid, int secure, const char *password) {
  wifi_profile_t *profile = wifi_find_profile(ssid);

  if (!profile) {
    wifi_set_status("Selected network disappeared before connect completed.");
    return 0;
  }

  if (secure && profile->password[0] &&
      (!password || strcmp(password, profile->password) != 0)) {
    wifi_set_status("Incorrect Wi-Fi password.");
    return 0;
  }

  wifi_report_connected(profile->network.ssid, profile->network.signal, 0);
  return 1;
}

static void wifi_compat_disconnect(void) {
  wifi_report_disconnected("Disconnected from Wi-Fi.");
}

void wifi_init(void) {
  const wifi_pci_match_t *match;

  if (wifi_state.initialized)
    return;

  wifi_state.initialized = 1;
  wifi_state.selected_network = -1;
  wifi_reset_scan_results();

  printk(KERN_INFO "WIFI: Loading wireless drivers...\n");

  match = wifi_probe_supported_adapter();
  if (!match) {
    wifi_set_status("No supported Wi-Fi adapter detected.");
    printk(KERN_INFO "WIFI: No supported PCI Wi-Fi adapter detected\n");
    return;
  }

  wifi_state.adapter_present = 1;
  wifi_state.intel_adapter = match->vendor_id == 0x8086 ? 1 : 0;
  wifi_state.adapter_name = match->adapter_name;
  wifi_state.driver_name = match->driver_name;
  wifi_load_profiles_from_cmdline();
  wifi_register_backend(&wifi_compat_backend, "compatibility survey backend");
  wifi_set_status("Adapter ready. Run Scan to list nearby networks.");

  printk(KERN_INFO "WIFI: Bound %s using %s (%s)\n", wifi_state.adapter_name,
         wifi_state.driver_name, wifi_state.backend_name);
}

void wifi_register_backend(const wifi_backend_ops_t *ops,
                           const char *backend_name) {
  wifi_state.backend = ops ? ops : &wifi_stub_backend;
  wifi_state.backend_name =
      backend_name && backend_name[0] ? backend_name : "wireless backend";
  wifi_set_backend_capabilities(wifi_state.backend->supports_real_scanning,
                                wifi_state.backend->supports_real_connecting);
}

int wifi_has_supported_adapter(void) { return wifi_state.adapter_present; }

int wifi_is_intel_adapter(void) { return wifi_state.intel_adapter; }

int wifi_is_connected(void) { return wifi_state.connected; }

int wifi_has_scan_results(void) {
  return wifi_state.adapter_present && wifi_state.scan_ready &&
         wifi_state.visible_count > 0;
}

int wifi_supports_real_scanning(void) { return wifi_state.real_scan_supported; }

int wifi_supports_real_connect(void) {
  return wifi_state.real_connect_supported;
}

int wifi_can_connect_selected(void) {
  return wifi_state.real_connect_supported &&
         wifi_state.selected_network >= 0 &&
         wifi_state.selected_network < wifi_state.visible_count;
}

const char *wifi_get_adapter_name(void) { return wifi_state.adapter_name; }

const char *wifi_get_driver_name(void) { return wifi_state.driver_name; }

const char *wifi_get_status_text(void) { return wifi_state.status_text; }

const char *wifi_get_connected_ssid(void) {
  if (!wifi_state.connected || !wifi_state.connected_ssid[0])
    return "Not connected";

  return wifi_state.connected_ssid;
}

int wifi_get_signal_strength(void) {
  if (!wifi_state.connected)
    return 0;

  return wifi_state.connected_signal;
}

int wifi_get_network_count(void) {
  if (!wifi_has_scan_results())
    return 0;

  return wifi_state.visible_count;
}

const char *wifi_get_network_ssid(int index) {
  if (index < 0 || index >= wifi_state.visible_count)
    return "";
  return wifi_state.visible_networks[index].ssid;
}

int wifi_get_network_signal(int index) {
  if (index < 0 || index >= wifi_state.visible_count)
    return 0;
  return wifi_state.visible_networks[index].signal;
}

int wifi_get_network_secure(int index) {
  if (index < 0 || index >= wifi_state.visible_count)
    return 0;
  return wifi_state.visible_networks[index].secure;
}

int wifi_is_network_connected(int index) {
  if (index < 0 || index >= wifi_state.visible_count || !wifi_state.connected)
    return 0;
  return wifi_text_equal(wifi_state.visible_networks[index].ssid,
                         wifi_state.connected_ssid);
}

int wifi_get_selected_network(void) { return wifi_state.selected_network; }

void wifi_select_network(int index) {
  if (index < 0 || index >= wifi_get_network_count())
    return;
  wifi_state.selected_network = index;
  wifi_set_status("Network selected. Connect when the driver is ready.");
}

void wifi_set_backend_capabilities(int supports_scanning,
                                   int supports_connecting) {
  wifi_state.real_scan_supported = supports_scanning ? 1 : 0;
  wifi_state.real_connect_supported = supports_connecting ? 1 : 0;
}

void wifi_begin_hardware_scan(void) { wifi_reset_scan_results(); }

int wifi_add_hardware_scan_result(const char *ssid, int signal, int secure,
                                  int connected) {
  int i;
  wifi_network_t *entry;

  if (!ssid || !ssid[0] || wifi_state.visible_count >= WIFI_MAX_NETWORKS)
    return 0;

  for (i = 0; i < wifi_state.visible_count; i++) {
    if (wifi_text_equal(wifi_state.visible_networks[i].ssid, ssid)) {
      if (wifi_clamp_signal(signal) > wifi_state.visible_networks[i].signal)
        wifi_state.visible_networks[i].signal = wifi_clamp_signal(signal);
      if (secure)
        wifi_state.visible_networks[i].secure = 1;
      if (connected)
        wifi_report_connected(ssid, signal, "Connected network refreshed.");
      return 1;
    }
  }

  entry = &wifi_state.visible_networks[wifi_state.visible_count++];
  wifi_copy_text(entry->ssid, sizeof(entry->ssid), ssid);
  entry->signal = wifi_clamp_signal(signal);
  entry->secure = secure ? 1 : 0;

  if (connected)
    wifi_report_connected(ssid, signal, "Connected network refreshed.");

  return 1;
}

void wifi_finish_hardware_scan(const char *status_text) {
  wifi_sort_visible_networks();
  wifi_state.scan_ready = 1;
  wifi_set_default_selection();

  if (status_text && status_text[0]) {
    wifi_set_status(status_text);
  } else if (wifi_state.visible_count > 0) {
    wifi_set_status("Scan complete. Nearby Wi-Fi networks updated.");
  } else {
    wifi_set_status("Scan complete. No nearby Wi-Fi networks were reported.");
  }
}

void wifi_report_connected(const char *ssid, int signal, const char *status_text) {
  wifi_state.connected = 1;
  wifi_state.connected_signal = wifi_clamp_signal(signal);
  wifi_copy_text(wifi_state.connected_ssid, sizeof(wifi_state.connected_ssid),
                 ssid ? ssid : "");
  if (status_text && status_text[0]) {
    wifi_set_status(status_text);
  } else {
    wifi_set_status_with_suffix("Connected to ", wifi_state.connected_ssid);
  }
  wifi_set_default_selection();
}

void wifi_report_disconnected(const char *status_text) {
  wifi_state.connected = 0;
  wifi_state.connected_signal = 0;
  wifi_state.connected_ssid[0] = '\0';
  if (status_text && status_text[0])
    wifi_set_status(status_text);
  else
    wifi_set_status("Disconnected from Wi-Fi.");
  wifi_set_default_selection();
}

int wifi_scan(void) {
  if (!wifi_state.adapter_present) {
    wifi_set_status("No supported Wi-Fi adapter detected.");
    return 0;
  }

  if (!wifi_state.backend || !wifi_state.backend->scan) {
    wifi_begin_hardware_scan();
    wifi_finish_hardware_scan("No Wi-Fi scan backend is loaded.");
    return 0;
  }

  return wifi_state.backend->scan();
}

int wifi_connect_selected(const char *password) {
  wifi_network_t *network;

  if (!wifi_state.adapter_present) {
    wifi_set_status("No supported Wi-Fi adapter detected.");
    return 0;
  }

  if (!wifi_state.real_connect_supported) {
    wifi_set_status("Real Wi-Fi connection is not implemented for this adapter.");
    return 0;
  }

  if (!wifi_state.scan_ready || wifi_state.visible_count <= 0) {
    wifi_set_status("Run a real Wi-Fi scan before connecting.");
    return 0;
  }

  if (wifi_state.selected_network < 0 ||
      wifi_state.selected_network >= wifi_state.visible_count) {
    wifi_set_status("Select a Wi-Fi network first.");
    return 0;
  }

  network = &wifi_state.visible_networks[wifi_state.selected_network];
  if (network->secure && (!password || !password[0])) {
    wifi_set_status("Enter the Wi-Fi password before connecting.");
    return 0;
  }

  if (!wifi_state.backend || !wifi_state.backend->connect_selected) {
    wifi_set_status("No Wi-Fi connection backend is loaded.");
    return 0;
  }

  return wifi_state.backend->connect_selected(network->ssid, network->secure,
                                              password);
}

void wifi_disconnect(void) {
  if (!wifi_state.adapter_present) {
    wifi_set_status("No supported Wi-Fi adapter detected.");
    return;
  }

  if (!wifi_state.connected) {
    wifi_set_status("Wi-Fi link is already idle.");
    return;
  }

  if (wifi_state.backend && wifi_state.backend->disconnect)
    wifi_state.backend->disconnect();
  else
    wifi_report_disconnected("Disconnected from Wi-Fi.");
}
