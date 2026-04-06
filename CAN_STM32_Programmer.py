import sys
import os
import time
import can
import cantools
from serial.tools import list_ports
 
from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, 
                             QHBoxLayout, QPushButton, QLabel, QComboBox, 
                             QFileDialog, QTextEdit, QMessageBox, QLineEdit, QGroupBox,
                             QProgressBar)
from PyQt5.QtCore import Qt, QThread, pyqtSignal
from PyQt5.QtGui import QPixmap, QPainter
import base64 as _b64
 
 
 
# --- Constantes do Protocolo STM32 (AN3154) ---
ACK = 0x79
NACK = 0x1F
CMD_SYNC = 0x79
CMD_GET = 0x00
CMD_GET_VERSION = 0x01
CMD_GET_ID = 0x02
CMD_SPEED = 0x03
CMD_READ = 0x11
CMD_GO = 0x21
CMD_WRITE = 0x31
CMD_ERASE = 0x43
 
APP_BITRATE = 250000
BOOT_BITRATE = 125000
BOOTLOADER_DATA_ID = 0x04   # AN3154 recommends 0x04 for write data frames
 
WRITE_CHUNK_SIZE = 256       # AN3154 allows up to 256 bytes
BASE_ADDRESS = 0x08000000    # normal STM32F412 flash base
 
def open_slcan(port, bitrate):
    return can.interface.Bus(
        interface='slcan',
        channel=port,
        bitrate=bitrate
    )
 
def retry_open_slcan(port, bitrate, retries=10, delay=0.5):
    last_exc = None
    for _ in range(retries):
        try:
            return open_slcan(port, bitrate)
        except Exception as e:
            last_exc = e
            time.sleep(delay)
    raise last_exc
 
def parse_can_id(text: str):
    text = text.strip()
    if not text:
        return None
 
    if text.lower().startswith("0x"):
        return int(text, 16)
 
    if text.lower().endswith("h"):
        return int(text[:-1], 16)
 
    return int(text, 10)
 
