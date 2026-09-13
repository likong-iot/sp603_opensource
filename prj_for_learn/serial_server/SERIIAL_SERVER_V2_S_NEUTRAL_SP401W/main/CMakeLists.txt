idf_component_register(
    SRCS "sx_main.c"
         "sx_http_client.c"
         "sx_http_ota.c"
         "sx_init_eth.c"
         "sx_init_wifi.c"
         "sx_mqtt_client.c"
         "sx_tcp_client.c"
         "sx_tcp_server.c"
         "sx_web_server.c"
         "sx_utils.c"
         "sx_dns_server.c"
         "sx_wifi_scan.c"

         "sx_udp_server.c"
     #     "sx_udp_multicast.c"  # 使用UDP组播源文件

         "sx_gpio.c"
         "sx_task.c"
        #  "sx_time.c"
         "sx_ap_sta.c"
         "sx_async_uart.c"
         "sx_modbus.c"
         "sx_log.c"
         "sx_timer_tasks.c"
         "sx_platform.c"

    INCLUDE_DIRS "include"
    INCLUDE_DIRS "."
    EMBED_FILES "static/web.html"
                "static/root.html"
                "static/web.js"
                "static/web.css"
                "static/brand-config.js"
                "static/i18n.js"
                "static/i18n/zh-CN.json"
                "static/i18n/en-US.json"
                "server_certs/ca_cert.pem"
)

# 变体配置
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DEVICE_MODEL="SP401W"
    BRAND_TYPE="NEUTRAL"
    BRAND_NAME_CN=""
    BRAND_NAME_EN=""
)
