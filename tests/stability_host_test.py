#!/usr/bin/env python3
"""Fault-inject production lifecycle, configuration and transmit helpers.

Requires Python 3, cc and pthreads. SDK/task/socket stubs exercise ownership
and error paths; they do not replace ESP32 hardware or on-wire tests.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:static )?[A-Za-z_][^\n;]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError(name)
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


COMMON = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
typedef int esp_err_t;
typedef int BaseType_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG,
       ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FOUND };
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e_=(expr); if(e_) return e_; } while(0)
#define ESP_RETURN_ON_FALSE(cond, err, ...) do { if(!(cond)) return err; } while(0)
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY -1
#define pdMS_TO_TICKS(x) (x)
typedef pthread_mutex_t *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t m=malloc(sizeof(*m)); assert(m);
    assert(pthread_mutex_init(m, NULL)==0); return m;
}
static int take_fails;
static int xSemaphoreTake(SemaphoreHandle_t m, int timeout) {
    (void)timeout; if(take_fails) return 0;
    assert(m); assert(pthread_mutex_lock(m)==0); return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(pthread_mutex_unlock(m)==0); }
static void vSemaphoreDelete(SemaphoreHandle_t m) { assert(pthread_mutex_destroy(m)==0); free(m); }
'''


TX_MOCK = r'''
#define SERIAL_PORT_COUNT 3
#define ASYNC_UART_BUF_SIZE 5120
#define SERIAL_TX_TIMEOUT_US 5000000LL
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct { size_t len, offset; int64_t deadline_us; uint8_t data[]; } serial_tx_frame_t;
typedef struct { serial_tx_frame_t *items[8]; unsigned count; } queue_t;
typedef queue_t *QueueHandle_t;
static QueueHandle_t s_tx_queues[4];
static SemaphoreHandle_t s_tx_locks[4];
static bool s_tx_accepting[4];
static atomic_bool s_tx_failed[4], s_serial_server_running;
static uint32_t s_tx_generation[4];
static uint8_t s_mqtt_rx_buffers[4][512];
static size_t s_mqtt_rx_lengths[4], s_mqtt_rx_totals[4];
static int64_t now;
static bool alloc_fails;
static int64_t esp_timer_get_time(void) { return now; }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return alloc_fails ? NULL : malloc(n); }
static unsigned uxQueueSpacesAvailable(QueueHandle_t q) { return 8-q->count; }
static int xQueueSend(QueueHandle_t q, serial_tx_frame_t **f, int timeout) {
    (void)timeout; if(q->count==8) return 0; q->items[q->count++]=*f; return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, serial_tx_frame_t **f, int timeout) {
    (void)timeout; if(!q->count) return 0; *f=q->items[0]; --q->count;
    memmove(q->items, q->items+1, q->count*sizeof(*f)); return pdTRUE;
}
static int send_result, send_errno, send_calls;
static uint8_t sent[32]; static size_t sent_len;
static int mock_send(int fd, const void *data, size_t size, int flags) {
    (void)fd; (void)flags; ++send_calls; errno=send_errno;
    if(send_result>0) { assert((size_t)send_result<=size); memcpy(sent+sent_len,data,send_result); sent_len+=send_result; }
    return send_result;
}
#define send mock_send
'''

TX_TEST = r'''
int main(void) {
    queue_t q={0}; s_tx_queues[1]=&q; s_tx_locks[1]=xSemaphoreCreateMutex();
    atomic_store(&s_serial_server_running,true); set_tx_active(1,true);
    uint32_t gen=s_tx_generation[1]; uint8_t data[]={1,2,3,4,5,6};
    assert(queue_network_tx(1,data,sizeof(data),gen)); data[0]=99;
    serial_tx_frame_t *f=NULL; assert(xQueueReceive(&q,&f,0)); assert(f->data[0]==1);
    send_result=2; assert(flush_tcp_frame(7,f)==0 && f->offset==2);
    send_result=-1; send_errno=EAGAIN; assert(flush_tcp_frame(7,f)==0 && f->offset==2);
    send_errno=EINTR; assert(flush_tcp_frame(7,f)==0 && f->offset==2);
    send_result=4; assert(flush_tcp_frame(7,f)==1 && f->offset==6);
    uint8_t expected[]={1,2,3,4,5,6}; assert(sent_len==6 && !memcmp(sent,expected,6));
    f->offset=0; send_result=-1; send_errno=ECONNRESET; assert(flush_tcp_frame(7,f)==-1);
    send_result=0; assert(flush_tcp_frame(7,f)==-1);
    now=f->deadline_us; int calls=send_calls; assert(flush_tcp_frame(7,f)==-1 && send_calls==calls);
    free(f); now=0;
    puts("PASS TCP short writes/backpressure/interruption preserve bytes; hard errors and deadlines fail");
    for(int i=0;i<8;++i) assert(queue_network_tx(1,data,6,gen));
    assert(!queue_network_tx(1,data,6,gen) && network_tx_failed(1));
    set_tx_active(1,false); assert(q.count==0 && !network_tx_failed(1));
    assert(!queue_network_tx(1,data,6,gen) && q.count==0);
    set_tx_active(1,true); assert(!queue_network_tx(1,data,6,gen) && !network_tx_failed(1));
    gen=s_tx_generation[1]; alloc_fails=true;
    assert(!queue_network_tx(1,data,6,gen) && network_tx_failed(1));
    alloc_fails=false; set_tx_active(1,true); take_fails=1;
    assert(!queue_network_tx(1,data,6,s_tx_generation[1]) && atomic_load(&s_tx_failed[1]));
    take_fails=0; set_tx_active(1,false); vSemaphoreDelete(s_tx_locks[1]);
    puts("PASS queue bounds, allocation/lock failure, offline discard and stale connection rejection");
    assert(append_mqtt_fragment(1,"abc",3,6,0)==0);
    assert(append_mqtt_fragment(1,"def",3,6,3)==6 && !memcmp(s_mqtt_rx_buffers[1],"abcdef",6));
    assert(append_mqtt_fragment(1,"abc",3,6,0)==0);
    assert(append_mqtt_fragment(1,"ef",2,6,4)==0 && s_mqtt_rx_totals[1]==0);
    assert(append_mqtt_fragment(1,"def",3,6,3)==0);
    assert(append_mqtt_fragment(1,"abc",3,6,0)==0);
    assert(append_mqtt_fragment(1,"def",3,7,3)==0 && s_mqtt_rx_totals[1]==0);
    assert(append_mqtt_fragment(1,NULL,1,1,0)==0);
    assert(append_mqtt_fragment(1,"x",1,513,0)==0);
    assert(append_mqtt_fragment(1,"xx",2,1,0)==0);
    assert(append_mqtt_fragment(1,"x",1,1,-1)==0);
    assert(append_mqtt_fragment(1,"ok",2,2,0)==2);
    puts("PASS MQTT complete/contiguous fragments accepted; gaps, changed totals and bounds rejected");
}
'''

MODE_MOCK = r'''
#define SX_SERIAL_LAYOUT_DUAL_RS485 1
typedef void (*sx_work_mode_uart_handler_t)(int,const uint8_t *,size_t);
static int layout=1, start_serial, stop_serial, start_auto, stop_auto;
static int start_error, stop_error;
static int sx_serial_server_init(void) { return ESP_OK; }
static int sx_serial_port_manager_get_layout(void) { return layout; }
static bool sx_serial_port_manager_port_available(int p) { return p==1 || p==3; }
static int sx_serial_server_start(void) { ++start_serial; return start_error; }
static int sx_serial_server_stop(void) { ++stop_serial; return stop_error; }
static int sx_auto_collect_init(void) { ++start_auto; return start_error; }
static int sx_auto_collect_deinit(void) { ++stop_auto; return stop_error; }
static void sx_serial_server_handle_uart_data(int p,const uint8_t *d,size_t n) { (void)p;(void)d;(void)n; }
'''

MODE_TEST = r'''
int main(void) {
    assert(sx_work_mode_start_by_name("bad")==ESP_ERR_NOT_FOUND);
    assert(sx_work_mode_start_by_name("serial_server")==ESP_OK && start_serial==1);
    assert(sx_work_mode_start_by_name("serial_server")==ESP_OK && start_serial==1);
    assert(sx_work_mode_reload_auto_collect()==ESP_OK && stop_serial==0);
    stop_error=ESP_ERR_TIMEOUT;
    assert(sx_work_mode_start_by_name("auto_collect")==ESP_ERR_TIMEOUT && start_auto==0);
    assert(!strcmp(sx_work_mode_get_current_name(),"serial_server"));
    assert(sx_work_mode_get_uart_handler()==NULL);
    stop_error=0; assert(sx_work_mode_start_by_name("auto_collect")==ESP_OK && start_auto==1);
    assert(sx_work_mode_reload_auto_collect()==ESP_OK && start_auto==2 && stop_auto==1);
    assert(sx_work_mode_stop_current()==ESP_OK && sx_work_mode_get_current_name()==NULL);
    start_error=ESP_ERR_NO_MEM; stop_error=ESP_ERR_TIMEOUT;
    assert(sx_work_mode_start_by_name("serial_server")==ESP_ERR_NO_MEM);
    assert(!strcmp(sx_work_mode_get_current_name(),"serial_server") && !atomic_load(&s_mode_started));
    int starts=start_auto;
    assert(sx_work_mode_start_by_name("auto_collect")==ESP_ERR_TIMEOUT && start_auto==starts);
    start_error=stop_error=0;
    assert(sx_work_mode_start_by_name("serial_server")==ESP_OK && sx_work_mode_get_uart_handler()!=NULL);
    assert(sx_work_mode_stop_current()==ESP_OK);
    start_error=ESP_FAIL; assert(sx_work_mode_start_by_name("auto_collect")==ESP_FAIL);
    assert(sx_work_mode_get_current_name()==NULL);
    start_error=0; layout=0; assert(sx_work_mode_start_by_name("auto_collect")==ESP_ERR_NOT_FOUND);
    vSemaphoreDelete(s_mode_mutex);
    puts("PASS mode idempotence, stop timeout ownership/retry, failed-start cleanup and conditional reload");
}
'''

NETWORK_MOCK = r'''
typedef struct { int ethernet_role; bool routing_enabled; int value; } sx_network_config_t;
static sx_network_config_t s_config, s_saved_config;
static struct { bool routing_enabled, reboot_required; } s_status;
static SemaphoreHandle_t s_mutex, s_save_mutex;
static int save_error;
static sx_network_config_t persisted;
static void sx_network_config_normalize(sx_network_config_t *c) { (void)c; }
static int sx_network_config_save(const sx_network_config_t *c) {
    if(save_error) return save_error;
    persisted=*c; return ESP_OK;
}
'''

NETWORK_TEST = r'''
static void *writer(void *arg) {
    int id=(int)(intptr_t)arg;
    for(int i=0;i<1000;++i) {
        sx_network_config_t c={.ethernet_role=id,.routing_enabled=true,.value=i};
        assert(sx_network_manager_save_config(&c)==ESP_OK);
    }
    return NULL;
}
int main(void) {
    s_mutex=xSemaphoreCreateMutex(); s_save_mutex=xSemaphoreCreateMutex();
    s_config=(sx_network_config_t){.ethernet_role=1,.routing_enabled=false}; s_saved_config=s_config;
    sx_network_config_t changed={.ethernet_role=3,.routing_enabled=true,.value=17}, saved, active;
    save_error=ESP_FAIL; assert(sx_network_manager_save_config(&changed)==ESP_FAIL);
    assert(s_saved_config.ethernet_role==1 && !s_status.reboot_required);
    save_error=0; assert(sx_network_manager_save_config(&changed)==ESP_OK);
    sx_network_manager_get_config(&saved); sx_network_manager_get_active_config(&active);
    assert(saved.ethernet_role==3 && active.ethernet_role==1);
    assert(!active.routing_enabled && !s_status.routing_enabled && s_status.reboot_required);
    assert(sx_network_manager_save_config(NULL)==ESP_ERR_INVALID_ARG);
    pthread_t a,b; assert(!pthread_create(&a,NULL,writer,(void *)1));
    assert(!pthread_create(&b,NULL,writer,(void *)2)); pthread_join(a,NULL); pthread_join(b,NULL);
    assert(s_saved_config.ethernet_role==persisted.ethernet_role && s_saved_config.value==persisted.value);
    assert(s_config.ethernet_role==1 && !s_config.routing_enabled);
    vSemaphoreDelete(s_mutex); vSemaphoreDelete(s_save_mutex);
    puts("PASS pending network save does not alter active roles/routes; failure and concurrent saves preserve snapshots");
}
'''

DNS_MOCK = r'''
typedef int sx_network_interface_t;
enum { SX_NETWORK_IF_NONE, SX_NETWORK_IF_WIFI, SX_NETWORK_IF_W5500, SX_NETWORK_IF_4G };
enum { ESP_IPADDR_TYPE_V4, ESP_IPADDR_TYPE_V6, ESP_NETIF_DNS_MAIN = 0 };
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { struct { int type; union { ip4_addr_t ip4; } u_addr; } ip; } esp_netif_dns_info_t;
typedef int esp_netif_t;
static struct { char active_dns[16]; bool dns_ready; } s_status;
static struct { char wifi_sta_dns[16], ethernet_dns[16]; } s_config;
static int get_error, set_error, set_calls;
static esp_netif_dns_info_t offered;
static int esp_netif_get_dns_info(esp_netif_t *n, int t, esp_netif_dns_info_t *d) {
    (void)n; (void)t; *d=offered; return get_error;
}
static int esp_netif_set_dns_info(esp_netif_t *n, int t, esp_netif_dns_info_t *d) {
    (void)n; (void)t; (void)d; ++set_calls; return set_error;
}
static int ip4addr_aton(const char *s, ip4_addr_t *a) { return inet_pton(AF_INET,s,&a->addr); }
static void inet_ntoa_r(ip4_addr_t a, char *out, size_t n) { assert(inet_ntop(AF_INET,&a.addr,out,n)); }
static const char *sx_network_interface_name(int id) { (void)id; return "test"; }
static void send_system_log(const char *a,const char *b,const char *c) { (void)a;(void)b;(void)c; }
'''

DNS_TEST = r'''
int main(void) {
    esp_netif_t netif=1;
    strcpy(s_config.ethernet_dns,"8.8.8.8");
    set_error=ESP_FAIL;
    refresh_active_dns_locked(SX_NETWORK_IF_W5500,&netif);
    assert(!s_status.dns_ready && !s_status.active_dns[0] && set_calls==1);
    set_error=0;
    refresh_active_dns_locked(SX_NETWORK_IF_W5500,&netif);
    assert(s_status.dns_ready && strcmp(s_status.active_dns,"8.8.8.8")==0);
    assert(ip4addr_aton("1.1.1.1",&offered.ip.u_addr.ip4));
    refresh_active_dns_locked(SX_NETWORK_IF_W5500,&netif);
    assert(strcmp(s_status.active_dns,"1.1.1.1")==0 && set_calls==2);
    get_error=ESP_FAIL; s_config.ethernet_dns[0]=0;
    refresh_active_dns_locked(SX_NETWORK_IF_W5500,&netif);
    assert(!s_status.dns_ready && !s_status.active_dns[0]);
    get_error=0; offered.ip.u_addr.ip4.addr=0;
    strcpy(s_config.ethernet_dns,"0.0.0.0");
    refresh_active_dns_locked(SX_NETWORK_IF_W5500,&netif);
    assert(!s_status.dns_ready);
    refresh_active_dns_locked(SX_NETWORK_IF_NONE,&netif);
    assert(!s_status.dns_ready);
    refresh_active_dns_locked(SX_NETWORK_IF_W5500,NULL);
    assert(!s_status.dns_ready);
    puts("PASS DNS readiness requires successful read/install; fallback failure cannot report usable DNS");
}
'''

STARTUP_MOCK = r'''
enum { SX_NETWORK_IF_NONE, SX_LED_NETWORK_OFF, SX_LED_SYSTEM_WARNING, SX_LED_SYSTEM_NORMAL };
static SemaphoreHandle_t s_mutex, s_start_mutex;
static bool s_management_started, s_application_ready=true;
static bool s_wifi_start_error, s_w5500_start_error, s_modem_start_error;
static int s_management_start_result, wifi_error, timer_error, stop_error, wifi_calls, system_state;
static int64_t s_wifi_ap_timeout_deadline_us;
static struct { bool wifi_ap_enabled, wifi_sta_enabled, wifi_sta_static, wifi_ap_dhcp_enabled;
    char wifi_ssid[1],wifi_password[1],wifi_sta_ip[1],wifi_sta_netmask[1],wifi_sta_gateway[1],
         wifi_sta_dns[1],ap_ip[1],ap_netmask[1]; } s_config;
static struct { bool wifi_ap_started,wifi_sta_started,wifi_ap_timed_out,dns_ready;
    int wifi_ap_client_count,active_interface; } s_status;
static int sx_wifi_start_configured(bool ap, ...) { (void)ap;++wifi_calls;return wifi_error; }
static int schedule_wifi_ap_timeout_locked(void) { return timer_error; }
static bool has_configured_uplink_locked(void) { return false; }
static void update_network_leds_locked(void) {}
static void sx_led_manager_set_wifi(int state) { (void)state; }
static void sx_led_manager_set_system_state(int state) { system_state=state; }
static int sx_wifi_stop_ap(bool sta) { (void)sta;return stop_error; }
static void send_system_log(const char *a,const char *b,const char *c) { (void)a;(void)b;(void)c; }
'''

STARTUP_TEST = r'''
int main(void) {
    s_mutex=xSemaphoreCreateMutex();s_start_mutex=xSemaphoreCreateMutex();
    s_config.wifi_ap_enabled=true;
    timer_error=ESP_FAIL;
    assert(sx_network_manager_start_management_network()==ESP_FAIL);
    assert(s_status.wifi_ap_started && s_wifi_start_error && system_state==SX_LED_SYSTEM_WARNING);
    update_system_indicator_locked();
    assert(system_state==SX_LED_SYSTEM_WARNING);
    timer_error=0;
    assert(sx_network_manager_start_management_network()==ESP_FAIL && wifi_calls==1);
    s_management_started=false;wifi_error=ESP_FAIL;
    assert(sx_network_manager_start_management_network()==ESP_FAIL && s_wifi_start_error);
    s_management_started=false;wifi_error=0;
    assert(sx_network_manager_start_management_network()==ESP_OK && !s_wifi_start_error);
    assert(system_state==SX_LED_SYSTEM_NORMAL);
    s_wifi_ap_timeout_deadline_us=1;stop_error=ESP_FAIL;
    wifi_ap_timeout_cb(NULL);
    assert(s_status.wifi_ap_started && !s_wifi_ap_timeout_deadline_us && s_wifi_start_error);
    assert(system_state==SX_LED_SYSTEM_WARNING);
    vSemaphoreDelete(s_mutex);vSemaphoreDelete(s_start_mutex);
    puts("PASS Wi-Fi start/AP timer failures retain warning and repeat-start result; timeout-stop failure refreshes warning");
}
'''

TASK_MOCK = r'''
#define SERIAL_PORT_COUNT 3
#define tskNO_AFFINITY -1
static atomic_bool s_serial_server_running, s_port_task_active[4];
static SemaphoreHandle_t s_lifecycle_mutex;
static int create_calls, create_failure, delays;
static bool stuck;
static bool sx_serial_port_manager_port_available(int p) { return p==1 || p==2; }
static void serial_port_task(void *arg) { (void)arg; }
static void vTaskDelay(int ticks) {
    (void)ticks; ++delays;
    if(!stuck && !atomic_load(&s_serial_server_running))
        for(int p=1;p<=3;++p) atomic_store(&s_port_task_active[p],false);
}
static int create_app_task_psram(void (*task)(void *), const char *name, int size,
                                void *arg, int priority, void *handle, int affinity) {
    (void)task;(void)name;(void)size;(void)arg;(void)priority;(void)handle;(void)affinity;
    return ++create_calls==create_failure ? 0 : pdPASS;
}
'''

TASK_TEST = r'''
int main(void) {
    s_lifecycle_mutex=xSemaphoreCreateMutex(); create_failure=2;
    assert(sx_serial_server_start()==ESP_ERR_NO_MEM && create_calls==2);
    assert(!s_serial_server_running && !s_port_task_active[1] && !s_port_task_active[2]);
    create_failure=0; assert(sx_serial_server_start()==ESP_OK);
    assert(s_port_task_active[1] && s_port_task_active[2] && !s_port_task_active[3]);
    int calls=create_calls; assert(sx_serial_server_start()==ESP_OK && calls==create_calls);
    stuck=true; assert(sx_serial_server_stop()==ESP_ERR_TIMEOUT);
    assert(s_port_task_active[1] && !s_serial_server_running);
    assert(sx_serial_server_start()==ESP_ERR_INVALID_STATE && calls==create_calls);
    stuck=false; assert(sx_serial_server_stop()==ESP_OK);
    assert(sx_serial_server_start()==ESP_OK && create_calls==calls+2);
    assert(sx_serial_server_stop()==ESP_OK);
    vSemaphoreDelete(s_lifecycle_mutex);
    puts("PASS serial partial task creation rollback, idempotent start and stop-timeout ownership/retry");
}
'''

AUTO_MOCK = r'''
#define AC_COLLECT_PORT_COUNT 2
#define AC_MAX_ITEMS_PER_PORT 2
#define NVS_READONLY 0
#define tskNO_AFFINITY -1
typedef int nvs_handle_t;
typedef void *TaskHandle_t;
typedef struct { int baudrate; } serial_port_config_t;
typedef struct { int dummy; } ac_item_config_t;
typedef struct { uint8_t mapped_slave_addr; int items_count; ac_item_config_t items[2]; } ac_port_config_t;
static int create_calls, create_failure, restored[4], deleted_mutexes;
static bool stuck, restore_fails;
static int nvs_open(const char *ns, int mode, nvs_handle_t *n) { (void)ns;(void)mode; *n=1; return 0; }
static void nvs_close(nvs_handle_t n) { assert(n==1); }
static void load_port_config_from_nvs(nvs_handle_t n, const char *key, int p, ac_port_config_t *c) {
    (void)n;(void)key;(void)p; memset(c,0,sizeof(*c)); c->items_count=1;
}
static int get_current_runtime_config(int p, serial_port_config_t *c) { c->baudrate=9600+p; return 0; }
static int quick_reconfigure_port(int p, const serial_port_config_t *c) {
    if(restore_fails) return ESP_FAIL;
    assert(c->baudrate==9600+p); ++restored[p]; return 0;
}
static void ac_polling_task(void *arg) { (void)arg; }
static int create_app_task_psram(void (*task)(void *), const char *name, int size,
                                void *arg, int priority, TaskHandle_t *handle, int affinity) {
    (void)task;(void)name;(void)size;(void)arg;(void)priority;(void)handle;(void)affinity;
    return ++create_calls==create_failure ? 0 : pdPASS;
}
'''

AUTO_TEST = r'''
static void vTaskDelay(int ticks) {
    (void)ticks;
    if(!stuck && !s_running)
        for(int i=0;i<2;++i) atomic_store(&s_runtime[i].exited,true);
}
PRODUCTION
int main(void) {
    create_failure=2;
    assert(sx_auto_collect_init()==ESP_ERR_NO_MEM && !s_running);
    assert(!s_runtime[0].mutex && !s_runtime[1].mutex && restored[1]==1 && restored[3]==1);
    create_failure=0;
    assert(sx_auto_collect_init()==ESP_OK && s_running && s_runtime[1].task_created);
    stuck=true;
    SemaphoreHandle_t m=s_runtime[0].mutex;
    assert(sx_auto_collect_deinit()==ESP_ERR_TIMEOUT && s_runtime[0].mutex==m);
    stuck=false; restore_fails=true;
    assert(sx_auto_collect_deinit()==ESP_FAIL && s_runtime[0].saved_uart_valid && s_runtime[0].mutex==m);
    restore_fails=false;
    assert(sx_auto_collect_deinit()==ESP_OK && !s_runtime[0].mutex && !s_runtime[1].mutex);
    assert(sx_auto_collect_deinit()==ESP_OK);
    puts("PASS auto-collect explicit first start, partial-start cleanup, cooperative timeout and UART restoration retry");
}
'''

CONFIG_MOCK = r'''
#include "sx_serial_server.h"
#define SERIAL_PORT_COUNT 3
#define SERIAL_SERVER_DEFAULT_PORT 8888
#define ASYNC_UART_BUF_SIZE 5120
#define SX_SERIAL_LAYOUT_RS422 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
static int layout=SX_SERIAL_LAYOUT_RS422, fail_write, write_calls, commits, closes, commit_error;
static uint16_t s_tcp_ports[4];
static SemaphoreHandle_t s_config_mutex;
static struct { char key[32], text[160]; unsigned value; int type; } entries[100];
static int entry_count;
static int entry(const char *key) {
    for(int i=0;i<entry_count;++i) if(!strcmp(entries[i].key,key)) return i;
    assert(entry_count<100); snprintf(entries[entry_count].key,32,"%s",key); return entry_count++;
}
static int nvs_open(const char *ns,int mode,nvs_handle_t *h) { (void)ns;(void)mode;*h=1;return 0; }
static void nvs_close(nvs_handle_t h) { assert(h==1); ++closes; }
static int nvs_commit(nvs_handle_t h) { assert(h==1); ++commits; return commit_error; }
static int nvs_set_str(nvs_handle_t h,const char *key,const char *value) {
    assert(h==1); if(++write_calls==fail_write) return ESP_FAIL;
    int i=entry(key); entries[i].type=1; snprintf(entries[i].text,160,"%s",value);return 0;
}
static int nvs_set_u32(nvs_handle_t h,const char *key,uint32_t value) {
    assert(h==1); if(++write_calls==fail_write) return ESP_FAIL;
    int i=entry(key); entries[i].type=2; entries[i].value=value;return 0;
}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value) { return nvs_set_u32(h,key,value); }
static int nvs_get_str(nvs_handle_t h,const char *key,char *value,size_t *len) {
    assert(h==1);int i=entry(key);
    if(entries[i].type!=1 || *len<strlen(entries[i].text)+1) return ESP_ERR_NOT_FOUND;
    strcpy(value,entries[i].text);*len=strlen(value)+1;return 0;
}
static int nvs_get_u32(nvs_handle_t h,const char *key,uint32_t *value) {
    assert(h==1); int i=entry(key);if(entries[i].type!=2) return ESP_ERR_NOT_FOUND;
    *value=entries[i].value;return 0;
}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *value) {
    uint32_t v;int rc=nvs_get_u32(h,key,&v);if(!rc)*value=(uint8_t)v;return rc;
}
static bool sx_serial_port_manager_port_available(int p) { return p>=1 && p<=(layout==1 ? 2 : 3); }
static int sx_serial_port_manager_get_layout(void) { return layout; }
static const char *sx_serial_port_manager_port_key(int p) { return p==1 ? "com2" : p==2 ? "rs232" : "com1"; }
static void set_serial_mqtt_defaults(int p,sx_serial_port_config_t *c) {
    (void)p;snprintf(c->mqtt_uri,sizeof(c->mqtt_uri),"mqtt://example:1883");
    snprintf(c->mqtt_publish_topic,sizeof(c->mqtt_publish_topic),"up");
    snprintf(c->mqtt_subscribe_topic,sizeof(c->mqtt_subscribe_topic),"down");
}
'''

CONFIG_TEST = r'''
int main(void) {
    s_config_mutex=xSemaphoreCreateMutex(); sx_serial_port_config_t c,read;
    assert(sx_serial_server_get_config(1,&c)==ESP_OK);
    /* Hidden internal port 3 has the same default port in dual-RS485 mode. */
    c.local_port=8888; c.stop_bit=15;
    assert(sx_serial_server_save_config(&c)==ESP_OK);
    assert(!strcmp(entries[entry("com2_stop")].text,"1.5"));
    assert(sx_serial_server_get_config(1,&read)==ESP_OK && read.stop_bit==15);
    assert(nvs_set_str(1,"com2_stop","15")==ESP_OK);
    assert(sx_serial_server_get_config(1,&read)==ESP_OK && read.stop_bit==15);
    assert(sx_serial_server_get_config(2,&read)==ESP_OK); c.local_port=read.local_port;
    assert(sx_serial_server_save_config(&c)==ESP_ERR_INVALID_ARG);
    c.local_port=8888; c.port=3; assert(sx_serial_server_save_config(&c)==ESP_ERR_INVALID_ARG);
    uint16_t ports[]={8888,8889,8888}; assert(sx_serial_server_save_ports(ports)==ESP_OK);
    ports[1]=8888; assert(sx_serial_server_save_ports(ports)==ESP_ERR_INVALID_ARG);
    c.port=1; memset(c.remote_ip,'x',sizeof(c.remote_ip));
    assert(sx_serial_server_save_config(&c)==ESP_ERR_INVALID_ARG);
    snprintf(c.remote_ip,sizeof(c.remote_ip),"bad-ip"); c.tcp_mode=SX_SERIAL_TCP_CLIENT;
    assert(sx_serial_server_save_config(&c)==ESP_ERR_INVALID_ARG);
    c.tcp_mode=SX_SERIAL_TCP_SERVER; c.remote_ip[0]=0;
    fail_write=write_calls+2; int old_commits=commits, old_closes=closes;
    assert(sx_serial_server_save_config(&c)==ESP_FAIL && commits==old_commits && closes>old_closes);
    fail_write=0; commit_error=ESP_FAIL;
    assert(sx_serial_server_save_config(&c)==ESP_FAIL); commit_error=0;
    assert(sx_serial_server_save_config(&c)==ESP_OK);
    vSemaphoreDelete(s_config_mutex);
    puts("PASS 1.5 stop-bit round trip/legacy read, RS422 hidden-port validation, bad strings/IP and NVS errors");
}
'''

W5500_INIT_MOCK = r'''
#define W5500_SPI_HOST 2
#define W5500_PIN_MOSI 1
#define W5500_PIN_MISO 2
#define W5500_PIN_SCLK 3
#define W5500_PIN_CS 4
#define W5500_PIN_INT 5
#define W5500_PIN_RST 6
#define W5500_PHY_ADDR 1
#define SPI_DMA_CH_AUTO 1
#define GPIO_MODE_OUTPUT 1
#define ESP_INTR_FLAG_LEVEL2 2
#define ETH_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define IP_EVENT_ETH_GOT_IP 3
#define IP_EVENT_ETH_LOST_IP 4
typedef struct { int mosi_io_num,miso_io_num,sclk_io_num,quadwp_io_num,quadhd_io_num,max_transfer_sz; } spi_bus_config_t;
typedef struct { int int_gpio_num; } eth_w5500_config_t;
typedef struct { int unused; } eth_mac_config_t;
typedef struct { int phy_addr,reset_gpio_num; } eth_phy_config_t;
typedef struct mac { void (*del)(struct mac *); } esp_eth_mac_t;
typedef struct phy { void (*del)(struct phy *); } esp_eth_phy_t;
typedef struct { esp_eth_mac_t *mac; esp_eth_phy_t *phy; } esp_eth_config_t;
#define ETH_W5500_DEFAULT_CONFIG(host,devcfg) {0}
#define ETH_MAC_DEFAULT_CONFIG() {0}
#define ETH_PHY_DEFAULT_CONFIG() {0}
#define ETH_DEFAULT_CONFIG(m,p) {m,p}
static bool s_initialized,s_cleanup_failed,s_isr_service_installed;
static bool s_dhcp_server_enabled,s_static_address,s_w5500_dns_valid,s_w5500_link_up;
static void *s_eth_handle,*s_eth_glue,*s_eth_netif;
static bool bus_live,mac_live,phy_live,driver_live,glue_live,netif_live,driver_started,uninstall_fails;
static int step,fail_at,registrations,stops;
static int checkpoint(void) { return ++step==fail_at ? ESP_FAIL : ESP_OK; }
static int spi_bus_initialize(int host,const spi_bus_config_t *cfg,int dma) {
    (void)host;(void)cfg;(void)dma;int err=checkpoint(); if(!err)bus_live=true;return err;
}
static int spi_bus_free(int host) { (void)host;assert(!mac_live && !phy_live);bus_live=false;return 0; }
static int gpio_install_isr_service(int flags) { (void)flags;return checkpoint(); }
static int gpio_reset_pin(int pin) { (void)pin;return checkpoint(); }
static int gpio_set_direction(int pin,int mode) { (void)pin;(void)mode;return checkpoint(); }
static int gpio_set_level(int pin,int level) { (void)pin;(void)level;return checkpoint(); }
static void vTaskDelay(int ticks) { (void)ticks; }
static void delete_mac(esp_eth_mac_t *m) { (void)m;assert(!driver_live);mac_live=false; }
static void delete_phy(esp_eth_phy_t *p) { (void)p;assert(!driver_live);phy_live=false; }
static esp_eth_mac_t mac_instance={delete_mac}; static esp_eth_phy_t phy_instance={delete_phy};
static esp_eth_mac_t *esp_eth_mac_new_w5500(const eth_w5500_config_t *c,const eth_mac_config_t *m) {
    (void)c;(void)m;if(checkpoint())return NULL;mac_live=true;return &mac_instance;
}
static esp_eth_phy_t *esp_eth_phy_new_w5500(const eth_phy_config_t *p) {
    (void)p;if(checkpoint())return NULL;phy_live=true;return &phy_instance;
}
static int esp_eth_driver_install(const esp_eth_config_t *c,void **h) {
    assert(c->mac && c->phy);int err=checkpoint();if(!err){driver_live=true;*h=(void *)1;}return err;
}
static int w5500_apply_mac(void *h) { assert(h);return checkpoint(); }
static void *esp_eth_new_netif_glue(void *h) { assert(h);if(checkpoint())return NULL;glue_live=true;return (void *)2; }
static int w5500_configure_netif(bool lan,const char *ip,const char *mask,bool fixed,const char *gw,const char *dns,bool dhcp) {
    (void)lan;(void)ip;(void)mask;(void)fixed;(void)gw;(void)dns;(void)dhcp;
    netif_live=true;s_eth_netif=(void *)3;return checkpoint();
}
static void w5500_eth_event_handler(void) {}
static void w5500_on_got_ip(void) {}
static void w5500_on_lost_ip(void) {}
static int esp_event_handler_register(int base,int id,void (*handler)(void),void *arg) {
    (void)base;(void)id;(void)handler;(void)arg;int err=checkpoint();if(!err)++registrations;return err;
}
static int esp_event_handler_unregister(int base,int id,void (*handler)(void)) {
    (void)base;(void)id;(void)handler;assert(registrations>0);--registrations;return 0;
}
static int esp_eth_start(void *h) { assert(h);driver_started=true;return checkpoint(); }
static int esp_eth_stop(void *h) { assert(h);driver_started=false;++stops;return 0; }
static int esp_eth_del_netif_glue(void *g) { assert(g && !driver_started);glue_live=false;return 0; }
static int esp_eth_driver_uninstall(void *h) {
    assert(h && !driver_started && !glue_live);
    if(uninstall_fails)return ESP_FAIL;
    driver_live=false;return 0;
}
static void esp_netif_destroy(void *n) { assert(n && !driver_live);netif_live=false; }
'''

W5500_INIT_TEST = r'''
static int init(void) { return w5500_manager_init_with_role(true,"192.168.10.1","255.255.255.0",true,NULL,"8.8.8.8",true); }
static void reset(void) {
    s_initialized=s_cleanup_failed=s_isr_service_installed=false;
    bus_live=mac_live=phy_live=driver_live=glue_live=netif_live=driver_started=false;
    s_eth_handle=s_eth_glue=s_eth_netif=NULL;step=registrations=stops=0;
}
int main(void) {
    assert(init()==ESP_OK); int last_step=step;
    assert(last_step>10 && registrations==3 && s_initialized);
    for(int failure=1;failure<=last_step;++failure) {
        reset(); fail_at=failure;
        assert(init()!=ESP_OK);
        assert(!bus_live && !mac_live && !phy_live && !driver_live && !glue_live && !netif_live);
        assert(!s_eth_handle && !s_eth_glue && !s_eth_netif && !s_initialized && !registrations);
        if(failure==last_step)assert(stops==1);
        fail_at=0;assert(init()==ESP_OK);
    }
    reset();fail_at=last_step;uninstall_fails=true;
    assert(init()!=ESP_OK && s_cleanup_failed);
    assert(driver_live && mac_live && phy_live && netif_live && bus_live && !glue_live);
    int before=step;assert(init()==ESP_ERR_INVALID_STATE && step==before);
    puts("PASS W5500 every init failure releases partial resources and allows retry; failed uninstall retains live resources");
}
'''


def run_suite(name, source):
    with tempfile.TemporaryDirectory(prefix="sp603-stability-") as directory:
        path = Path(directory)
        (path / "test.c").write_text(COMMON + source, encoding="utf-8")
        (path / "esp_err.h").write_text("/* esp_err_t provided by test */\n")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable", "-pthread",
                        "-I", str(path), "-I", str(ROOT / "main/include"),
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True, timeout=10)


def main():
    serial = (ROOT / "main/sx_serial_server.c").read_text(encoding="utf-8")
    network = (ROOT / "main/network/sx_network_manager.c").read_text(encoding="utf-8")
    mode = (ROOT / "main/sx_work_mode.c").read_text(encoding="utf-8")
    mode = mode[mode.index("static const char *TAG"):mode.index("void sx_work_mode_handle_uart_data(")]
    run_suite("transmit", TX_MOCK + "\n".join(function(serial, name) for name in [
        "set_tx_active", "queue_network_tx", "network_tx_failed", "flush_tcp_frame",
        "append_mqtt_fragment"]) + TX_TEST)
    run_suite("mode", MODE_MOCK + mode + MODE_TEST)
    run_suite("network", NETWORK_MOCK + "\n".join(function(network, name) for name in [
        "sx_network_manager_get_config", "sx_network_manager_get_active_config",
        "sx_network_manager_save_config"]) + NETWORK_TEST)
    run_suite("DNS", DNS_MOCK + "\n".join(function(network, name) for name in [
        "usable_ipv4_dns", "configured_dns_locked", "refresh_active_dns_locked"]) + DNS_TEST)
    run_suite("network startup", STARTUP_MOCK + "\n".join(function(network, name) for name in [
        "update_system_indicator_locked", "wifi_ap_timeout_cb",
        "sx_network_manager_start_management_network"]) + STARTUP_TEST)
    run_suite("serial tasks", TASK_MOCK + "\n".join(function(serial, name) for name in [
        "stop_serial_tasks_locked", "sx_serial_server_start", "sx_serial_server_stop"]) + TASK_TEST)
    auto = (ROOT / "main/sx_auto_collect.c").read_text(encoding="utf-8")
    runtime = auto[auto.index("typedef struct {"):auto.index("static uint16_t modbus_crc16(")]
    run_suite("auto collect", AUTO_MOCK + runtime + AUTO_TEST.replace("PRODUCTION", "\n".join(
        function(auto, name) for name in ["sx_auto_collect_deinit", "sx_auto_collect_init"])))
    run_suite("serial configuration", CONFIG_MOCK + "\n".join(function(serial, name) for name in [
        "serial_config_key", "default_server_port", "sx_serial_protocol_name",
        "sx_serial_protocol_from_name", "load_port_value", "load_serial_server_ports",
        "get_config_locked", "sx_serial_server_get_config", "save_config_locked",
        "sx_serial_server_save_config", "save_ports_locked", "sx_serial_server_save_ports"]) + CONFIG_TEST)
    w5500 = (ROOT / "main/network/w5500_manager.c").read_text(encoding="utf-8")
    run_suite("W5500 init", W5500_INIT_MOCK + function(w5500, "w5500_manager_init_with_role") + W5500_INIT_TEST)


if __name__ == "__main__":
    main()
