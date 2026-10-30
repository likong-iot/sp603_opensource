#!/usr/bin/env python3
"""Exercise production W5500 configuration/events with ESP-NETIF state stubs.

Requires Python 3 and cc. This checks role/state sequencing, not DHCP packets
or hardware. The stubs model ESP-IDF 5.5's interface flags and DHCP states.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

TEST = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

typedef int esp_err_t;
typedef const char *esp_event_base_t;
typedef void *esp_eth_handle_t;
typedef unsigned char dhcps_offer_t;
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct {
    struct { int type; union { ip4_addr_t ip4; } u_addr; } ip;
} esp_netif_dns_info_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, TEST_FAILURE,
       ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED,
       ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED };
enum { ESP_NETIF_DHCP_CLIENT = 1, ESP_NETIF_DHCP_SERVER = 2,
       ESP_NETIF_FLAG_GARP = 4, ESP_IPADDR_TYPE_V4 = 0,
       ESP_NETIF_DNS_MAIN = 0, ESP_NETIF_OP_SET = 1,
       ESP_NETIF_DOMAIN_NAME_SERVER = 6, OFFER_DNS = 2 };
enum { ETHERNET_EVENT_CONNECTED, ETHERNET_EVENT_DISCONNECTED,
       ETHERNET_EVENT_START, ETHERNET_EVENT_STOP, ETH_CMD_G_MAC_ADDR };
enum { INIT, STOPPED, STARTED };
typedef struct { unsigned flags; } esp_netif_inherent_config_t;
typedef struct { const esp_netif_inherent_config_t *base; } esp_netif_config_t;
typedef struct {
    unsigned flags;
    bool attached, up;
    int client, server;
    esp_netif_ip_info_t info;
    esp_netif_dns_info_t dns;
    dhcps_offer_t offer_dns;
} esp_netif_t;
typedef struct { esp_netif_t *esp_netif; } ip_event_got_ip_t;
#define ESP_NETIF_INHERENT_DEFAULT_ETH() { .flags = ESP_NETIF_DHCP_CLIENT | ESP_NETIF_FLAG_GARP }
#define ESP_NETIF_DEFAULT_ETH() { .base = NULL }
#define ESP_RETURN_ON_FALSE(cond, err, ...) do { if (!(cond)) return (err); } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int rc_ = (expr); if (rc_ != ESP_OK) return rc_; } while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

static bool s_w5500_link_up, s_w5500_dns_valid, s_static_address, s_dhcp_server_enabled;
static esp_eth_handle_t s_eth_handle = (void *)2;
static void *s_eth_glue = (void *)1;
static esp_netif_t *s_eth_netif;
static esp_netif_t netif;
static int client_starts, client_stops, server_starts, server_stops;
static int link_ups, link_downs, got_ips, lost_ips;
static bool new_fails;
static int attach_error, dns_error, option_error, set_ip_error;

static int ip4addr_aton(const char *s, ip4_addr_t *out)
{
    return s && inet_pton(AF_INET, s, &out->addr) == 1;
}
static uint32_t ip(const char *s)
{
    ip4_addr_t out;
    assert(ip4addr_aton(s, &out));
    return out.addr;
}
static esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg)
{
    if (new_fails) return NULL;
    unsigned flags = cfg->base->flags;
    /* ESP-IDF rejects simultaneous client/server and only allocates
       server state when the server flag is present at creation. */
    if ((flags & 3) == 3) return NULL;
    netif = (esp_netif_t){ .flags = flags, .client = INIT, .server = INIT };
    return &netif;
}
static esp_err_t esp_netif_attach(esp_netif_t *n, void *glue)
{
    assert(n && glue);
    if (attach_error) return attach_error;
    n->attached = true;
    return ESP_OK;
}
static esp_err_t esp_netif_dhcpc_start(esp_netif_t *n)
{
    assert(n->attached && (n->flags & ESP_NETIF_DHCP_CLIENT));
    client_starts++;
    if (n->client == STARTED) return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
    n->client = n->up ? STARTED : INIT;
    return ESP_OK;
}
static esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n)
{
    assert(n->flags & ESP_NETIF_DHCP_CLIENT);
    client_stops++;
    if (n->client == STOPPED) return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    n->client = STOPPED;
    return ESP_OK;
}
static esp_err_t esp_netif_dhcps_start(esp_netif_t *n)
{
    assert(n->attached && (n->flags & ESP_NETIF_DHCP_SERVER));
    server_starts++;
    if (n->server == STARTED) return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
    n->server = n->up ? STARTED : INIT;
    return ESP_OK;
}
static esp_err_t esp_netif_dhcps_stop(esp_netif_t *n)
{
    assert(n->flags & ESP_NETIF_DHCP_SERVER);
    server_stops++;
    if (n->server == STOPPED) return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    n->server = STOPPED;
    return ESP_OK;
}
static esp_err_t esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *info)
{
    assert(!(n->flags & ESP_NETIF_DHCP_CLIENT) || n->client == STOPPED);
    assert(!(n->flags & ESP_NETIF_DHCP_SERVER) || n->server == STOPPED);
    if (set_ip_error) return set_ip_error;
    n->info = *info;
    return ESP_OK;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *info)
{
    *info = n->info;
    return ESP_OK;
}
static esp_err_t esp_netif_set_dns_info(esp_netif_t *n, int type, esp_netif_dns_info_t *dns)
{
    assert(type == ESP_NETIF_DNS_MAIN);
    if (dns_error) return dns_error;
    n->dns = *dns;
    return ESP_OK;
}
static esp_err_t esp_netif_dhcps_option(esp_netif_t *n, int op, int id, void *value, uint32_t size)
{
    assert((n->flags & ESP_NETIF_DHCP_SERVER) && n->server == STOPPED);
    assert(op == ESP_NETIF_OP_SET && id == ESP_NETIF_DOMAIN_NAME_SERVER);
    assert(size == sizeof(dhcps_offer_t));
    if (option_error) return option_error;
    n->offer_dns = *(dhcps_offer_t *)value;
    return ESP_OK;
}
static void esp_eth_ioctl(esp_eth_handle_t h, int cmd, void *out)
{
    (void)h;
    assert(cmd == ETH_CMD_G_MAC_ADDR);
    memset(out, 0, 6);
}
static void sx_network_manager_w5500_link_up(void) { link_ups++; }
static void sx_network_manager_w5500_link_down(void) { link_downs++; }
static void sx_network_manager_w5500_lost_ip(void) { lost_ips++; }
static void sx_network_manager_w5500_got_ip(const esp_netif_ip_info_t *info)
{
    assert(info->ip.addr);
    got_ips++;
}

PRODUCTION_HELPERS

static void reset(void)
{
    s_eth_netif = NULL;
    s_w5500_link_up = s_w5500_dns_valid = false;
    s_static_address = s_dhcp_server_enabled = false;
    client_starts = client_stops = server_starts = server_stops = 0;
    link_ups = link_downs = got_ips = 0;
    new_fails = false;
    attach_error = dns_error = option_error = set_ip_error = 0;
    memset(&netif, 0, sizeof(netif));
}
static void event(int id)
{
    esp_eth_handle_t handle = (void *)2;
    /* SDK glue receives the event before the application handler. */
    if (id == ETHERNET_EVENT_CONNECTED) netif.up = true;
    if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) netif.up = false;
    w5500_eth_event_handler(NULL, NULL, id, &handle);
}
static esp_err_t configure(bool lan, bool use_static, bool server)
{
    return w5500_configure_netif(lan, "192.168.10.1", "255.255.255.0",
                                use_static, "192.168.10.254", "8.8.8.8", server);
}
int main(void)
{
    reset();
    assert(configure(true, false, true) == ESP_OK);
    assert(netif.flags & ESP_NETIF_DHCP_SERVER);
    assert(!(netif.flags & ESP_NETIF_DHCP_CLIENT));
    assert(netif.flags & ESP_NETIF_FLAG_GARP);
    assert(netif.info.ip.addr == ip("192.168.10.1"));
    assert(netif.info.netmask.addr == ip("255.255.255.0"));
    assert(netif.info.gw.addr == netif.info.ip.addr);
    assert(netif.dns.ip.u_addr.ip4.addr == ip("8.8.8.8"));
    assert(netif.offer_dns == OFFER_DNS);
    assert(netif.server == STOPPED && server_starts == 0);
    esp_eth_handle_t unrelated = (void *)99;
    w5500_eth_event_handler(NULL, NULL, ETHERNET_EVENT_CONNECTED, &unrelated);
    w5500_eth_event_handler(NULL, NULL, ETHERNET_EVENT_CONNECTED, NULL);
    assert(link_ups == 0 && server_starts == 0);
    event(ETHERNET_EVENT_START);
    assert(server_starts == 0);
    event(ETHERNET_EVENT_CONNECTED);
    assert(netif.server == STARTED && s_w5500_link_up);
    assert(link_ups == 1 && got_ips == 1);
    event(ETHERNET_EVENT_CONNECTED);  /* already started is harmless */
    assert(netif.server == STARTED);
    event(ETHERNET_EVENT_DISCONNECTED);
    assert(netif.server == STOPPED && !s_w5500_link_up && link_downs == 1);
    event(ETHERNET_EVENT_DISCONNECTED); /* already stopped is harmless */
    event(ETHERNET_EVENT_CONNECTED);
    assert(netif.server == STARTED && got_ips == 3);
    event(ETHERNET_EVENT_STOP);
    assert(netif.server == STOPPED);
    assert(client_starts == 0 && client_stops == 0);
    puts("PASS downlink server address, router/DNS offers, and link lifecycle");

    reset();
    assert(configure(true, true, false) == ESP_OK);
    assert(!(netif.flags & (ESP_NETIF_DHCP_CLIENT | ESP_NETIF_DHCP_SERVER)));
    assert(netif.info.gw.addr == netif.info.ip.addr);
    event(ETHERNET_EVENT_CONNECTED);
    event(ETHERNET_EVENT_DISCONNECTED);
    assert(client_starts == 0 && client_stops == 0 && server_starts == 0 && server_stops == 0);
    puts("PASS downlink without DHCP keeps a static address and no DHCP role");

    reset();
    assert(configure(false, true, false) == ESP_OK);
    assert(netif.flags & ESP_NETIF_DHCP_CLIENT);
    assert(!(netif.flags & ESP_NETIF_DHCP_SERVER));
    assert(netif.client == STOPPED && client_stops == 1);
    assert(netif.info.gw.addr == ip("192.168.10.254"));
    assert(netif.dns.ip.u_addr.ip4.addr == ip("8.8.8.8"));
    event(ETHERNET_EVENT_CONNECTED);
    event(ETHERNET_EVENT_DISCONNECTED);
    assert(server_starts == 0 && server_stops == 0 && got_ips == 1);
    puts("PASS static uplink retains its configured gateway and DNS");

    reset();
    assert(w5500_configure_netif(false, NULL, NULL, false, NULL, NULL, false) == ESP_OK);
    assert(netif.flags & ESP_NETIF_DHCP_CLIENT);
    assert(!s_static_address && client_starts == 1 && client_stops == 0);
    event(ETHERNET_EVENT_CONNECTED);
    assert(got_ips == 0 && server_starts == 0 && server_stops == 0);
    puts("PASS DHCP uplink uses only the client and waits for an IP event");

    s_w5500_dns_valid = true;
    ip_event_got_ip_t lost = {.esp_netif = NULL};
    w5500_on_lost_ip(NULL, NULL, 0, NULL);
    w5500_on_lost_ip(NULL, NULL, 0, &lost);
    assert(lost_ips == 0 && s_w5500_dns_valid);
    lost.esp_netif = s_eth_netif;
    w5500_on_lost_ip(NULL, NULL, 0, &lost);
    assert(lost_ips == 1 && !s_w5500_dns_valid);
    s_static_address = true;
    w5500_on_lost_ip(NULL, NULL, 0, &lost);
    assert(lost_ips == 1);
    puts("PASS lost-IP ignores null/unrelated netifs and static addresses");

    reset(); new_fails = true;
    assert(configure(true, false, true) == ESP_ERR_NO_MEM);
    reset(); attach_error = TEST_FAILURE;
    assert(configure(true, false, true) == TEST_FAILURE);
    reset(); set_ip_error = TEST_FAILURE;
    assert(configure(true, false, true) == TEST_FAILURE);
    reset(); dns_error = TEST_FAILURE;
    assert(configure(true, false, true) == TEST_FAILURE);
    reset(); option_error = TEST_FAILURE;
    assert(configure(true, false, true) == TEST_FAILURE);
    reset();
    assert(w5500_configure_netif(true, "invalid", "255.255.255.0",
                                false, NULL, NULL, true) == ESP_ERR_INVALID_ARG);
    assert(server_starts == 0);
    puts("PASS allocation/attach/address/DNS errors propagate before server start");
    return 0;
}
"""


def main():
    source = (ROOT / "main/network/w5500_manager.c").read_text(encoding="utf-8")
    lifecycle = source[
        source.index("static void w5500_set_dhcp_server_running("):
        source.index("static void w5500_on_got_ip(")
    ]
    config = source[
        source.index("static esp_err_t w5500_configure_netif("):
        source.index("esp_err_t w5500_manager_init_with_role(")
    ]
    lost_ip = source[source.index("static void w5500_on_lost_ip("):
                     source.index("static esp_err_t w5500_apply_mac(")]
    with tempfile.TemporaryDirectory(prefix="w5500-dhcp-test-") as directory:
        cfile = Path(directory) / "test.c"
        binary = Path(directory) / "test"
        cfile.write_text(TEST.replace("PRODUCTION_HELPERS", lifecycle + lost_ip + config), encoding="utf-8")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
