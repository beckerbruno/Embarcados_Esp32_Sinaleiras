#include <Arduino.h>
#include <Wire.h>
#include <PCF8574.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ============================================================
// Configurações de rede e MQTT
// ============================================================
const char *WIFI_SSID = "iPhone de Bruno";
const char *WIFI_PASSWORD = "70707070";
const char *MQTT_BROKER = "broker.hivemq.com";
const int MQTT_PORT = 1883;
const char *MQTT_CLIENT = "esp32_semaforos_t3";

// ============================================================
// Tópicos MQTT
// ============================================================
// Publicação (ESP32 → Broker) - JSON unificado com todos os semáforos
const char *TOPIC_STATUS = "embarcados/pucrs/semaforo/status";

// Subscrição (Broker → ESP32)
const char *TOPIC_COMANDO = "embarcados/pucrs/semaforo/comando";

// ============================================================
// Pinagem
// ============================================================
#define PIN_SDA 21
#define PIN_SCL 22
#define PIN_BTN_PED 15 // Botão físico do pedestre (pull-up interno)

// ============================================================
// PCF8574 - NOVA PINAGEM (HIGH = LED ACESO - ÂNODO no PCF8574)
// Chip 1 (0x38)
//   P0..P2 = sinaleira 5 (vermelho, amarelo, verde)
//   P3..P5 = sinaleira 4 (vermelho, amarelo, verde)
//   P6..P7 = sinaleira 1 (vermelho, amarelo) - VD no chip2
//   P7      = reservado
//
// Chip 2 (0x39)
//   P0 = led do pedestre
//   P1 = verde da sinaleira 1
//   P2..P4 = sinaleira 2 (vermelho, amarelo, verde)
//   P5..P7 = sinaleira 3 (vermelho, amarelo, verde)
//
// ATENÇÃO: saída do PCF8574 ligada ao ÂNODO → HIGH = LED ACESO
// ============================================================
PCF8574 chip1(0x38);
PCF8574 chip2(0x39);

// Pinagem do chip1
#define S5_VM 0
#define S5_AM 1
#define S5_VD 2
#define S4_VM 3
#define S4_AM 4
#define S4_VD 5
#define S1_VM 6
#define S1_AM 7

// Pinagem do chip2
#define PED_LED 0
#define S1_VD 1
#define S2_VM 2
#define S2_AM 3
#define S2_VD 4
#define S3_VM 5
#define S3_AM 6
#define S3_VD 7

// Compatibilidade com nomenclatura anterior
#define SAT_VM S1_VM
#define SAT_AM S1_AM
#define SAT_VD S1_VD
#define PRO_VM S4_VM
#define PRO_AM S4_AM
#define PRO_VD S4_VD
#define PED_VM PED_LED
#define PED_VD PED_LED

// ============================================================
// Temporização (ms)
// ============================================================
#define T_VERDE_SAT 10000UL	  // Verde Saturnino: 10 s
#define T_VERDE_PRO 15000UL	  // Verde Protásio:  15 s (maior)
#define T_AMARELO 3000UL	  // Amarelo: 3 s
#define T_PEDESTRE 8000UL	  // Pedestre: 8 s de passagem
#define T_PISCA_ATENCAO 500UL // Pisca 1 Hz = 500 ms ON + 500 ms OFF

// ============================================================
// FSM — Estados do ciclo do semáforo
// ============================================================
enum EstadoSemaforo
{
	// 1º TEMPO: S1=VD, S2=VD, S3=VM, S4=VM, S5=VM
	TEMPO1_VERDE,
	TEMPO1_AMARELO,  // Transição para 2º tempo
	
	// 2º TEMPO: S1=VM, S2=VM, S3=VD, S4=VM, S5=VD  
	TEMPO2_VERDE,
	TEMPO2_AMARELO,  // Transição para 3º tempo
	
	// 3º TEMPO: S1=VM, S2=VD, S3=VM, S4=VD, S5=VD
	TEMPO3_VERDE,
	TEMPO3_AMARELO,  // Transição para 1º tempo ou pedestre
	
	PEDESTRE_FASE,   // Todos vermelhos, pedestre atravessa
	ATENCAO		     // Todos amarelo piscante 1 Hz
};

volatile EstadoSemaforo estadoAtual = TEMPO1_VERDE;
volatile EstadoSemaforo estadoAntes = TEMPO1_VERDE; // estado anterior ao modo ATENCAO

volatile bool modoPendente = false; // flag para publicar modo fora do callback
volatile bool modoAtencao = false;
volatile bool pedestreSolicit = false; // botão foi pressionado

