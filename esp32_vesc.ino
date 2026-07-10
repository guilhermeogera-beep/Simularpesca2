/*
  SIMULADOR DE PESCA v2 — ESP32-C3 Mini + VESC 75100 (UART)
  ---------------------------------------------------------
  Diferença pro v1: o motor agora é um BLDC controlado pelo VESC via UART.
  Isso permite PUXADA (corrente pra frente) E FRENAGEM ATIVA (corrente de freio),
  cada uma com sua RAMPA. A linha recolhida vem do TACÔMETRO do VESC (sem AS5600).

  CONCEITO:
  - O app manda uma FORÇA COM SINAL (-100..+100):
      força > 0  -> PUXA  (o peixe corre / tira linha)   -> corrente pra frente
      força < 0  -> FREIA (resistência / peso do peixe)   -> corrente de freio
      força = 0  -> solto (sem torque)
    O "peso do peixe" é aplicado NO APP (multiplica a curva) — aqui só executamos a força.
  - PUXADA = controle por ROTAÇÃO (RPM): o valor da curva vira a rotação de enrolar. O VESC
    segura essa rotação com torque alto -> o motor ENROLA SEMPRE (imparável, mesmo você forçando).
    O valor mapeia entre MIN_RPM e MAX_RPM (calibráveis pelo app). O FREIO é só corrente (resistência).
  - As rampas (subida/descida) são feitas AQUI no firmware pra suavizar acelerar e frear.

  HARDWARE:
  - ESP32-C3 Mini
  - VESC 75100 na UART:  ESP GPIO5(TX)->VESC RX,  ESP GPIO6(RX)<-VESC TX,  GND comum
  - Antes de usar: no VESC Tool rode a DETECÇÃO do motor e habilite o app UART (115200).

  BIBLIOTECA: "VescUart" (by SolidGeek) — instalar pelo Library Manager.
*/

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <VescUart.h>

// ── UART pro VESC ─────────────────────────────────────────────
#define VESC_TX   5      // ESP TX  -> VESC RX
#define VESC_RX   6      // ESP RX  <- VESC TX
#define VESC_BAUD 115200

// ── LIMITES E RAMPAS (ajustar depois da detecção no VESC Tool) ─
#define MAX_PUXADA_A   30.0f   // (LEGADO — puxada virou setRPM; não usado)
#define MAX_FREIO_A    25.0f   // corrente (A) do FREIO máximo = base do gráfico (valor -FREIO_ESC)
#define FREIO_ESC      20.0f   // valor da curva (-20 = base do gráfico) que corresponde ao FREIO máximo
#define RAMPA_SOBE     250.0f  // força/s subindo  (rampa de ACELERAÇÃO)
#define RAMPA_DESCE    300.0f  // força/s descendo (rampa de FRENAGEM, costuma poder ser mais rápida)
// PUXADA por ROTAÇÃO: o valor da curva (0..FORCA_MAX) vira ERPM entre MIN_RPM e MAX_RPM.
// MIN/MAX_RPM são CALIBRÁVEIS pelo app (canal CONFIG) — não precisa regravar pra ajustar os metros.
#define FORCA_MAX  40.0f       // valor da curva que corresponde ao RPM MÁX (o gráfico vai até 40)
float MIN_RPM = 500.0f;        // ERPM da corrida mais fraca (valor baixo) — evita o motor travar
float MAX_RPM = 8000.0f;       // ERPM da corrida no talo (valor 40) — "capa" quantos metros o peixe leva
#define RPM_SINAL  1           // se o motor enrolar pro lado ERRADO, troque pra -1

// ── UUIDs BLE (mantidos do v1 pra o app conectar igual) ───────
#define SERVICE_UUID      "a1b2c3d4-0001-4a5b-8c6d-1234567890ab"
#define CHAR_MOTOR_UUID   "a1b2c3d4-0002-4a5b-8c6d-1234567890ab"   // agora: 1 byte int8 FORÇA (-100..+100)
#define CHAR_LINHA_UUID   "a1b2c3d4-0003-4a5b-8c6d-1234567890ab"   // tacômetro do VESC (int32 LE); write zera
#define CHAR_CONFIG_UUID  "a1b2c3d4-0004-4a5b-8c6d-1234567890ab"   // app envia RPM mín/máx (2x uint16 LE)
#define BLE_NOME  "SimuladorPesca"

