"""
Supervisório de Semáforos — T3 Sistemas Embarcados
Cruzamento: Av. Saturnino de Brito × Av. Protásio Alves

Dependências:
    pip install PyQt5 paho-mqtt

Uso:
    python main.py
"""

import sys
import json
import datetime
from PyQt5 import QtWidgets, uic, QtCore, QtGui
import paho.mqtt.client as mqtt

# ============================================================
# Configuração MQTT (protocolo do amigo)
# ============================================================
BROKER = "broker.hivemq.com"
PORT = 1883

TOPIC_STATUS = "embarcados/pucrs/semaforo/status"
TOPIC_COMANDO = "embarcados/pucrs/semaforo/comando"

# ============================================================
# Paleta de cores (Catppuccin Mocha)
# ============================================================
COR_VM  = "#f38ba8"   # vermelho
COR_AM  = "#f9e2af"   # amarelo
COR_VD  = "#a6e3a1"   # verde
COR_OFF = "#45475a"   # apagado

EMOJI_VM  = "🔴"
EMOJI_AM  = "🟡"
EMOJI_VD  = "🟢"
EMOJI_OFF = "⚫"


# ============================================================
# Bridge de sinais para thread MQTT → main thread Qt
# ============================================================
class MqttSignals(QtCore.QObject):
    mensagem_recebida = QtCore.pyqtSignal(str, str)   # (topic, payload)
    conectado         = QtCore.pyqtSignal()
    desconectado      = QtCore.pyqtSignal()


# ============================================================
# Worker MQTT (roda em QThread separado)
# ============================================================
class MqttWorker(QtCore.QThread):
    def __init__(self, broker: str, porta: int, signals: MqttSignals):
        super().__init__()
        self.broker  = broker
        self.porta   = porta
        self.signals = signals
        self.client  = mqtt.Client(client_id="supervisorio_t3")

        self.client.on_connect    = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message    = self._on_message

    def run(self):
        self.client.connect(self.broker, self.porta, keepalive=60)
        self.client.loop_forever()

    def stop(self):
        self.client.disconnect()
        self.quit()

    def publish(self, topic: str, payload: str):
        self.client.publish(topic, payload)

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            client.subscribe(TOPIC_STATUS)
            self.signals.conectado.emit()
        else:
            self.signals.desconectado.emit()

    def _on_disconnect(self, client, userdata, rc):
        self.signals.desconectado.emit()

    def _on_message(self, client, userdata, msg):
        payload = msg.payload.decode("utf-8", errors="replace")
        self.signals.mensagem_recebida.emit(msg.topic, payload)


