# T3 — Supervisório de Semáforos

**Cruzamento:** Av. Saturnino de Brito × Av. Protásio Alves  
**Disciplina:** Sistemas Embarcados — PUCRS 2026/1

---

## Estrutura do projeto

```
t3/
├── firmware/              # Firmware ESP32 (PlatformIO)
│   ├── platformio.ini
│   └── src/
│       └── main.cpp
└── supervisorio/          # Supervisório PC (Python + PyQt5)
    ├── main.py
    ├── supervisorio.ui    # Layout QtDesigner
    └── requirements.txt
```

---

## 1. Firmware ESP32 — `firmware/`

### Pré-requisitos
- VS Code + extensão PlatformIO

### Configurar credenciais

Edite as primeiras linhas de `firmware/src/main.cpp`:

```cpp
const char* WIFI_SSID     = "SEU_WIFI";
const char* WIFI_PASSWORD = "SUA_SENHA";
const char* MQTT_BROKER   = "broker.hivemq.com";
```

### Hardware — Pinagem

| Pino ESP32 | Função |
|---|---|
| GPIO 21 | SDA (I2C) |
| GPIO 22 | SCL (I2C) |
| GPIO 15 | Botão pedestre (pull-up interno) |
| 3V3 | Alimentação dos módulos |
| GND | Terra comum |

### Conexões Físicas

#### LEDs da Sinaleira (4 cabos cada módulo)

| Cabo | Conexão |
|---|---|
| VCC | Não conectado / Reservado |
| SDA | GPIO 21 (SDA) - Dados I2C |
| SCL | GPIO 22 (SCL) - Clock I2C |
| 3V3 | 3V3 da ESP32 |

> Os módulos de LED são endereçáveis via I2C (endereços 0x38 e 0x39)

#### Botão Pedestre

| Fio | Conexão |
|---|---|
| Fio 1 | GPIO 15 |
| Fio 2 | GND |

> O botão usa o **pull-up interno** da ESP32. Quando pressionado, o GPIO 15 vai para LOW.

### PCF8574 — Mapeamento de LEDs

**Chip 1 (0x38) — Av. Saturnino de Brito + Pedestre**

| Pino PCF | LED |
|---|---|
| P0 | Vermelho — Saturnino |
| P1 | Amarelo — Saturnino |
| P2 | Verde — Saturnino |
| P3 | Vermelho — Pedestre |
| P4 | Verde — Pedestre |

**Chip 2 (0x39) — Av. Protásio Alves**

| Pino PCF | LED |
|---|---|
| P0 | Vermelho — Protásio |
| P1 | Amarelo — Protásio |
| P2 | Verde — Protásio |

> ⚠ Os LEDs estão ligados com **ânodo em 3V3 e cátodo no PCF8574**.  
> Portanto: `LOW = LED ACESO`, `HIGH = LED APAGADO`.

### Biblioteca necessária

```ini
lib_deps =
    knolleary/PubSubClient@^2.8
    xreef/PCF8574 library@^2.4.0
```

### Gravar

```bash
cd firmware
pio run --target upload
pio device monitor   # 115200 baud
```

---

## 2. Lógica dos Semáforos (FSM)

| Estado | Saturnino | Protásio | Pedestre | Duração |
|---|---|---|---|---|
| SAT_VERDE | 🟢 Verde | 🔴 Vermelho | 🔴 Vermelho | 10 s |
| SAT_AMARELO | 🟡 Amarelo | 🔴 Vermelho | 🔴 Vermelho | 3 s |
| PRO_VERDE | 🔴 Vermelho | 🟢 Verde | 🔴 Vermelho | 15 s |
| PRO_AMARELO | 🔴 Vermelho | 🟡 Amarelo | 🔴 Vermelho | 3 s |
| PEDESTRE_FASE | 🔴 Vermelho | 🔴 Vermelho | 🟢 Verde | 8 s |
| ATENCAO | 🟡 Piscante | 🟡 Piscante | 🔴 Vermelho | — |

> Protásio Alves tem **temporização maior** (15 s > 10 s) conforme requisito.

### Requisitos atendidos

1. ✅ Protásio Alves com temporização maior que Saturnino de Brito
2. ✅ Amarelo sempre antes de mudar de verde para vermelho
3. ✅ Botão pedestre para o trânsito por tempo determinado (8 s)
4. ✅ Status de cada semáforo exibido no supervisório
5. ✅ Botão "Amarelo Piscante" no supervisório (1 Hz, modo atenção)

---

## 3. Tópicos MQTT

| Direção | Tópico | Conteúdo |
|---|---|---|
| Publish | `semaforo/saturnino/status` | `VERDE` / `AMARELO` / `VERMELHO` |
| Publish | `semaforo/protasio/status`  | `VERDE` / `AMARELO` / `VERMELHO` |
| Publish | `semaforo/pedestre/status`  | `LIVRE` / `AGUARDANDO` / `ATRAVESSANDO` |
| Publish | `semaforo/modo`             | `NORMAL` / `ATENCAO` |
| Subscribe | `semaforo/comando/modo`   | `ATENCAO` / `NORMAL` |
| Subscribe | `semaforo/comando/pedestre` | `SOLICITAR` |

---

## 4. Supervisório PC — `supervisorio/`

### Pré-requisitos

```bash
pip install -r supervisorio/requirements.txt
```

### Executar

```bash
cd supervisorio
python main.py
```

### Funcionalidades da interface

- **Status visual** de cada semáforo (emojis 🔴🟡🟢) em tempo real via MQTT
- **Status do pedestre** (LIVRE / AGUARDANDO / ATRAVESSANDO)
- **Botão "Solicitar Travessia"** — simula o botão físico pelo supervisório
- **Botão "⚠ Amarelo Piscante"** — ativa modo atenção em todos os semáforos
- **Botão "▶ Retomar Normal"** — volta ao ciclo normal
- **Log de eventos** com timestamp
- **Campo configurável** de broker/porta

---

## Fluxo do sistema

```
ESP32 ──[I2C]──► PCF8574 (0x38) ──► LEDs Saturnino + Pedestre
      ──[I2C]──► PCF8574 (0x39) ──► LEDs Protásio
      ──[Wi-Fi]──► broker.hivemq.com ──[MQTT]──► Supervisório Python/PyQt5
                        ▲
                        └──[MQTT]──── Comandos do Supervisório
```
