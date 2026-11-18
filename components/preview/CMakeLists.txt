idf_component_register(SRCS "preview.cpp"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "index.html"
                       REQUIRES inference people
                       PRIV_REQUIRES camera esp_wifi esp_http_server nvs_flash esp_netif)