# ============================================================
# Janela principal
# ============================================================
class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        uic.loadUi("supervisorio.ui", self)

        self.mqtt_worker: MqttWorker | None = None
        self.mqtt_signals = MqttSignals()
        self.mqtt_signals.mensagem_recebida.connect(self._processar_mensagem)
        self.mqtt_signals.conectado.connect(self._on_conectado)
        self.mqtt_signals.desconectado.connect(self._on_desconectado)

        # Conectar botões
        self.btn_conectar.clicked.connect(self._toggle_conexao)

        # Timer para piscar visualmente o amarelo no supervisório (modo piscante)
        self._pisca_timer = QtCore.QTimer(self)
        self._pisca_timer.setInterval(500)
        self._pisca_timer.timeout.connect(self._piscar_atencao)
        self._pisca_estado = False
        self._modo_atencao_ativo = False

        # Flag para evitar loop quando atualiza checkbox via MQTT
        self._atualizando_checkbox = False

        # Conectar checkbox de modo piscante
        self.chk_piscante.stateChanged.connect(self._alterar_modo)

        self._log("Supervisório iniciado.")

    # ----------------------------------------------------------
    # Conexão MQTT
    # ----------------------------------------------------------
    def _toggle_conexao(self):
        if self.mqtt_worker and self.mqtt_worker.isRunning():
            self.mqtt_worker.stop()
            self.mqtt_worker = None
            self.btn_conectar.setText("Conectar")
        else:
            self.mqtt_worker = MqttWorker(BROKER, PORT, self.mqtt_signals)
            self.mqtt_worker.start()
            self.btn_conectar.setText("Desconectar")
            self._log(f"Conectando a {BROKER}:{PORT}…")

    def _on_conectado(self):
        self.lbl_conn_status.setText("● Conectado")
        self.lbl_conn_status.setStyleSheet("color: #a6e3a1; font-weight: bold;")
        self._log("MQTT conectado ao broker.")

    def _on_desconectado(self):
        self.lbl_conn_status.setText("● Desconectado")
        self.lbl_conn_status.setStyleSheet("color: #f38ba8; font-weight: bold;")
        self._log("MQTT desconectado.")

    # ----------------------------------------------------------
    # Processar mensagens recebidas
    # ----------------------------------------------------------
    def _processar_mensagem(self, topic: str, payload: str):
        self._log(f"[{topic}] {payload}")

        if topic == TOPIC_STATUS:
            try:
                dados = json.loads(payload)
                self._atualizar_todos_semaforos(dados)
            except json.JSONDecodeError as e:
                self._log(f"[ERRO] JSON inválido: {e}")

    # ----------------------------------------------------------
    # Atualizar visuais dos semáforos
    # ----------------------------------------------------------
    def _atualizar_todos_semaforos(self, dados: dict):
        """Processa o JSON completo recebido da ESP"""
        # Atualizar semáforos S1-S5
        for i in range(1, 6):
            chave = f"s{i}"
            if chave in dados:
                self._atualizar_semaforo_individual(i, dados[chave])

        # Atualizar pedestre
        if "pedestre" in dados:
            self._atualizar_pedestre(dados["pedestre"])

        # Atualizar modo (PISCANTE ou NORMAL)
        if "modo" in dados:
            self._atualizar_modo(dados["modo"])

    def _atualizar_semaforo_individual(self, numero: int, status: str):
        """Atualiza um semáforo individual (S1-S5)"""
        vm = status == "VERMELHO"
        am = status == "AMARELO"
        vd = status == "VERDE"

        # Obter widgets dinamicamente
        led_vm = getattr(self, f"led_s{numero}_vm", None)
        led_am = getattr(self, f"led_s{numero}_am", None)
        led_vd = getattr(self, f"led_s{numero}_vd", None)
        lbl_status = getattr(self, f"lbl_s{numero}_status", None)

        if led_vm:
            led_vm.setText(EMOJI_VM if vm else EMOJI_OFF)
        if led_am:
            led_am.setText(EMOJI_AM if am else EMOJI_OFF)
        if led_vd:
            led_vd.setText(EMOJI_VD if vd else EMOJI_OFF)
        if lbl_status:
            lbl_status.setText(status)
            cor = COR_VM if vm else (COR_AM if am else COR_VD)
            lbl_status.setStyleSheet(f"color: {cor}; font-weight: bold; font-size: 13px;")

    def _atualizar_pedestre(self, pedestre_acionado: bool):
        """Atualiza o status do pedestre"""
        if pedestre_acionado:
            self.lbl_ped_status.setText("ACIONADO")
            self.lbl_ped_status.setStyleSheet(f"color: {COR_VD}; font-weight: bold; font-size: 13px;")
            self.led_ped_vd.setText(EMOJI_VD)
            self.led_ped_vm.setText(EMOJI_OFF)
        else:
            self.lbl_ped_status.setText("NÃO ACIONADO")
            self.lbl_ped_status.setStyleSheet("color: #cdd6f4; font-weight: bold; font-size: 13px;")
            self.led_ped_vd.setText(EMOJI_OFF)
            self.led_ped_vm.setText(EMOJI_VM)

    def _atualizar_modo(self, modo: str):
        """Atualiza o modo de operação (NORMAL ou PISCANTE)"""
        piscante = modo == "PISCANTE"
        self._modo_atencao_ativo = piscante

        # Evitar que o checkbox dispare o envio de comando
        self._atualizando_checkbox = True

        if piscante:
            self.lbl_modo_atual.setText("Modo: PISCANTE ⚠")
            self.lbl_modo_atual.setStyleSheet(
                "font-size: 14px; font-weight: bold; color: #f9e2af;")
            self.chk_piscante.setChecked(True)
            if not self._pisca_timer.isActive():
                self._pisca_timer.start()
        else:
            self.lbl_modo_atual.setText("Modo: NORMAL")
            self.lbl_modo_atual.setStyleSheet(
                "font-size: 14px; font-weight: bold; color: #a6e3a1;")
            self.chk_piscante.setChecked(False)
            self._pisca_timer.stop()
            self._pisca_estado = False

        self._atualizando_checkbox = False

    # ----------------------------------------------------------
    # Timer de pisca visual para modo piscante
    # ----------------------------------------------------------
    def _piscar_atencao(self):
        self._pisca_estado = not self._pisca_estado
        emoji = EMOJI_AM if self._pisca_estado else EMOJI_OFF
        # Piscar todos os LEDs amarelos (S1-S5)
        for i in range(1, 6):
            led_am = getattr(self, f"led_s{i}_am", None)
            if led_am:
                led_am.setText(emoji)
            # Apagar vermelho e verde
            led_vm = getattr(self, f"led_s{i}_vm", None)
            led_vd = getattr(self, f"led_s{i}_vd", None)
            if led_vm:
                led_vm.setText(EMOJI_OFF)
            if led_vd:
                led_vd.setText(EMOJI_OFF)

    # ----------------------------------------------------------
    # Comandos do operador
    # ----------------------------------------------------------
    def _alterar_modo(self):
        """Envia comando de mudança de modo quando o checkbox é alterado"""
        if self._atualizando_checkbox:
            return

        if self.mqtt_worker and self.mqtt_worker.isRunning():
            if self.chk_piscante.isChecked():
                payload = {"modo": "PISCANTE"}
            else:
                payload = {"modo": "NORMAL"}

            self.mqtt_worker.publish(TOPIC_COMANDO, json.dumps(payload))
            self._log(f"Comando enviado: {payload}")
        else:
            self._log("[AVISO] Não conectado ao broker.")

    # ----------------------------------------------------------
    # Log
    # ----------------------------------------------------------
    def _log(self, msg: str):
        agora = datetime.datetime.now().strftime("%H:%M:%S")
        self.txt_log.append(f"[{agora}] {msg}")

    # ----------------------------------------------------------
    # Fechar
    # ----------------------------------------------------------
    def closeEvent(self, event):
        if self.mqtt_worker and self.mqtt_worker.isRunning():
            self.mqtt_worker.stop()
            self.mqtt_worker.wait(2000)
        event.accept()


# ============================================================
# Entry point
# ============================================================
if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())