class FlasherThread(QThread):
    log_signal = pyqtSignal(str)
    progress_signal = pyqtSignal(int)
    
    def __init__(self, port, filepath, jump_id, jump_data, app_bitrate=1000000):
        super().__init__()
        self.port = port
        self.filepath = filepath
        self.app_bitrate = app_bitrate
        
        # Converter string hexadecimal do ID (ex: "123") para inteiro
        try:
            self.jump_id = parse_can_id(jump_id)
        except ValueError:
            self.jump_id = None
            
        # Converter string hexadecimal dos dados (ex: "AA BB CC") para lista de inteiros
        try:
            if jump_data.strip():
                self.jump_data = [int(x, 16) for x in jump_data.split()]
            else:
                self.jump_data = []
        except ValueError:
            self.jump_data = []
    
    def _send_std(self, bus, std_id, data=b""):
        if isinstance(data, list):
            data = bytes(data)
        msg = can.Message(
            arbitration_id=std_id,
            data=data,
            is_extended_id=False
        )
        bus.send(msg)
 
    def _recv_until_ack(self, bus, expected_id=None, timeout=2.0, ignore_echo_ids=None):
        """
        Wait for ACK/NACK.
        expected_id: if set, only accept ACK/NACK on that Std ID.
        ignore_echo_ids: iterable of IDs that we sent and want to ignore if adapter echoes them.
        """
        start = time.time()
        ignore_echo_ids = set(ignore_echo_ids or [])
 
        while (time.time() - start) < timeout:
            msg = bus.recv(timeout=0.1)
            if msg is None:
                continue
 
            # ignore non-standard frames
            if msg.is_extended_id:
                continue
 
            # ignore empty frames
            if len(msg.data) < 1:
                continue
 
            # avoid false ACK from our own sent traffic on echoing adapters
            if msg.arbitration_id in ignore_echo_ids and msg.data[0] not in (ACK, NACK):
                continue
 
            if expected_id is not None and msg.arbitration_id != expected_id:
                continue
 
            if msg.data[0] == ACK:
                return True
 
            if msg.data[0] == NACK:
                self.log_signal.emit(f"ERRO: NACK recebido no ID 0x{msg.arbitration_id:03X}.")
                return False
 
        self.log_signal.emit("ERRO: Timeout à espera de ACK/NACK.")
        return False
 
    def _sync_bootloader(self, bus):
        # AN3154: TX CAN ID=0x079, DLC=1, data=0x00 every 1ms until ACK.
        #
        # Recovery: if a previous write failed mid-chunk the bootloader stays
        # in write-data mode and ACKs every frame on ID=0x031.  Once it gets
        # enough bytes it finishes the write and returns to command mode.
        # At that point our sync frame gets NACKed on ID=0x079 — meaning the
        # bootloader is in command mode (already synced from the first session).
        # Treat NACK on sync as "already synced, safe to proceed".
        self.log_signal.emit("A sincronizar com bootloader (TX: ID=0x079 data=[00] a cada 1ms)...")
        pending_write_flush = False
        deadline = time.time() + 10.0
        while time.time() < deadline:
            self._send_std(bus, CMD_SYNC, bytes([0x00]))   # CAN ID=0x79, DLC=1, data=0x00
            time.sleep(0.001)
            msg = bus.recv(timeout=0.0)
            if msg is None:
                continue
 
            ext = "EXT" if msg.is_extended_id else "STD"
            data_hex = " ".join(f"{b:02X}" for b in msg.data)
            self.log_signal.emit(
                f"  [RX] {ext} ID=0x{msg.arbitration_id:03X}  DLC={msg.dlc}  data=[{data_hex}]"
            )
            if msg.is_extended_id or len(msg.data) < 1:
                continue
 
            # Normal sync ACK
            if msg.arbitration_id == CMD_SYNC and msg.data[0] == ACK:
                self.log_signal.emit("Bootloader sincronizado! ACK recebido em ID=0x079.")
                return True
 
            # NACK on sync = bootloader already in command mode from a previous
            # session that was partially recovered — safe to continue.
            if msg.arbitration_id == CMD_SYNC and msg.data[0] == NACK:
                self.log_signal.emit(
                    "Bootloader já em modo de comando (sessão anterior recuperada). "
                    "A retomar programação..."
                )
                return True
 
            # ACK on CMD_WRITE = bootloader stuck in write-data mode from a
            # failed previous write.  Feed it dummy frames to flush the chunk.
            if msg.arbitration_id == CMD_WRITE and msg.data[0] == ACK:
                if not pending_write_flush:
                    self.log_signal.emit(
                        "Bootloader preso em modo de escrita pendente — "
                        "a enviar dados dummy para limpar..."
                    )
                    pending_write_flush = True
                self._send_std(bus, BOOTLOADER_DATA_ID, bytes(8))
 
        self.log_signal.emit(
            "ERRO: Timeout no sync — bootloader não respondeu. "
            "Verifica se entrou no bootloader e se o baudrate é 125k."
        )
        return False
 
    def _get_id(self, bus):
        self.log_signal.emit("A pedir o Chip ID (comando Get ID, Std ID = 0x02)...")
        self._send_std(bus, CMD_GET_ID, b"")
 
        start = time.time()
        got_first_ack = False
        chip_id = None
 
        while (time.time() - start) < 2.0:
            msg = bus.recv(timeout=0.1)
            if msg is None or msg.is_extended_id:
                continue
            if msg.arbitration_id != CMD_GET_ID or len(msg.data) < 1:
                continue
 
            if not got_first_ack:
                if msg.data[0] == ACK:
                    got_first_ack = True
                    continue
                if msg.data[0] == NACK:
                    self.log_signal.emit("ERRO: NACK no Get ID.")
                    return None
            else:
                # one message with PID bytes, then final ACK
                if len(msg.data) >= 2 and chip_id is None:
                    chip_id = (msg.data[0] << 8) | msg.data[1]
                    continue
                if msg.data[0] == ACK:
                    return chip_id
 
        self.log_signal.emit("ERRO: Timeout no Get ID.")
        return None
 
    def _global_erase(self, bus):
        self.log_signal.emit("A iniciar Global Erase (Std ID = 0x43, data=0xFF)...")
        self._send_std(bus, CMD_ERASE, [0xFF])
 
        # AN3154 erase flow: wait for 2 ACK or 1 NACK for global erase
        if not self._recv_until_ack(bus, expected_id=CMD_ERASE, timeout=2.0, ignore_echo_ids={CMD_ERASE}):
            return False
 
        self.log_signal.emit("Erase em curso...")
        if not self._recv_until_ack(bus, expected_id=CMD_ERASE, timeout=20.0, ignore_echo_ids={CMD_ERASE}):
            return False
 
        self.log_signal.emit("Flash apagada com sucesso.")
        return True
 
    def _write_block(self, bus, address, chunk: bytes):
        if not (1 <= len(chunk) <= 256):
            raise ValueError("Chunk inválido para Write Memory.")
 
        n_minus_1 = len(chunk) - 1
        cmd_payload = [
            (address >> 24) & 0xFF,
            (address >> 16) & 0xFF,
            (address >> 8) & 0xFF,
            address & 0xFF,
            n_minus_1
        ]
 
        # command frame: Std ID = 0x31, DLC = 5
        self._send_std(bus, CMD_WRITE, cmd_payload)
 
        # ACK to command frame
        if not self._recv_until_ack(bus, expected_id=CMD_WRITE, timeout=2.0, ignore_echo_ids={CMD_WRITE}):
            self.log_signal.emit(f"ERRO: STM32 rejeitou comando Write em 0x{address:08X}.")
            return False
 
        # data frames: any std ID 0x00..0xFF; 0x04 recommended by AN3154
        for i in range(0, len(chunk), 8):
            frame = chunk[i:i + 8]
            self._send_std(bus, BOOTLOADER_DATA_ID, frame)
 
            # after each received message, device sends ACK/NACK
            if not self._recv_until_ack(bus, expected_id=CMD_WRITE, timeout=2.0, ignore_echo_ids={BOOTLOADER_DATA_ID}):
                self.log_signal.emit(f"ERRO: Falha durante envio de dados para 0x{address:08X}.")
                return False
 
        # AN3154: after last data message, host gets one more ACK after programming succeeds
        if not self._recv_until_ack(bus, expected_id=CMD_WRITE, timeout=3.0, ignore_echo_ids={BOOTLOADER_DATA_ID}):
            self.log_signal.emit(f"ERRO: Falha na programação final do bloco em 0x{address:08X}.")
            return False
 
        return True
 
    def run(self):
        bus = None
        try:
            # 1) app bus
            self.log_signal.emit(f"A abrir SLCAN em {self.port} a {self.app_bitrate} bps...")
            bus = retry_open_slcan(self.port, self.app_bitrate, retries=5, delay=0.3)
            time.sleep(0.5)
 
            # optional jump frame, sent ONCE only
            if self.jump_id is not None:
                self.log_signal.emit(f"A enviar comando de salto no CAN normal (ID: 0x{self.jump_id:X})...")
                self._send_std(bus, self.jump_id, self.jump_data)
                self.log_signal.emit("Comando de salto enviado. A aguardar entrada no bootloader...")
                time.sleep(1.0)
 
            bus.shutdown()
            bus = None
            time.sleep(0.3)
 
            # 2) bootloader bus at 125 kbps
            self.log_signal.emit(f"A reabrir SLCAN em {self.port} a {BOOT_BITRATE} bps...")
            bus = retry_open_slcan(self.port, BOOT_BITRATE, retries=12, delay=0.5)
            time.sleep(0.5)
 
            # 3) sync
            if not self._sync_bootloader(bus):
                return
 
            self.log_signal.emit("Bootloader respondeu ao sync.")
 
            # 4) optional sanity check: Get ID
            chip_id = self._get_id(bus)
            if chip_id is None:
                self.log_signal.emit("Aviso: não foi possível ler o Chip ID, mas vou continuar.")
            else:
                self.log_signal.emit(f"Chip ID recebido: 0x{chip_id:04X}")
 
            # 5) erase
            if not self._global_erase(bus):
                return
 
            # 6) program
            with open(self.filepath, "rb") as f:
                firmware = f.read()
 
            total_bytes = len(firmware)
            self.log_signal.emit(f"A iniciar gravação de {total_bytes} bytes em 0x{BASE_ADDRESS:08X}...")
 
            for offset in range(0, total_bytes, WRITE_CHUNK_SIZE):
                chunk = firmware[offset:offset + WRITE_CHUNK_SIZE]
                addr = BASE_ADDRESS + offset
 
                if not self._write_block(bus, addr, chunk):
                    self.log_signal.emit(
                        f"ERRO: Gravação falhou no bloco 0x{addr:08X} "
                        f"({offset // WRITE_CHUNK_SIZE + 1} de "
                        f"{(total_bytes + WRITE_CHUNK_SIZE - 1) // WRITE_CHUNK_SIZE})."
                    )
                    self.log_signal.emit(
                        "⚠ A flash foi apagada mas está incompleta — NÃO faças reset manual. "
                        "O bootloader continua activo. Tenta programar de novo."
                    )
                    self.progress_signal.emit(0)
                    return
 
                progress = int(((offset + len(chunk)) / total_bytes) * 100)
                self.progress_signal.emit(progress)
 
            self.log_signal.emit("Programação concluída com sucesso.")
 
            # 7) Go command — AN3154 §3.6
            # CAN ID=0x21, DLC=4, data = jump address (0x08000000 = flash base)
            self.log_signal.emit("A enviar Go command (salto para 0x08000000)...")
            go_addr = BASE_ADDRESS
            go_payload = [
                (go_addr >> 24) & 0xFF,
                (go_addr >> 16) & 0xFF,
                (go_addr >>  8) & 0xFF,
                 go_addr        & 0xFF,
            ]
            self._send_std(bus, CMD_GO, go_payload)
            # bootloader sends ACK then jumps — short timeout is enough
            ack = self._recv_until_ack(bus, expected_id=CMD_GO, timeout=2.0,
                                       ignore_echo_ids={CMD_GO})
            if ack:
                self.log_signal.emit("STM32 a arrancar aplicação. Programação completa! ✓")
            else:
                self.log_signal.emit(
                    "Aviso: Go command sem ACK — o STM32 pode ter saltado na mesma. "
                    "Faz reset manual se necessário."
                )
 
        except Exception as e:
            self.log_signal.emit(f"ERRO CRÍTICO: {e}")
        finally:
            if bus is not None:
                bus.shutdown()
 
 
