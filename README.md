# firmware-warehouse-monitor

Firmware para **ESP32** (ESP-IDF) que monitora temperatura e umidade de um ambiente com sensor **DHT22/AM2301**, publicando as leituras via **MQTT** para o [backend-warehouse-monitor](https://github.com/NKRaff/backend-warehouse-monitor). Inclui provisionamento de Wi-Fi via portal cativo (Access Point + página web), fallback automático para modo AP e reset de configuração via botão físico.

## Sumário

- [Funcionalidades](#funcionalidades)
- [Hardware](#hardware)
- [Pinagem (GPIOs)](#pinagem-gpios)
- [Pré-requisitos](#pré-requisitos)
- [Configuração](#configuração)
- [Build, flash e monitor](#build-flash-e-monitor)
- [Dev Container](#dev-container)
- [Como funciona](#como-funciona)
- [Estrutura de pastas](#estrutura-de-pastas)
- [Segurança](#segurança)

## Funcionalidades

- Leitura periódica (a cada 3s) de temperatura e umidade via sensor DHT22
- Publicação das leituras via MQTT (TLS) somente quando o valor muda, em tópicos únicos por dispositivo (identificados pelo endereço MAC)
- Provisionamento de Wi-Fi por portal cativo: o ESP32 sobe um Access Point com uma página HTML para o usuário informar SSID/senha da rede local
- Persistência das credenciais Wi-Fi em NVS (memória não volátil)
- Fallback automático para modo Access Point caso a conexão à rede STA configurada falhe (timeout de 10s)
- Reset das configurações de Wi-Fi segurando o botão físico de reset
- Indicação visual via LEDs: modo de configuração, leitura de temperatura, leitura de umidade e erro de sensor

## Hardware

- Placa ESP32
- Sensor de temperatura/umidade DHT22 (AM2301)
- 4 LEDs (configuração, temperatura, umidade, erro)
- 1 botão (reset de configuração)

## Pinagem (GPIOs)

| Função                  | GPIO |
|--------------------------|------|
| Sensor DHT22              | 33   |
| LED de configuração        | 14   |
| LED de temperatura         | 27   |
| LED de umidade              | 26   |
| LED de erro                 | 25   |
| Botão de reset               | 32   |

## Pré-requisitos

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html) instalado (ou uso do Dev Container fornecido)
- Placa ESP32 conectada via USB
- Sensor DHT22 conectado ao GPIO configurado
- Um broker MQTT acessível (o projeto foi testado com [HiveMQ Cloud](https://www.hivemq.com/mqtt-cloud-broker/))

## Configuração

As credenciais do broker MQTT e o SSID/senha do Access Point de configuração estão definidos como macros no topo de `main/main.c`:

```c
#define WIFI_AP_SSID  "ESP32-AP"
#define WIFI_AP_PASS  "12345678"

#define CONFIG_BROKER_URL     "mqtts://<seu-broker>:8883"
#define CONFIG_MQTT_USERNAME  "<usuario>"
#define CONFIG_MQTT_PASSWORD  "<senha>"
```

Ajuste esses valores para o seu broker MQTT antes de compilar. As credenciais da rede Wi-Fi local (modo estação) **não** ficam no código-fonte — são fornecidas em tempo de execução pelo portal cativo e salvas em NVS.

> ⚠️ Veja a seção [Segurança](#segurança) sobre os valores hardcoded no firmware.

## Build, flash e monitor

Com o ESP-IDF configurado no ambiente (`. $IDF_PATH/export.sh`):

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORTA_SERIAL> flash monitor
```

Para sair do monitor serial: `Ctrl+]`.

## Dev Container

O projeto inclui um Dev Container (`.devcontainer/`) baseado na imagem oficial `espressif/idf`, pronto para uso no VS Code com a extensão ESP-IDF. Basta abrir a pasta do projeto no VS Code e escolher "Reopen in Container".

## Como funciona

1. **Boot**: o firmware inicializa NVS, GPIOs (LEDs/botão) e tenta ler credenciais Wi-Fi salvas.
2. **Sem credenciais salvas (ou falha de conexão)**: sobe um Access Point (`ESP32-AP`) e um servidor HTTP com uma página de configuração. O usuário se conecta ao AP, acessa `192.168.4.1` e informa o SSID/senha da rede desejada; as credenciais são salvas em NVS e o dispositivo reinicia.
3. **Com credenciais válidas**: conecta-se à rede Wi-Fi (modo STA). Ao obter IP, inicia a conexão MQTT (TLS) com o broker configurado.
4. **Loop principal**: a cada 3 segundos, lê o sensor DHT22; se o valor de temperatura ou umidade mudou desde a última leitura, publica no tópico MQTT correspondente (`<MAC>/temperatura` e `<MAC>/umidade`) e pisca o LED indicativo. Falhas de leitura acionam o LED de erro.
5. **Reset**: mantendo o botão físico pressionado, o firmware apaga as credenciais Wi-Fi da NVS e reinicia, voltando ao modo de configuração (Access Point).

## Estrutura de pastas

```
firmware-warehouse-monitor/
├── CMakeLists.txt              # Configuração raiz do projeto (build system ESP-IDF)
├── main/
│   ├── CMakeLists.txt           # Componentes/dependências do módulo principal
│   ├── idf_component.yml        # Dependências gerenciadas (esp-idf-lib/dht)
│   └── main.c                   # Lógica completa do firmware
├── managed_components/          # Componentes baixados automaticamente pelo IDF Component Manager
└── .devcontainer/                # Ambiente de desenvolvimento containerizado (ESP-IDF)
```

## Segurança

O SSID/senha do Access Point de configuração e as credenciais do broker MQTT estão definidos diretamente no código-fonte (`main/main.c`). Para uso em produção, recomenda-se:

- Mover esses valores para `menuconfig`/`Kconfig` ou para variáveis armazenadas em NVS/partição de segurança, em vez de hardcoded;
- Trocar a senha padrão do Access Point de configuração;
- Utilizar certificados/credenciais MQTT únicos por dispositivo, em vez de credenciais compartilhadas.