VescUart vesc;
bool conectado = false;

// ── Estado da força (com rampa) ───────────────────────────────
float forcaAlvo  = 0.0f;   // -100..+100 vinda do app
float forcaAtual = 0.0f;   // valor já rampado, aplicado no VESC
unsigned long tUltRamp = 0;

// ── Trava de segurança (watchdog) ─────────────────────────────
#define MOTOR_TIMEOUT_MS 1200
unsigned long ultimoComando = 0;

// ── Linha: tacômetro do VESC ──────────────────────────────────
long tacoOffset = 0;       // pra "zerar" a contagem sem mexer no VESC
long tacoBruto  = 0;       // último tacômetro lido
long contagem   = 0;       // tacoBruto - tacoOffset (o que o app vê)
long ultimaEnviada = 0;

BLECharacteristic* chLinha = nullptr;

// ── Aplica a força no VESC: + puxa, - freia, 0 solta ──────────
void aplicarForca(float f) {
  if (f >  100) f =  100;
  if (f < -100) f = -100;
  if (f > 0.5f) {                                          // PUXA — controle por ROTAÇÃO (enrola sempre)
    float frac = f / FORCA_MAX;  if (frac > 1.0f) frac = 1.0f;   // valor 0..40 -> 0..1
    float rpm = MIN_RPM + frac * (MAX_RPM - MIN_RPM);      // mapeia pra rotação (mín..máx)
    vesc.setRPM((int32_t)(rpm * RPM_SINAL));               // o VESC segura essa rotação com torque alto = imparável
  }
  else if (f < -0.5f) { float fb = -f / FREIO_ESC; if (fb > 1.0f) fb = 1.0f; vesc.setBrakeCurrent(fb * MAX_FREIO_A); }   // FREIA — resistência: -20 (base do gráfico) = freio máximo
  else                 vesc.setCurrent(0.0f);                              // solto (coast)
}
void pararTudo() { forcaAlvo = 0; forcaAtual = 0; vesc.setCurrent(0.0f); }

// ── Envia a contagem (int32 LE) por BLE ───────────────────────
void enviarLinha() {
  if (!chLinha || !conectado) return;
  long v = contagem;
  uint8_t buf[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
  chLinha->setValue(buf, 4);
  chLinha->notify();
  ultimaEnviada = v;
}

// ── Callback: FORÇA do app (1 byte int8 -100..+100) ───────────
class MotorCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    uint8_t* d = c->getData();
    if (d && c->getLength() >= 1) {
      forcaAlvo = (float)((int8_t)d[0]);      // interpreta como signed
      ultimoComando = millis();
    }
  }
};

// ── Callback: app escreve na linha => zera a contagem ─────────
class LinhaCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) { tacoOffset = tacoBruto; contagem = 0; enviarLinha(); }
};

// ── Callback: CONFIG do app => RPM mín/máx (2x uint16 LE) ──────
class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    uint8_t* d = c->getData();
    if (d && c->getLength() >= 4) {
      MIN_RPM = (float)(d[0] | (d[1] << 8));
      MAX_RPM = (float)(d[2] | (d[3] << 8));
      if (MAX_RPM < MIN_RPM) MAX_RPM = MIN_RPM;   // segurança
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s)    { conectado = true; }
  void onDisconnect(BLEServer* s) { conectado = false; pararTudo(); BLEDevice::startAdvertising(); }
};

