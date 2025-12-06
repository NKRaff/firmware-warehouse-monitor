#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dht.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
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

#define CONFIG_BROKER_URL     "mqtts://38046a81f3ca4a18aa3b57d26f8a9887.s1.eu.hivemq.cloud:8883"
#define CONFIG_MQTT_USERNAME  "ESP32"
#define CONFIG_MQTT_PASSWORD  "Senha1234"

#define SENSOR_TYPE           DHT_TYPE_AM2301
#define SENSOR_GPIO           33
#define LED_CONFIG_GPIO       14
#define LED_TEMPERATURA_GPIO  27
#define LED_UMIDADE_GPIO      26
#define LED_ERRO_GPIO         25
#define BOTAO_RESET_GPIO      32

static char device_mac_str[18];
static char topic_umidade[64];
static char topic_temperatura[64];
static int s_retry_num = 0;
static esp_mqtt_client_handle_t global_mqtt_client = NULL;
static EventGroupHandle_t s_wifi_event_group;
TaskHandle_t ap_blink_handle = NULL;

static const char *TAG_AP   = "WiFi SoftAP";
static const char *TAG_STA  = "WiFi Sta";
static const char *TAG_HTTP = "Webserver";
static const char *TAG_MQTT = "MQTT";
static char wifi_config_html[2048];
static char wifi_config_html_template[] =
"<!DOCTYPE html>"
"<html lang='pt-br'>"
"<head>"
"  <meta charset='UTF-8'>"
"  <meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"  <title>Configurar WiFi</title>"
"  <style>"
"    body { font-family: Arial; padding: 20px; background: #f0f0f0; }"
"    .card { max-width: 400px; margin: auto; background: white; padding: 20px; border-radius: 10px; "
"            box-shadow: 0 2px 6px rgba(0,0,0,0.2); }"
"    input { width: 100%; box-sizing: border-box; padding: 12px; margin: 8px 0; border-radius: 5px; border: 1px solid #ccc; }"
"    button { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 5px;"
"             font-size: 16px; cursor: pointer; }"
"    button:hover { background: #0056b3; }"
"    .mac-box { background:#e9ecef; padding:10px; border-radius:5px; margin-bottom:15px; font-size:14px; }"
"  </style>"
"</head>"
"<body>"
"  <div class='card'>"
"    <h2>Configurar WiFi</h2>"
"    <div class='mac-box'>Endereço MAC do dispositivo:<br><b>%s</b></div>"
"    <form action='/wifi' method='POST'>"
"      <label>SSID</label>"
"      <input type='text' name='ssid' placeholder='Nome da Rede' required>"
"      <label>Senha</label>"
"      <input type='password' name='password' placeholder='Senha' required>"
"      <button type='submit'>Salvar e Conectar</button>"
"    </form>"
"  </div>"
"</body>"
"</html>";

// -----------------------------------------------------------------------------------------------------------
// HARDWARE
// -----------------------------------------------------------------------------------------------------------

void config_led(void) 
{
  gpio_reset_pin(LED_CONFIG_GPIO);
  gpio_reset_pin(LED_TEMPERATURA_GPIO);
  gpio_reset_pin(LED_UMIDADE_GPIO);
  gpio_reset_pin(LED_ERRO_GPIO);
  gpio_set_direction(LED_CONFIG_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_TEMPERATURA_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_UMIDADE_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_direction(LED_ERRO_GPIO, GPIO_MODE_OUTPUT);
}

void config_button(void)
{
  gpio_reset_pin(BOTAO_RESET_GPIO);
  gpio_set_direction(BOTAO_RESET_GPIO, GPIO_MODE_INPUT);
  gpio_set_pull_mode(BOTAO_RESET_GPIO, GPIO_PULLUP_ONLY);
}

void config_sensor(void)
{
  gpio_reset_pin(SENSOR_GPIO);
  gpio_set_direction(SENSOR_GPIO, GPIO_MODE_INPUT);
}

void blink_led(int LED_GPIO)
{
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(LED_GPIO, 0);
}

esp_err_t get_esp_mac_address(char *mac_addr_str)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);

    if(err == ESP_OK) {
        sprintf(mac_addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    return err;
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
// WEBSERVER - HANDLER
// -----------------------------------------------------------------------------------------------------------

void prepare_wifi_page(const char *mac)
{
  snprintf(wifi_config_html, sizeof(wifi_config_html), wifi_config_html_template, mac);
}

esp_err_t wifi_get_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, wifi_config_html, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t wifi_post_handler(httpd_req_t *req)
{
  char buf[256];
  int ret, remaining = req->content_len;

  char ssid[32] = {0};
  char pass[64] = {0};

  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf, (remaining < sizeof(buf) ? remaining : sizeof(buf)))) <= 0) {
      return ESP_FAIL;
    }
    remaining -= ret;
    buf[ret] = '\0';

    // Aceita JSON simples ou form-urlencoded
    sscanf(buf, "{\"ssid\":\"%31[^\"]\",\"password\":\"%63[^\"]\"}", ssid, pass);

    if (strlen(ssid) == 0) {
      // tenta form-urlencoded
      sscanf(buf, "ssid=%31[^&]&password=%63s", ssid, pass);
    }
  }

  if (strlen(ssid) == 0 || strlen(pass) == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "Bad request", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  ESP_LOGI(TAG_HTTP, "Recebido via POST -> SSID: %s | PASS: %s", ssid, pass);

  // Salva no NVS
  nvs_handle handle;
  if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_str(handle, "ssid", ssid);
    nvs_set_str(handle, "password", pass);
    nvs_commit(handle);
    nvs_close(handle);
  }

  httpd_resp_sendstr(req, "OK, WiFi salvo. Reiniciando...");

  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
  return ESP_OK;
}

