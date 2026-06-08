#include <Arduino.h>
#include <Wire.h>
#include <PCF8574.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ============================================================
// Configurações de rede e MQTT
// ============================================================
const char* WIFI_SSID     = "Tim fibra 5G";
const char* WIFI_PASSWORD = "";
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "esp32_semaforos_t3";

// ============================================================
// Tópicos MQTT
// ============================================================
// Publicação (ESP32 → Broker)
const char* TOPIC_STATUS_SAT   = "semaforo/saturnino/status";    // VERDE/AMARELO/VERMELHO
const char* TOPIC_STATUS_PRO   = "semaforo/protasio/status";     // VERDE/AMARELO/VERMELHO
const char* TOPIC_STATUS_PED   = "semaforo/pedestre/status";     // LIVRE/AGUARDANDO/ATRAVESSANDO
const char* TOPIC_STATUS_MODO  = "semaforo/modo";                // NORMAL/ATENCAO

// Subscrição (Broker → ESP32)
const char* TOPIC_CMD_MODO     = "semaforo/comando/modo";        // NORMAL / ATENCAO
const char* TOPIC_CMD_PEDESTRE = "semaforo/comando/pedestre";    // SOLICITAR (simula botão pelo supervisório)

// ============================================================
// Pinagem
// ============================================================
#define PIN_SDA        21
#define PIN_SCL        22
#define PIN_BTN_PED    15   // Botão físico do pedestre (pull-up interno)

// ============================================================
// PCF8574
// Chip 1 (0x38) — Semáforo da Av. Saturnino de Brito
//   P0=VM_SAT, P1=AM_SAT, P2=VD_SAT
//   P3=VM_PED, P4=VD_PED  (semáforo de pedestre)
//
// Chip 2 (0x39) — Semáforo da Av. Protásio Alves
//   P0=VM_PRO, P1=AM_PRO, P2=VD_PRO
//   P3..P7 livres
//
// ATENÇÃO: saída do PCF8574 ligada ao CÁTODO → LOW = LED ACESO
// ============================================================
PCF8574 chip1(0x38);
PCF8574 chip2(0x39);

// Pinos do chip1
#define SAT_VM  0
#define SAT_AM  1
#define SAT_VD  2
#define PED_VM  3
#define PED_VD  4

// Pinos do chip2
#define PRO_VM  0
#define PRO_AM  1
#define PRO_VD  2

// ============================================================
// Temporização (ms)
// ============================================================
#define T_VERDE_SAT    10000UL   // Verde Saturnino: 10 s
#define T_VERDE_PRO    15000UL   // Verde Protásio:  15 s (maior)
#define T_AMARELO       3000UL   // Amarelo: 3 s
#define T_PEDESTRE      8000UL   // Pedestre: 8 s de passagem
#define T_PISCA_ATENCAO  500UL   // Pisca 1 Hz = 500 ms ON + 500 ms OFF

// ============================================================
// FSM — Estados do ciclo do semáforo
// ============================================================
enum EstadoSemaforo {
    SAT_VERDE,        // Saturnino verde, Protásio vermelho
    SAT_AMARELO,      // Saturnino amarelo (transição)
    PRO_VERDE,        // Protásio verde, Saturnino vermelho
    PRO_AMARELO,      // Protásio amarelo (transição)
    PEDESTRE_FASE,    // Todos vermelhos, pedestre atravessa
    ATENCAO           // Todos amarelo piscante 1 Hz
};

volatile EstadoSemaforo estadoAtual = SAT_VERDE;
volatile EstadoSemaforo estadoAntes = SAT_VERDE;  // estado anterior ao modo ATENCAO

volatile bool modoPendente     = false;   // flag para publicar modo fora do callback
volatile bool modoAtencao      = false;
volatile bool pedestreSolicit  = false;   // botão foi pressionado

// ============================================================
// Objetos globais
// ============================================================
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

SemaphoreHandle_t xMqttMutex;
SemaphoreHandle_t xEstadoMutex;

// ============================================================
// Protótipos
// ============================================================
void connectWiFi();
void connectMQTT();
void publishStatus(const char* topicSat, const char* topicPro,
                   const char* topicPed, const char* topicModo);
void setChip1(bool vmSat, bool amSat, bool vdSat, bool vmPed, bool vdPed);
void setChip2(bool vmPro, bool amPro, bool vdPro);
void mqttCallback(char* topic, byte* payload, unsigned int length);

void taskMQTT(void* pvParameters);
void taskSemaforo(void* pvParameters);
void taskBotaoPedestre(void* pvParameters);