// ============================================================
// Objetos globais
// ============================================================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

SemaphoreHandle_t xMqttMutex;
SemaphoreHandle_t xEstadoMutex;

// ============================================================
// Protótipos
// ============================================================
void connectWiFi();
void connectMQTT();
void publishAll(const char *s1, const char *s2, const char *s3, 
				const char *s4, const char *s5, bool pedestre, const char *modo);
void setChip1(bool vmSat, bool amSat, bool vdSat);
void setChip2(bool vmPro, bool amPro, bool vdPro);
void setPedestrianLed(bool active);
void mqttCallback(char *topic, byte *payload, unsigned int length);
void taskMQTT(void *pvParameters);
void taskSemaforo(void *pvParameters);
void taskBotaoPedestre(void *pvParameters);

// ============================================================
// Setup
// ============================================================
void setup()
{
	Serial.begin(115200);

	Wire.begin(PIN_SDA, PIN_SCL);
	chip1.begin();
	chip2.begin();

	// Todos os LEDs apagados no início (LOW = LED apagado)
	for (int i = 0; i < 8; i++)
	{
		chip1.digitalWrite(i, LOW);
		chip2.digitalWrite(i, LOW);
	}

	pinMode(PIN_BTN_PED, INPUT_PULLUP);

	xMqttMutex = xSemaphoreCreateMutex();
	xEstadoMutex = xSemaphoreCreateMutex();

	connectWiFi();
	mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
	mqttClient.setCallback(mqttCallback);
	connectMQTT();

	xTaskCreate(taskMQTT, "MQTT", 4096, NULL, 3, NULL);
	xTaskCreate(taskSemaforo, "Semaforo", 4096, NULL, 2, NULL);
	xTaskCreate(taskBotaoPedestre, "BtnPed", 2048, NULL, 2, NULL);
}

void loop()
{
	vTaskDelay(portMAX_DELAY);
}

// ============================================================
// Conexões
// ============================================================
void connectWiFi()
{
	Serial.printf("[WiFi] Conectando a %s\n", WIFI_SSID);
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
	while (WiFi.status() != WL_CONNECTED)
	{
		vTaskDelay(pdMS_TO_TICKS(500));
		Serial.print(".");
	}
	Serial.printf("\n[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
	Serial.println("[WiFi] Sucesso! Acendendo todos os LEDs por 2 segundos...");
	for (int i = 0; i < 8; i++)
	{
		chip1.digitalWrite(i, HIGH);  // HIGH = aceso
		chip2.digitalWrite(i, HIGH);
	}
	vTaskDelay(pdMS_TO_TICKS(2000));
	for (int i = 0; i < 8; i++)
	{
		chip1.digitalWrite(i, LOW);  // LOW = apagado
		chip2.digitalWrite(i, LOW);
	}
	Serial.println("[WiFi] LEDs desligados. Seguindo com o programa.");
}

void connectMQTT()
{
	mqttClient.setKeepAlive(60);  // Keepalive de 60 segundos
	
	while (!mqttClient.connected())
	{
		Serial.print("[MQTT] Conectando ao broker...");
		if (mqttClient.connect(MQTT_CLIENT))
		{
			Serial.println(" conectado!");
			mqttClient.subscribe(TOPIC_COMANDO);
			Serial.println("[MQTT] Subscrição OK");
		}
		else
		{
			Serial.printf(" falhou (rc=%d). Tentando em 3s...\n", mqttClient.state());
			vTaskDelay(pdMS_TO_TICKS(3000));
		}
	}
}

// ============================================================
// Callback MQTT - processa JSON de comando
// ============================================================
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
	char msg[256];
	unsigned int len = min(length, (unsigned int)255);
	memcpy(msg, payload, len);
	msg[len] = '\0';

	String topicStr = String(topic);

	Serial.printf("[MQTT] [%s]: %s\n", topic, msg);

	if (topicStr == TOPIC_COMANDO)
	{
		// Parse JSON do comando - remove espaços para facilitar parsing
		String msgStr = String(msg);
		msgStr.replace(" ", ""); // Remove todos os espaços
		msgStr.replace("'", "\""); // Troca aspas simples por duplas (se houver)
		
		Serial.printf("[MQTT] Processando: %s\n", msgStr.c_str());
		
		// Verifica modo PISCANTE/NORMAL/ATENCAO
		if (msgStr.indexOf("\"modo\":\"PISCANTE\"") >= 0 || msgStr.indexOf("\"modo\":\"ATENCAO\"") >= 0)
		{
			modoAtencao = true;
			modoPendente = true;
			Serial.println("[MQTT] Comando: Modo PISCANTE/ATENCAO ativado");
		}
		else if (msgStr.indexOf("\"modo\":\"NORMAL\"") >= 0)
		{
			modoAtencao = false;
			modoPendente = true;
			Serial.println("[MQTT] Comando: Modo NORMAL ativado");
		}
		
		// Verifica comando de pedestre
		if (msgStr.indexOf("\"pedestre\":true") >= 0 || msgStr.indexOf("\"pedestre\":\"SOLICITAR\"") >= 0)
		{
			pedestreSolicit = true;
			Serial.println("[MQTT] Comando: Pedestre solicitado");
		}
	}
}