// -----------------------------------------------------------------------------------------------------------
// INICIA SERVIDOR WEB
// -----------------------------------------------------------------------------------------------------------

httpd_handle_t start_webserver(void)
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = NULL;

  ESP_LOGI(TAG_HTTP, "Iniciando Webserver");

  if (httpd_start(&server, &config) == ESP_OK) {

    httpd_uri_t wifi_get = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = wifi_get_handler
      };
    httpd_register_uri_handler(server, &wifi_get);
    
    httpd_uri_t wifi_post = {
      .uri = "/wifi",
      .method = HTTP_POST,
      .handler = wifi_post_handler
    };
    httpd_register_uri_handler(server, &wifi_post);

    return server;
  }

  return NULL;
}


// -----------------------------------------------------------------------------------------------------------
// CONFIGURAÇÕES NO NVS
// -----------------------------------------------------------------------------------------------------------

esp_err_t wifi_read_sta_config(char *ssid, char *password) 
{
  esp_err_t err;
  nvs_handle my_handle;

  err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) return err;

  // SSID
  size_t ssid_size = 0;
  err = nvs_get_str(my_handle, "ssid", NULL, &ssid_size);
  if (err == ESP_OK && ssid_size <= 32) {
    nvs_get_str(my_handle, "ssid", ssid, &ssid_size);
  }

  // Password
  size_t pass_size = 0;
  err = nvs_get_str(my_handle, "password", NULL, &pass_size);
  if (err == ESP_OK && pass_size <= 64) {
    nvs_get_str(my_handle, "password", password, &pass_size);
  }

  nvs_close(my_handle);
  return ESP_OK;
}

// -----------------------------------------------------------------------------------------------------------
// TASKS
// -----------------------------------------------------------------------------------------------------------