// ============================================================
// Setup
// ============================================================
void setup() {
    Serial.begin(115200);

    Wire.begin(PIN_SDA, PIN_SCL);
    chip1.begin();
    chip2.begin();

    // Todos os LEDs apagados no início (HIGH = LED apagado)
    for (int i = 0; i < 8; i++) {
        chip1.write(i, HIGH);
        chip2.write(i, HIGH);
    }

    pinMode(PIN_BTN_PED, INPUT_PULLUP);

    xMqttMutex   = xSemaphoreCreateMutex();
    xEstadoMutex = xSemaphoreCreateMutex();

    connectWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    connectMQTT();

    xTaskCreate(taskMQTT,          "MQTT",     4096, NULL, 3, NULL);
    xTaskCreate(taskSemaforo,      "Semaforo", 4096, NULL, 2, NULL);
    xTaskCreate(taskBotaoPedestre, "BtnPed",   2048, NULL, 2, NULL);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}

// ============================================================
// Helpers de hardware
// LOW = LED ACESO (cátodo no PCF8574)
// ============================================================
void setChip1(bool vmSat, bool amSat, bool vdSat, bool vmPed, bool vdPed) {
    chip1.write(SAT_VM, vmSat ? LOW : HIGH);
    chip1.write(SAT_AM, amSat ? LOW : HIGH);
    chip1.write(SAT_VD, vdSat ? LOW : HIGH);
    chip1.write(PED_VM, vmPed ? LOW : HIGH);
    chip1.write(PED_VD, vdPed ? LOW : HIGH);
}

void setChip2(bool vmPro, bool amPro, bool vdPro) {
    chip2.write(PRO_VM, vmPro ? LOW : HIGH);
    chip2.write(PRO_AM, amPro ? LOW : HIGH);
    chip2.write(PRO_VD, vdPro ? LOW : HIGH);
}