void setup() {
  Serial.begin(115200);
  Serial1.begin(VESC_BAUD, SERIAL_8N1, VESC_RX, VESC_TX);
  vesc.setSerialPort(&Serial1);
  delay(300);
  Serial.println("\n=== SIMULADOR DE PESCA v2 (BLE + VESC) ===");

  BLEDevice::init(BLE_NOME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService* service = server->createService(SERVICE_UUID);

  BLECharacteristic* chMotor = service->createCharacteristic(
    CHAR_MOTOR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  chMotor->setCallbacks(new MotorCallbacks());

  chLinha = service->createCharacteristic(
    CHAR_LINHA_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  chLinha->addDescriptor(new BLE2902());
  chLinha->setCallbacks(new LinhaCallbacks());
  uint8_t zero[4] = {0,0,0,0}; chLinha->setValue(zero, 4);

  BLECharacteristic* chConfig = service->createCharacteristic(
    CHAR_CONFIG_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  chConfig->setCallbacks(new ConfigCallbacks());

  service->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID); adv->setScanResponse(true); adv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.printf("[OK] BLE \"%s\" anunciando\n", BLE_NOME);
}

unsigned long tLeitura = 0, tEnvio = 0;
void loop() {
  unsigned long agora = millis();

  // ── RAMPA: move forcaAtual -> forcaAlvo (sobe/desce em taxas diferentes) ──
  if (tUltRamp == 0) tUltRamp = agora;
  float dt = (agora - tUltRamp) / 1000.0f; tUltRamp = agora;
  if (dt > 0.05f) dt = 0.05f;                 // limita saltos
  // APLICAR força (afastando de 0 — puxada OU freio) = RAMPA_SOBE (suave);
  // SOLTAR/aliviar (voltando pra 0 ou trocando de sinal) = RAMPA_DESCE (rápida)
  bool aplicando = (forcaAlvo * forcaAtual >= 0) && (fabs(forcaAlvo) > fabs(forcaAtual));
  float taxa = aplicando ? RAMPA_SOBE : RAMPA_DESCE;
  float passo = taxa * dt;
  if (fabs(forcaAlvo - forcaAtual) <= passo) forcaAtual = forcaAlvo;
  else forcaAtual += (forcaAlvo > forcaAtual) ? passo : -passo;

  // ── Aplica a força no VESC a ~50Hz ──
  static unsigned long tAplica = 0;
  if (agora - tAplica >= 20) { tAplica = agora; aplicarForca(forcaAtual); }

  // ── TRAVA DE SEGURANÇA: sem comando recente e com força -> zera ──
  if (fabs(forcaAtual) > 0.5f && (agora - ultimoComando > MOTOR_TIMEOUT_MS)) {
    forcaAlvo = 0; forcaAtual = 0; vesc.setCurrent(0.0f);
    Serial.println("[SEGURANCA] sem comando -> motor zerado");
  }

  // ── Lê o tacômetro do VESC a cada ~30ms (backoff se o VESC não responder,
  //    senão cada tentativa bloqueia ~100ms e trava o loop) ──
  static unsigned long intervaloLeitura = 30;
  if (agora - tLeitura >= intervaloLeitura) {
    tLeitura = agora;
    if (vesc.getVescValues()) { tacoBruto = vesc.data.tachometer; contagem = tacoBruto - tacoOffset; intervaloLeitura = 30; }
    else intervaloLeitura = 300;   // sem resposta (VESC desligado/cabo solto): tenta menos vezes pra não travar o loop
  }

  // ── Notifica o app ~6x/s quando mudou ──
  if (agora - tEnvio >= 160) {
    tEnvio = agora;
    if (conectado && contagem != ultimaEnviada) enviarLinha();
  }

  delay(2);
}

/* TODO (após detecção no VESC Tool):
   - Ajustar MAX_PUXADA_A / MAX_FREIO_A pros limites seguros do SEU motor.
   - O "passo" do tacômetro por volta depende dos POLOS do motor (tachometer conta
     em passos de comutação). Calibrar igual ao v1: zera -> recolhe X metros
     conhecidos -> calcula metros/contagem no app (assistente do dashboard).
   - Ajustar RAMPA_SOBE / RAMPA_DESCE pro feeling desejado.
   - RPM mín/máx: calibra pelo APP (dashboard) — o app manda pro ESP no canal CONFIG. RPM máx
     "capa" os metros da corrida; RPM mín garante que o motor gira nos valores baixos.
   - IMPORTANTE p/ ser IMPARÁVEL: no VESC Tool, deixe o LIMITE DE CORRENTE do motor ALTO
     (o setRPM segura a rotação usando essa corrente — é o que faz o motor ganhar do pescador).
   - Se o motor enrolar pro lado ERRADO, troque RPM_SINAL pra -1.
*/
