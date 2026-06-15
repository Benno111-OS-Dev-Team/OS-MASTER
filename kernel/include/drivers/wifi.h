/*
 * OS8 - Wi-Fi Driver Abstraction
 *
 * Wireless support used by Settings and system status surfaces.
 * This layer detects a small set of supported adapters and exposes only
 * hardware-backed scan/connect state; it no longer fabricates nearby networks.
 */

#ifndef DRIVERS_WIFI_H
#define DRIVERS_WIFI_H

#include "types.h"

#define WIFI_MAX_SSID_LEN 32

typedef struct {
  int supports_real_scanning;
  int supports_real_connecting;
  int (*scan)(void);
  int (*connect_selected)(const char *ssid, int secure,
                          const char *password);
  void (*disconnect)(void);
} wifi_backend_ops_t;

void wifi_init(void);
void wifi_register_backend(const wifi_backend_ops_t *ops,
                           const char *backend_name);
int wifi_has_supported_adapter(void);
int wifi_is_intel_adapter(void);
int wifi_is_connected(void);
int wifi_has_scan_results(void);
int wifi_supports_real_scanning(void);
int wifi_supports_real_connect(void);
int wifi_can_connect_selected(void);
const char *wifi_get_adapter_name(void);
const char *wifi_get_driver_name(void);
const char *wifi_get_status_text(void);
const char *wifi_get_connected_ssid(void);
int wifi_get_signal_strength(void);
int wifi_get_network_count(void);
const char *wifi_get_network_ssid(int index);
int wifi_get_network_signal(int index);
int wifi_get_network_secure(int index);
int wifi_is_network_connected(int index);
int wifi_get_selected_network(void);
void wifi_select_network(int index);
int wifi_scan(void);
int wifi_connect_selected(const char *password);
void wifi_disconnect(void);
void wifi_set_backend_capabilities(int supports_scanning,
                                   int supports_connecting);
void wifi_begin_hardware_scan(void);
int wifi_add_hardware_scan_result(const char *ssid, int signal, int secure,
                                  int connected);
void wifi_finish_hardware_scan(const char *status_text);
void wifi_report_connected(const char *ssid, int signal,
                           const char *status_text);
void wifi_report_disconnected(const char *status_text);

#endif
