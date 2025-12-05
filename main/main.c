#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_STA_SSID   ""
#define WIFI_STA_PASS   ""
#define WIFI_MAX_RETRY  5

#define WIFI_AP_SSID  "ESP32-AP"
#define WIFI_AP_PASS  "12345678"
#define WIFI_CHANNEL  1
#define MAX_STA_CONN  4

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG_AP   = "WiFi SoftAP";
static const char *TAG_STA  = "WiFi Sta";

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

// -----------------------------------------------------------------------------------------------------------
// CONFIGURAÇÕES NO NVS
// -----------------------------------------------------------------------------------------------------------

esp_err_t wifi_read_sta_config(char *ssid, char *password) 
{
  esp_err_t err;
  nvs_handle my_handle;

  err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_STA, "Falha ao abrir NVS: %s", esp_err_to_name(err));
    return err;
  }

  // Ler SSID
  size_t ssid_size = 0;
  err = nvs_get_str(my_handle, "ssid", NULL, &ssid_size);

  if (err == ESP_OK) {
    if (ssid_size > 0 && ssid_size <= 32) {
      err = nvs_get_str(my_handle, "ssid", ssid, &ssid_size);
      if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
      }
    } else {
      ESP_LOGE(TAG_STA, "SSID no NVS tem tamanho inválido");
      nvs_close(my_handle);
      return ESP_ERR_NVS_INVALID_LENGTH;
    }
  } else {
    ESP_LOGI(TAG_STA, "Nenhuma SSID salva no NVS.");
  }

  // Ler Password
  size_t pass_size = 0;
  err = nvs_get_str(my_handle, "password", NULL, &pass_size);

  if (err == ESP_OK) {
    if (pass_size > 0 && pass_size <= 64) {
      err = nvs_get_str(my_handle, "password", password, &pass_size);
      if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
      }
    } else {
      ESP_LOGE(TAG_STA, "Senha no NVS tem tamanho inválido");
      nvs_close(my_handle);
      return ESP_ERR_NVS_INVALID_LENGTH;
    }
  } else {
    ESP_LOGI(TAG_STA, "Nenhuma senha salva no NVS.");
  }

  nvs_close(my_handle);
  return ESP_OK;
}

// -----------------------------------------------------------------------------------------------------------
// EVENTS HANDLER
// -----------------------------------------------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
    ESP_LOGI(TAG_AP, "Dispositivo "MACSTR" conectou, AID=%d", 
            MAC2STR(event->mac), event->aid);
  } 
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
    ESP_LOGI(TAG_AP, "Dispositivo "MACSTR" desconectou, AID=%d, reason:%d", 
            MAC2STR(event->mac), event->aid, event->reason);
  } 
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    ESP_LOGI(TAG_STA, "Modo STA iniciado");
  } 
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG_STA, "IP obtido:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

// -----------------------------------------------------------------------------------------------------------
// INICIALIZA AP / STA
// -----------------------------------------------------------------------------------------------------------

esp_netif_t *wifi_init_softap(void)
{
  esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();
  wifi_config_t wifi_ap_config = {
    .ap = {
      .ssid = WIFI_AP_SSID,
      .password = WIFI_AP_PASS,
      .channel = WIFI_CHANNEL,
      .max_connection = MAX_STA_CONN,
      .authmode = WIFI_AUTH_WPA2_PSK,
    },
  };

  if (strlen(WIFI_AP_PASS) < 8) {
    wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
  ESP_LOGI(TAG_AP, "Inicialização do Access Point concluída. SSID:%s password:%s channel:%d",
          WIFI_AP_SSID, WIFI_AP_PASS, WIFI_CHANNEL);
  return esp_netif_ap;
}

esp_netif_t *wifi_init_sta(const char *ssid, const char *password)
{
  esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

  wifi_config_t wifi_sta_config = {
    .sta = {
      .ssid = {0},
      .password = {0},
      .scan_method = WIFI_ALL_CHANNEL_SCAN,
      .failure_retry_cnt = WIFI_MAX_RETRY
    },
  };

  strncpy((char *)wifi_sta_config.sta.ssid, ssid, sizeof(wifi_sta_config.sta.ssid) - 1);
  strncpy((char *)wifi_sta_config.sta.password, password, sizeof(wifi_sta_config.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
  ESP_LOGI(TAG_STA, "Inicialização do modo STA concluída.");
  return esp_netif_sta;
}

// -----------------------------------------------------------------------------------------------------------
// APP MAIN
// -----------------------------------------------------------------------------------------------------------

void app_main(void)
{
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Inicializa NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  /*
  // TESTE INSERÇÃO NVS
  nvs_handle_t handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &handle));
  
  char ssidNvs[32] = "ssid";
  char passNvs[64] = "senha";

  ESP_ERROR_CHECK(nvs_set_str(handle, "ssid", ssidNvs));
  ESP_ERROR_CHECK(nvs_set_str(handle, "password", passNvs));
  ESP_ERROR_CHECK(nvs_commit(handle));
  nvs_close(handle);
  */
 
  // Inicializa event group
  s_wifi_event_group = xEventGroupCreate();

  // Registra Event handler
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                  ESP_EVENT_ANY_ID,
                  &wifi_event_handler,
                  NULL,
                  NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                  IP_EVENT_STA_GOT_IP,
                  &wifi_event_handler,
                  NULL,
                  NULL));

  // Variáveis para SSID e senha
  char ssid[32] = {0};
  char password[64] = {0};

  // Verifica se há configurações salvas de SSID e senha no NVS
  if (wifi_read_sta_config(ssid, password) == ESP_OK && strlen(ssid) > 0 && strlen(password) > 0) {
    ESP_LOGI(TAG_STA, "Iniciando em modo STA...");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_netif_t *esp_netif_sta = wifi_init_sta(ssid, password);
    ESP_ERROR_CHECK(esp_wifi_start());
  } else {
    ESP_LOGI(TAG_AP, "Iniciando em modo Access Point...");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    esp_netif_t *esp_netif_ap = wifi_init_softap();
    ESP_ERROR_CHECK(esp_wifi_start());
  }
}
