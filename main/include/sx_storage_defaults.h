#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX_NVS_NAMESPACE "storage"
#define SX_NVS_SCHEMA_KEY "cfg_schema_ver"
#define SX_NVS_SCHEMA_VERSION "2"
#define SX_NVS_HW_PROFILE_KEY "hw_profile"
#define SX_NVS_HW_PROFILE_VALUE "SP603_MULTI_NETWORK_GATEWAY"

/* Add missing factory defaults without overwriting values saved by the user. */
esp_err_t sx_storage_defaults_init(void);

#ifdef __cplusplus
}
#endif