// ============================================================
// Conexões
// ============================================================
void connectWiFi() {
    Serial.printf("[WiFi] Conectando a %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
    while (!mqttClient.connected()) {
        Serial.print("[MQTT] Conectando ao broker...");
        if (mqttClient.connect(MQTT_CLIENT)) {
            Serial.println(" conectado!");
            mqttClient.subscribe(TOPIC_CMD_MODO);
            mqttClient.subscribe(TOPIC_CMD_PEDESTRE);
            Serial.println("[MQTT] Subscrições OK");
        } else {
            Serial.printf(" falhou (rc=%d). Tentando em 3s...\n", mqttClient.state());
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

// ============================================================
// Callback MQTT
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char msg[32];
    unsigned int len = min(length, (unsigned int)31);
    memcpy(msg, payload, len);
    msg[len] = '\0';

    String topicStr = String(topic);
    String msgStr   = String(msg);

    Serial.printf("[MQTT] [%s]: %s\n", topic, msg);

    if (topicStr == TOPIC_CMD_MODO) {
        if (msgStr == "ATENCAO") {
            modoAtencao   = true;
            modoPendente  = true;
        } else if (msgStr == "NORMAL") {
            modoAtencao  = false;
            modoPendente = true;
        }
    } else if (topicStr == TOPIC_CMD_PEDESTRE) {
        if (msgStr == "SOLICITAR") {
            pedestreSolicit = true;
        }
    }
}

// ============================================================
// Task MQTT — mantém conexão e processa mensagens
// ============================================================
void taskMQTT(void* pvParameters) {
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) connectWiFi();
        if (!mqttClient.connected())        connectMQTT();

        if (xSemaphoreTake(xMqttMutex, pdMS_TO_TICKS(100))) {
            mqttClient.loop();
            xSemaphoreGive(xMqttMutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
// Publica status de todos os semáforos
// ============================================================
void publishAll(const char* sat, const char* pro,
                const char* ped, const char* modo) {
    if (xSemaphoreTake(xMqttMutex, pdMS_TO_TICKS(300))) {
        mqttClient.publish(TOPIC_STATUS_SAT,  sat,  true);
        mqttClient.publish(TOPIC_STATUS_PRO,  pro,  true);
        mqttClient.publish(TOPIC_STATUS_PED,  ped,  true);
        mqttClient.publish(TOPIC_STATUS_MODO, modo, true);
        xSemaphoreGive(xMqttMutex);
    }
    Serial.printf("[FSM] SAT=%s PRO=%s PED=%s MODO=%s\n", sat, pro, ped, modo);
}

// ============================================================
// Task Semáforo — FSM principal
// ============================================================
void taskSemaforo(void* pvParameters) {
    bool piscaLigado = false;

    for (;;) {
        // --- Modo Atenção: amarelo piscante 1Hz ---
        if (modoAtencao) {
            if (estadoAtual != ATENCAO) {
                estadoAntes  = estadoAtual;
                estadoAtual  = ATENCAO;
                publishAll("AMARELO", "AMARELO", "VERMELHO", "ATENCAO");
            }

            // Pisca amarelo em ambos
            piscaLigado = !piscaLigado;
            setChip1(false, piscaLigado, false, true, false);  // PED vermelho fixo
            setChip2(false, piscaLigado, false);
            vTaskDelay(pdMS_TO_TICKS(T_PISCA_ATENCAO));
            continue;
        }

        // Voltou do modo atenção → retoma do estado anterior
        if (estadoAtual == ATENCAO) {
            estadoAtual = estadoAntes;
        }

        switch (estadoAtual) {

            // --------------------------------------------------
            // SAT_VERDE: Saturnino=VERDE, Protásio=VERMELHO
            // --------------------------------------------------
            case SAT_VERDE: {
                setChip1(false, false, true,  true, false);   // SAT VD, PED VM
                setChip2(true,  false, false);                 // PRO VM
                publishAll("VERDE", "VERMELHO", "LIVRE", "NORMAL");

                TickType_t inicio = xTaskGetTickCount();
                TickType_t duracao = pdMS_TO_TICKS(T_VERDE_SAT);

                while ((xTaskGetTickCount() - inicio) < duracao) {
                    if (modoAtencao) break;
                    if (pedestreSolicit) break;
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (modoAtencao) break;

                if (pedestreSolicit) {
                    estadoAtual = SAT_AMARELO;  // continua para amarelo antes de pedestre
                } else {
                    estadoAtual = SAT_AMARELO;
                }
                break;
            }

            // --------------------------------------------------
            // SAT_AMARELO: Saturnino=AMARELO (transição)
            // --------------------------------------------------
            case SAT_AMARELO: {
                setChip1(false, true, false, true, false);    // SAT AM, PED VM
                setChip2(true,  false, false);                 // PRO VM
                publishAll("AMARELO", "VERMELHO", "AGUARDANDO", "NORMAL");

                vTaskDelay(pdMS_TO_TICKS(T_AMARELO));

                if (modoAtencao) break;

                if (pedestreSolicit) {
                    estadoAtual    = PEDESTRE_FASE;
                    pedestreSolicit = false;
                } else {
                    estadoAtual = PRO_VERDE;
                }
                break;
            }

            // --------------------------------------------------
            // PRO_VERDE: Protásio=VERDE, Saturnino=VERMELHO
            // --------------------------------------------------
            case PRO_VERDE: {
                setChip1(true,  false, false, true, false);   // SAT VM, PED VM
                setChip2(false, false, true);                  // PRO VD
                publishAll("VERMELHO", "VERDE", "LIVRE", "NORMAL");

                TickType_t inicio = xTaskGetTickCount();
                TickType_t duracao = pdMS_TO_TICKS(T_VERDE_PRO);

                while ((xTaskGetTickCount() - inicio) < duracao) {
                    if (modoAtencao) break;
                    if (pedestreSolicit) break;
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                if (modoAtencao) break;

                estadoAtual = PRO_AMARELO;
                break;
            }

            // --------------------------------------------------
            // PRO_AMARELO: Protásio=AMARELO (transição)
            // --------------------------------------------------
            case PRO_AMARELO: {
                setChip1(true,  false, false, true, false);   // SAT VM, PED VM
                setChip2(false, true,  false);                 // PRO AM
                publishAll("VERMELHO", "AMARELO", "AGUARDANDO", "NORMAL");

                vTaskDelay(pdMS_TO_TICKS(T_AMARELO));

                if (modoAtencao) break;

                if (pedestreSolicit) {
                    estadoAtual    = PEDESTRE_FASE;
                    pedestreSolicit = false;
                } else {
                    estadoAtual = SAT_VERDE;
                }
                break;
            }

            // --------------------------------------------------
            // PEDESTRE_FASE: todos vermelhos, pedestre atravessa
            // --------------------------------------------------
            case PEDESTRE_FASE: {
                setChip1(true,  false, false, false, true);   // SAT VM, PED VD
                setChip2(true,  false, false);                 // PRO VM
                publishAll("VERMELHO", "VERMELHO", "ATRAVESSANDO", "NORMAL");

                vTaskDelay(pdMS_TO_TICKS(T_PEDESTRE));

                if (!modoAtencao) {
                    estadoAtual = SAT_VERDE;  // retoma ciclo normal
                }
                break;
            }

            default:
                estadoAtual = SAT_VERDE;
                break;
        }
    }
}

// ============================================================
// Task Botão Pedestre — lê GPIO físico (pull-up: LOW = pressionado)
// ============================================================
void taskBotaoPedestre(void* pvParameters) {
    bool ultimoEstado = HIGH;

    for (;;) {
        bool estadoBtn = digitalRead(PIN_BTN_PED);

        // Borda de descida (pressionado)
        if (ultimoEstado == HIGH && estadoBtn == LOW) {
            pedestreSolicit = true;
            Serial.println("[BTN] Pedestre solicitou travessia");

            // Publica no broker para o supervisório refletir
            if (xSemaphoreTake(xMqttMutex, pdMS_TO_TICKS(200))) {
                mqttClient.publish(TOPIC_STATUS_PED, "AGUARDANDO", true);
                xSemaphoreGive(xMqttMutex);
            }
            vTaskDelay(pdMS_TO_TICKS(300));  // debounce
        }

        ultimoEstado = estadoBtn;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