class STM32CanFlasher(QMainWindow):
    def paintEvent(self, event):
        """Draw background image at 20% opacity."""
        super().paintEvent(event)
        if not hasattr(self, '_bg_pixmap'):
            try:
                from PyQt5.QtCore import QByteArray
                img_bytes = _b64.b64decode(_BG_B64.encode('ascii', errors='ignore'))
                ba = QByteArray(img_bytes)
                pm = QPixmap()
                ok = pm.loadFromData(ba)
                self._bg_pixmap = pm if ok else None
            except Exception:
                self._bg_pixmap = None
        if self._bg_pixmap is None:
            return
        painter = QPainter(self)
        painter.setOpacity(0.20)
        pm = self._bg_pixmap.scaled(
            self.size(),
            Qt.KeepAspectRatio,
            Qt.SmoothTransformation
        )
        x = (self.width()  - pm.width())  // 2
        y = 0  # top
        painter.drawPixmap(x, y, pm)
        painter.end()
 
    def __init__(self):
        super().__init__()
        self.setWindowTitle("STM32 CAN Flasher - Protocolo AN3154")
        # Window icon from embedded image
        from PyQt5.QtGui import QIcon
        from PyQt5.QtCore import QByteArray
        try:
            _icon_bytes = _b64.b64decode(_BG_B64.encode('ascii', errors='ignore'))
            _icon_ba = QByteArray(_icon_bytes)
            _icon_pm = QPixmap()
            _icon_pm.loadFromData(_icon_ba)
            self.setWindowIcon(QIcon(_icon_pm))
        except Exception:
            pass  # imagem de ícone inválida — ignorar
        self.resize(700, 550)
        self.filepath = None
 
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        layout = QVBoxLayout(main_widget)
 
        # Configurações do Adaptador SLCAN
        group_adapter = QGroupBox("Configuração do Adaptador")
        adapter_layout = QHBoxLayout()
 
        self.port_input = QComboBox()
        self.port_input.setEditable(True)
 
        self.btn_refresh_ports = QPushButton("Atualizar")
        self.btn_refresh_ports.clicked.connect(self.refresh_ports)
 
        adapter_layout.addWidget(QLabel("Porta COM:"))
        adapter_layout.addWidget(self.port_input, stretch=1)
        adapter_layout.addWidget(self.btn_refresh_ports)
 
        group_adapter.setLayout(adapter_layout)
        layout.addWidget(group_adapter)
 
        # Configurações do Comando de Salto
        group_jump = QGroupBox("Comando para Saltar para o Bootloader (Opcional)")
        jump_layout = QHBoxLayout()
        self.jump_id_input = QLineEdit()
        self.jump_id_input.setPlaceholderText("Ex: 121 ou 0x79")
        self.jump_id_input.setText("0x0dc")   # default from your DBC
 
        self.jump_data_input = QLineEdit()
        self.jump_data_input.setPlaceholderText("Ex: 01 05 AA BB")
        self.jump_data_input.setText("01")  # programming_request = 1
 
        self.app_bitrate_combo = QComboBox()
        self.app_bitrate_combo.addItem("1 Mbit/s",  1000000)
        self.app_bitrate_combo.addItem("500 kbit/s", 500000)
        self.app_bitrate_combo.addItem("250 kbit/s", 250000)
        self.app_bitrate_combo.addItem("125 kbit/s", 125000)
        self.app_bitrate_combo.setCurrentIndex(0)  # 1 Mbit/s default
 
        jump_layout.addWidget(QLabel("CAN ID (Hex):"))
        jump_layout.addWidget(self.jump_id_input)
        jump_layout.addWidget(QLabel("Dados (Hex, c/ espaços):"))
        jump_layout.addWidget(self.jump_data_input)
        jump_layout.addWidget(QLabel("Bitrate do CAN normal:"))
        jump_layout.addWidget(self.app_bitrate_combo)
        group_jump.setLayout(jump_layout)
        layout.addWidget(group_jump)
 
        # Seleção do Ficheiro Binário
        file_layout = QHBoxLayout()
        self.file_label = QLabel("Nenhum ficheiro selecionado")
        self.file_label.setStyleSheet("border: 1px solid #aaa; padding: 8px; background-color: #f9f9f9;")
        btn_browse = QPushButton("Procurar Ficheiro .bin")
        btn_browse.clicked.connect(self.select_file)
        file_layout.addWidget(self.file_label, stretch=1)
        file_layout.addWidget(btn_browse)
        layout.addLayout(file_layout)
 
        # Botão Programar
        self.btn_flash = QPushButton("Iniciar Programação do STM32")
        self.btn_flash.setStyleSheet("background-color: #0056b3; color: white; font-weight: bold; padding: 15px; font-size: 14px;")
        self.btn_flash.clicked.connect(self.start_flashing)
        layout.addWidget(self.btn_flash)
 
        # Barra de Progresso
        self.progress_bar = QProgressBar()
        self.progress_bar.setValue(0)
        layout.addWidget(self.progress_bar)
 
        # Consola de Registo (Log)
        self.log_console = QTextEdit()
        self.log_console.setReadOnly(True)
        self.log_console.setStyleSheet("font-family: Consolas, monospace; background-color: #2b2b2b; color: #a9b7c6;")
        layout.addWidget(QLabel("Registo de Operações:"))
        layout.addWidget(self.log_console)
 
        self.refresh_ports()
 
    def select_file(self):
        options = QFileDialog.Options()
        file_name, _ = QFileDialog.getOpenFileName(self, "Selecionar Firmware", "", 
                                                   "Ficheiros Binários (*.bin);;Todos os Ficheiros (*)", 
                                                   options=options)
        if file_name:
            self.filepath = file_name
            self.file_label.setText(os.path.basename(file_name))
            self.log_message(f"Ficheiro carregado: {file_name}")
 
    def log_message(self, message):
        self.log_console.append(message)
        # Forçar o scroll a ir sempre para o fim
        scrollbar = self.log_console.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())
 
    def update_progress(self, value):
        self.progress_bar.setValue(value)
 
    def start_flashing(self):
        if not self.filepath:
            QMessageBox.warning(self, "Aviso", "Por favor, seleciona o ficheiro de firmware (.bin).")
            return
 
        # Desativar botão e resetar UI
        self.btn_flash.setEnabled(False)
        self.progress_bar.setValue(0)
        self.log_console.clear()
        
        # Iniciar a Thread de Flashing
        self.thread = FlasherThread(
            port=self.port_input.currentData() or self.port_input.currentText().split(" - ")[0],
            filepath=self.filepath,
            jump_id=self.jump_id_input.text(),
            jump_data=self.jump_data_input.text(),
            app_bitrate=self.app_bitrate_combo.currentData()
        )
        
        # Conectar sinais
        self.thread.log_signal.connect(self.log_message)
        self.thread.progress_signal.connect(self.update_progress)
        self.thread.finished.connect(lambda: self.btn_flash.setEnabled(True))
        
        # Executar
        self.thread.start()
 
 
    def refresh_ports(self):
        self.port_input.clear()
 
        ports = list(list_ports.comports())
 
        if not ports:
            self.port_input.addItem("Nenhuma porta encontrada")
            self.log_message("Nenhuma porta série/COM foi encontrada.")
            return
 
        found_slcan_like = False
 
        for port in ports:
            device = port.device              # ex: COM7
            desc = port.description or ""
            manuf = port.manufacturer or ""
            hwid = port.hwid or ""
 
            label = f"{device} - {desc}"
            lower_text = f"{desc} {manuf} {hwid}".lower()
 
            # Heurística simples para destacar adaptadores USB-Serial/SLCAN
            if any(k in lower_text for k in ["usb serial", "ch340", "cp210", "ftdi", "slcan", "canable", "lawicel"]):
                label += "  [possível SLCAN]"
                found_slcan_like = True
 
            self.port_input.addItem(label, device)
 
        if found_slcan_like:
            self.log_message("Portas detetadas. Algumas parecem ser adaptadores USB-Serial/SLCAN.")
        else:
            self.log_message("Portas detetadas, mas nenhuma foi claramente identificada como SLCAN.")
 
        # Auto-select COM15 if available, otherwise leave whatever is first
        for i in range(self.port_input.count()):
            if self.port_input.itemData(i) == "COM15":
                self.port_input.setCurrentIndex(i)
                self.log_message("COM15 selecionada automaticamente.")
                break
            
# --- Background image (embedded) ---
_BG_B64 = """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"""
 
if __name__ == '__main__':
    app = QApplication(sys.argv)
    
    # Aplicar um estilo genérico para a aplicação ficar mais limpa
    app.setStyle("Fusion")
    
    window = STM32CanFlasher()
    window.show()
    sys.exit(app.exec_())