// ============================================================
// Task MQTT — mantém conexão e processa mensagens
// ============================================================
void taskMQTT(void *pvParameters)
{
	for (;;)
	{
		if (WiFi.status() != WL_CONNECTED)
			connectWiFi();
		if (!mqttClient.connected())
			connectMQTT();

		if (xSemaphoreTake(xMqttMutex, pdMS_TO_TICKS(100)))
		{
			mqttClient.loop();
			xSemaphoreGive(xMqttMutex);
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

// ============================================================
// Publica status de todos os semáforos em JSON unificado
// ============================================================
// Mapeamento: S1=S1 (Saturnino), S2=S2, S3=S3, S4=S4 (Protásio), S5=S5
void publishAll(const char *s1, const char *s2, const char *s3, 
				const char *s4, const char *s5, bool pedestre, const char *modo)
{
	if (xSemaphoreTake(xMqttMutex, pdMS_TO_TICKS(300)))
	{
		// Monta JSON no formato do modelo Python
		char json[512];
		snprintf(json, sizeof(json), 
			"{\"s1\":\"%s\",\"s2\":\"%s\",\"s3\":\"%s\",\"s4\":\"%s\",\"s5\":\"%s\",\"pedestre\":%s,\"modo\":\"%s\"}",
			s1, s2, s3, s4, s5, pedestre ? "true" : "false", modo);
		
		mqttClient.publish(TOPIC_STATUS, json, true);
		xSemaphoreGive(xMqttMutex);
	}
	Serial.printf("[FSM] s1=%s s2=%s s3=%s s4=%s s5=%s PED=%s MODO=%s\n", 
		s1, s2, s3, s4, s5, pedestre ? "true" : "false", modo);
}

// ============================================================
// Helpers de sinaleiras - NOVA PINAGEM (HIGH = LED ACESO)
// Chip1: P0-2=S5, P3-5=S4, P6-7=S1(VM,AM)
// Chip2: P0=PED, P1=S1_VD, P2-4=S2, P5-7=S3
// ============================================================
inline void setS1(bool vm, bool am, bool vd)
{
	chip1.digitalWrite(S1_VM, vm ? HIGH : LOW);
	chip1.digitalWrite(S1_AM, am ? HIGH : LOW);
	chip2.digitalWrite(S1_VD, vd ? HIGH : LOW);
}

inline void setS2(bool vm, bool am, bool vd)
{
	chip2.digitalWrite(S2_VM, vm ? HIGH : LOW);
	chip2.digitalWrite(S2_AM, am ? HIGH : LOW);
	chip2.digitalWrite(S2_VD, vd ? HIGH : LOW);
}

inline void setS3(bool vm, bool am, bool vd)
{
	chip2.digitalWrite(S3_VM, vm ? HIGH : LOW);
	chip2.digitalWrite(S3_AM, am ? HIGH : LOW);
	chip2.digitalWrite(S3_VD, vd ? HIGH : LOW);
}

inline void setS4(bool vm, bool am, bool vd)
{
	chip1.digitalWrite(S4_VM, vm ? HIGH : LOW);
	chip1.digitalWrite(S4_AM, am ? HIGH : LOW);
	chip1.digitalWrite(S4_VD, vd ? HIGH : LOW);
}

inline void setS5(bool vm, bool am, bool vd)
{
	chip1.digitalWrite(S5_VM, vm ? HIGH : LOW);
	chip1.digitalWrite(S5_AM, am ? HIGH : LOW);
	chip1.digitalWrite(S5_VD, vd ? HIGH : LOW);
}

inline void setPed(bool active)
{
	chip2.digitalWrite(PED_LED, active ? HIGH : LOW);
}

// ============================================================
// Task Semáforo — FSM principal com 3 tempos
// ============================================================
// Sequência conforme imagem:
// 1º TEMPO: S1=VD, S2=VD, S3=VM, S4=VM, S5=VM
// 2º TEMPO: S1=VM, S2=VM, S3=VD, S4=VM, S5=VD
// 3º TEMPO: S1=VM, S2=VD, S3=VM, S4=VD, S5=VD
// PEDESTRE: Todos VM
// ============================================================
void taskSemaforo(void *pvParameters)
{
	bool piscaLigado = false;

	for (;;)
	{
		// --- Modo Atenção: amarelo piscante 1Hz ---
		if (modoAtencao)
		{
			if (estadoAtual != ATENCAO)
			{
				estadoAntes = estadoAtual;
				estadoAtual = ATENCAO;
				publishAll("AMARELO", "AMARELO", "AMARELO", "AMARELO", "AMARELO", false, "PISCANTE");
			}

			piscaLigado = !piscaLigado;
			// Todos amarelos piscam
			bool am = piscaLigado;
			setS1(false, am, false);
			setS2(false, am, false);
			setS3(false, am, false);
			setS4(false, am, false);
			setS5(false, am, false);
			setPed(false);
			vTaskDelay(pdMS_TO_TICKS(T_PISCA_ATENCAO));
			continue;
		}

		// Voltou do modo atenção → retoma do estado anterior
		if (estadoAtual == ATENCAO)
		{
			estadoAtual = estadoAntes;
		}

		switch (estadoAtual)
		{
		// ======================================================
		// 1º TEMPO: S1=VD, S2=VD, S3=VM, S4=VM, S5=VM
		// ======================================================
		case TEMPO1_VERDE:
		{
			setS1(false, false, true);   // S1=VD
			setS2(false, false, true);   // S2=VD
			setS3(true, false, false);   // S3=VM
			setS4(true, false, false);   // S4=VM
			setS5(true, false, false);   // S5=VM
			setPed(true);
			publishAll("VERDE", "VERDE", "VERMELHO", "VERMELHO", "VERMELHO", false, "NORMAL");

			TickType_t inicio = xTaskGetTickCount();
			TickType_t duracao = pdMS_TO_TICKS(T_VERDE_SAT);

			while ((xTaskGetTickCount() - inicio) < duracao)
			{
				if (modoAtencao) break;
				if (pedestreSolicit) break;
				vTaskDelay(pdMS_TO_TICKS(100));
			}

			if (modoAtencao) break;
			estadoAtual = TEMPO1_AMARELO;
			break;
		}

		case TEMPO1_AMARELO:
		{
			// S1=AM, S2=AM (transição VD→VM)
			setS1(false, true, false);   // S1=AM
			setS2(false, true, false);   // S2=AM
			setS3(true, false, false);   // S3=VM
			setS4(true, false, false);   // S4=VM
			setS5(true, false, false);   // S5=VM
			setPed(pedestreSolicit);
			publishAll("AMARELO", "AMARELO", "VERMELHO", "VERMELHO", "VERMELHO", pedestreSolicit, "NORMAL");

			vTaskDelay(pdMS_TO_TICKS(T_AMARELO));

			if (modoAtencao) break;

			if (pedestreSolicit)
			{
				estadoAtual = PEDESTRE_FASE;
				pedestreSolicit = false;
			}
			else
			{
				estadoAtual = TEMPO2_VERDE;
			}
			break;
		}

		// ======================================================
		// 2º TEMPO: S1=VM, S2=VM, S3=VD, S4=VM, S5=VD
		// ======================================================
		case TEMPO2_VERDE:
		{
			setS1(true, false, false);   // S1=VM
			setS2(true, false, false);   // S2=VM
			setS3(false, false, true);   // S3=VD
			setS4(true, false, false);   // S4=VM
			setS5(false, false, true);   // S5=VD
			setPed(true);
			publishAll("VERMELHO", "VERMELHO", "VERDE", "VERMELHO", "VERDE", false, "NORMAL");

			TickType_t inicio = xTaskGetTickCount();
			TickType_t duracao = pdMS_TO_TICKS(T_VERDE_PRO);

			while ((xTaskGetTickCount() - inicio) < duracao)
			{
				if (modoAtencao) break;
				if (pedestreSolicit) break;
				vTaskDelay(pdMS_TO_TICKS(100));
			}

			if (modoAtencao) break;
			estadoAtual = TEMPO2_AMARELO;
			break;
		}

		case TEMPO2_AMARELO:
		{
			// S3=AM, S5=AM (transição VD→VM)
			setS1(true, false, false);   // S1=VM
			setS2(true, false, false);   // S2=VM
			setS3(false, true, false);   // S3=AM
			setS4(true, false, false);   // S4=VM
			setS5(false, true, false);   // S5=AM
			setPed(pedestreSolicit);
			publishAll("VERMELHO", "VERMELHO", "AMARELO", "VERMELHO", "AMARELO", pedestreSolicit, "NORMAL");

			vTaskDelay(pdMS_TO_TICKS(T_AMARELO));

			if (modoAtencao) break;

			if (pedestreSolicit)
			{
				estadoAtual = PEDESTRE_FASE;
				pedestreSolicit = false;
			}
			else
			{
				estadoAtual = TEMPO3_VERDE;
			}
			break;
		}

		// ======================================================
		// 3º TEMPO: S1=VM, S2=VD, S3=VM, S4=VD, S5=VD
		// ======================================================
		case TEMPO3_VERDE:
		{
			setS1(true, false, false);   // S1=VM
			setS2(false, false, true);   // S2=VD
			setS3(true, false, false);   // S3=VM
			setS4(false, false, true);   // S4=VD
			setS5(false, false, true);   // S5=VD
			setPed(true);
			publishAll("VERMELHO", "VERDE", "VERMELHO", "VERDE", "VERDE", false, "NORMAL");

			TickType_t inicio = xTaskGetTickCount();
			TickType_t duracao = pdMS_TO_TICKS(T_VERDE_PRO);

			while ((xTaskGetTickCount() - inicio) < duracao)
			{
				if (modoAtencao) break;
				if (pedestreSolicit) break;
				vTaskDelay(pdMS_TO_TICKS(100));
			}

			if (modoAtencao) break;
			estadoAtual = TEMPO3_AMARELO;
			break;
		}

		case TEMPO3_AMARELO:
		{
			// S2=AM, S4=AM, S5=AM (transição VD→VM)
			setS1(true, false, false);   // S1=VM
			setS2(false, true, false);   // S2=AM
			setS3(true, false, false);   // S3=VM
			setS4(false, true, false);   // S4=AM
			setS5(false, true, false);   // S5=AM
			setPed(pedestreSolicit);
			publishAll("VERMELHO", "AMARELO", "VERMELHO", "AMARELO", "AMARELO", pedestreSolicit, "NORMAL");

			vTaskDelay(pdMS_TO_TICKS(T_AMARELO));

			if (modoAtencao) break;

			if (pedestreSolicit)
			{
				estadoAtual = PEDESTRE_FASE;
				pedestreSolicit = false;
			}
			else
			{
				estadoAtual = TEMPO1_VERDE;
			}
			break;
		}

		// ======================================================
		// PEDESTRE: Todos VM
		// ======================================================
		case PEDESTRE_FASE:
		{
			setS1(true, false, false);
			setS2(true, false, false);
			setS3(true, false, false);
			setS4(true, false, false);
			setS5(true, false, false);
			setPed(true);
			publishAll("VERMELHO", "VERMELHO", "VERMELHO", "VERMELHO", "VERMELHO", true, "NORMAL");

			vTaskDelay(pdMS_TO_TICKS(T_PEDESTRE));

			if (!modoAtencao)
			{
				estadoAtual = TEMPO1_VERDE;
			}
			break;
		}

		default:
			estadoAtual = TEMPO1_VERDE;
			break;
		}
	}
}

// ============================================================
// Task Botão Pedestre — lê GPIO físico (pull-up: LOW = pressionado)
// ============================================================
void taskBotaoPedestre(void *pvParameters)
{
	bool ultimoEstado = HIGH;

	for (;;)
	{
		bool estadoBtn = digitalRead(PIN_BTN_PED);

		// Borda de descida (pressionado)
		if (ultimoEstado == HIGH && estadoBtn == LOW)
		{
			pedestreSolicit = true;
			Serial.println("[BTN] Pedestre solicitou travessia");

			// Publica no broker para o supervisório refletir (via JSON unificado)
			if (xSemaphoreTake(xMqttMutex, pdMS_TO_TICKS(200)))
			{
				char json[256];
				snprintf(json, sizeof(json), 
					"{\"s1\":\"VERMELHO\",\"s2\":\"VERMELHO\",\"s3\":\"VERMELHO\",\"s4\":\"VERMELHO\",\"s5\":\"VERMELHO\",\"pedestre\":true,\"modo\":\"NORMAL\"}");
				mqttClient.publish(TOPIC_STATUS, json, true);
				xSemaphoreGive(xMqttMutex);
			}
			vTaskDelay(pdMS_TO_TICKS(300)); // debounce
		}

		ultimoEstado = estadoBtn;
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}