void wifi_reset_task(void *arg)
{
  time_t hold_time_ms = 3;
  time_t hold_start_button = 0;

  while(1) {
    while(!gpio_get_level(BOTAO_RESET_GPIO)) {
      if(hold_start_button == 0)
        hold_start_button = time(NULL);
      if(difftime(time(NULL), hold_start_button) >= hold_time_ms) {
        nvs_flash_erase();
        nvs_flash_init();
        esp_restart();
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void ap_blink_task(void *arg)
{
  while (1) {
    blink_led(LED_CONFIG_GPIO);
    vTaskDelay(pdMS_TO_TICKS(300));
  }
}

void dht_task(void *pvParameters)
{
  float temperatura;
  float umidade;

  float last_temperatura = -1;
  float last_umidade = -1;

  while(1) {
    if (dht_read_float_data(SENSOR_TYPE, SENSOR_GPIO, &umidade, &temperatura) == ESP_OK) {
      if(umidade != last_umidade && global_mqtt_client != NULL) {
        last_umidade = umidade;
        char msg[16];
        sprintf(msg, "%.1f", umidade);
        esp_mqtt_client_publish(global_mqtt_client, topic_umidade, msg, 0, 1, 0);
        blink_led(LED_UMIDADE_GPIO);
      }
      if(temperatura != last_temperatura && global_mqtt_client != NULL) {
        last_temperatura = temperatura;
        char msg[16];
        sprintf(msg, "%.1f", temperatura);
        esp_mqtt_client_publish(global_mqtt_client, topic_temperatura, msg, 0, 1, 0);
        blink_led(LED_TEMPERATURA_GPIO);
      }
      ESP_LOGI(TAG_MQTT, "Umidade: %.1f%%, Temperatura: %.1fºC", umidade, temperatura);
    } else {
      ESP_LOGE(TAG_MQTT, "Falha ao ler os dados do DHT22");
      blink_led(LED_ERRO_GPIO);
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

void sta_monitor_task(void *arg)
{
  EventBits_t bits = xEventGroupWaitBits(
    s_wifi_event_group,
    WIFI_CONNECTED_BIT,
    pdFALSE,
    pdFALSE,
    pdMS_TO_TICKS(10000)  // timeout 10 segundos
  );

  if (!(bits & WIFI_CONNECTED_BIT)) {
    ESP_LOGW(TAG_STA, "Timeout de conexão WiFi. Ativando fallback para AP...");
    
    // Desliga STA
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);

    // Inicia AP
    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_init_softap();
    esp_wifi_start();
    prepare_wifi_page(device_mac_str);
    start_webserver();
    xTaskCreate(ap_blink_task, "ap_blink_task", 2048, NULL, 5, NULL);
  }

  vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------------------------------------
// MQTT EVENTS HANDLER
// -----------------------------------------------------------------------------------------------------------

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG_MQTT, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  ESP_LOGD(TAG_MQTT, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG_MQTT, "MQTT_EVENT_CONNECTED");
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG_MQTT, "MQTT_EVENT_DISCONNECTED");
    break;

  case MQTT_EVENT_SUBSCRIBED:
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    break;
  case MQTT_EVENT_PUBLISHED:
    break;
  case MQTT_EVENT_DATA:
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG_MQTT, "MQTT_EVENT_ERROR");
    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
      log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
      log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
      log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
      ESP_LOGI(TAG_MQTT, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
    }
    break;
  default:
    ESP_LOGI(TAG_MQTT, "Other event id:%d", event->event_id);
    break;
  }
}

// -----------------------------------------------------------------------------------------------------------
// INICIALIZAÇÃO DO MQTT
// -----------------------------------------------------------------------------------------------------------

static void mqtt_app_start(void)
{
  esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri = CONFIG_BROKER_URL,
    .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    .credentials.username = CONFIG_MQTT_USERNAME,
    .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
  };
  global_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(global_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
  esp_mqtt_client_start(global_mqtt_client);
}

// -----------------------------------------------------------------------------------------------------------
// WIFI_EVENTS HANDLER
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
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG_STA, "Falha na conexão. Tentando novamente...");
    esp_wifi_connect();
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG_STA, "IP obtido:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;  
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    gpio_set_level(LED_CONFIG_GPIO, 0);
    mqtt_app_start();
  }
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

  // Configura hardware
  config_button();
  config_led();

  // Inicializa Wifi event group
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

  char ssid[32] = {0};
  char password[64] = {0};

  wifi_read_sta_config(ssid, password);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    
  if(get_esp_mac_address(device_mac_str) == ESP_OK) {
    ESP_LOGI(TAG_MQTT, "MAC Address obtido: %s", device_mac_str);
  } else {
    strcpy(device_mac_str, "UNKNOWN_DEVICE_ID");
    ESP_LOGE(TAG_MQTT, "Falha ao obter o MAC Address. Usando ID padrão");
  }

  sprintf(topic_umidade, "%s/umidade", device_mac_str);
  sprintf(topic_temperatura, "%s/temperatura", device_mac_str);

  if (strlen(ssid) > 0 && strlen(password) > 0) {
    ESP_LOGI(TAG_STA, "Iniciando STA com dados do NVS...");
    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_init_sta(ssid, password);
    esp_wifi_start();
    xTaskCreate(wifi_reset_task, "wifi_reset_task", 2048, NULL, 5, NULL);
    xTaskCreate(sta_monitor_task, "sta_monitor_task", 4096, NULL, 5, NULL);
    xTaskCreate(dht_task, "dht_task", 4096, NULL, 5, NULL);
  } else {
    ESP_LOGI(TAG_AP, "Iniciando Access Point...");
    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_init_softap();
    esp_wifi_start();
    prepare_wifi_page(device_mac_str);
    start_webserver();
    xTaskCreate(ap_blink_task, "ap_blink_task", 2048, NULL, 5, NULL);
  }
}
