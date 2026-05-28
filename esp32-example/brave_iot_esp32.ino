/*
 * ============================================================================
 *  Brave Access IoT  —  Sketch de referência para ESP32 (Phase 2)
 *  Versão: 2.0  ·  Heartbeat + parsing de comandos do servidor
 *
 *  Endpoint: POST https://app.braveaccess.com.br/api/iot/heartbeat
 *  Documentação completa: docs/iot-esp32-protocol.md
 *
 *  Resposta esperada: { "user", "pass", "Host", "Resp" }
 *    Host = IP local em hex 4 bytes (validação anti cross-talk)
 *    Resp = "CCRRTTCC"  (counter eco | rele bitmap | tempo s | cmd)
 *
 *  Pinout sugerido (ajuste conforme seu PCB):
 *    Sensores 1..8  : GPIO 32, 33, 34, 35, 36, 39, 25, 26  (INPUT_PULLUP)
 *    Relês    1..8  : GPIO 13, 12, 14, 27, 16, 17,  4,  5  (OUTPUT)
 *
 *  Bibliotecas necessárias:
 *    - WiFi, HTTPClient, WiFiClientSecure  (built-in ESP32)
 *    - ArduinoJson v6.x  (Benoit Blanchon)
 *    - Preferences  (built-in ESP32, p/ persistir estado dos relés em NVS)
 * ============================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ─────────────────────────────────────────────────────────────────────────────
// CONFIG — ajuste para o seu ambiente
// ─────────────────────────────────────────────────────────────────────────────
const char* WIFI_SSID = "SuaRedeWiFi";
const char* WIFI_PASS = "SuaSenhaWiFi";

const char* BACKEND_URL  = "https://app.braveaccess.com.br/api/iot/heartbeat";
const char* DEVICE_USER  = "admin";
const char* DEVICE_PASS  = "1234";

const uint32_t HEARTBEAT_INTERVAL_MS = 30000;   // 30 s
const uint32_t HTTP_TIMEOUT_MS       = 5000;    // 5 s

const uint8_t SENSOR_PINS[8] = {32, 33, 34, 35, 36, 39, 25, 26};
const uint8_t RELAY_PINS[8]  = {13, 12, 14, 27, 16, 17,  4,  5};

const uint8_t LED_STATUS_PIN = 2;

// ─────────────────────────────────────────────────────────────────────────────
// ESTADO INTERNO
// ─────────────────────────────────────────────────────────────────────────────
uint8_t  g_counter      = 1;
bool     g_bootReset    = true;
uint8_t  g_lastSensors  = 0;
uint8_t  g_lastRelays   = 0;
uint32_t g_lastHbMillis = 0;

// Estrutura para timer de pulso por relé (não-bloqueante)
uint32_t g_pulseEndMs[8] = {0};

// NVS — persiste estado dos relés p/ sobreviver a reboot
Preferences g_prefs;

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS — sensores / relés
// ─────────────────────────────────────────────────────────────────────────────

uint8_t readSensors() {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    if (digitalRead(SENSOR_PINS[i]) == LOW) {  // LOW = fechado (ativo)
      b |= (1 << i);
    }
  }
  return b;
}

uint8_t readRelays() {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    if (digitalRead(RELAY_PINS[i]) == HIGH) b |= (1 << i);
  }
  return b;
}

void setRelay(uint8_t idx, bool on) {
  if (idx >= 8) return;
  digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
}

/**
 * Salva o estado atual dos 8 relés no NVS (sobrevive a reboot).
 * Chamado após comandos 03/04 (retenção ON/OFF).
 */
void persistRelays() {
  g_prefs.putUChar("relays", readRelays());
}

/**
 * Restaura o estado dos relés gravado em NVS no boot.
 */
void restoreRelays() {
  uint8_t saved = g_prefs.getUChar("relays", 0);
  for (uint8_t i = 0; i < 8; i++) {
    setRelay(i, saved & (1 << i));
  }
  Serial.printf("[nvs] estado restaurado: 0x%02X\n", saved);
}

void buildStatusString(char* out, size_t outSize,
                       uint8_t b1, uint8_t b2, uint8_t b3, uint8_t b4) {
  snprintf(out, outSize, "%02X%02X%02X%02X", b1, b2, b3, b4);
}

void incrementCounter() {
  if (g_counter == 0xFF) g_counter = 0x01;
  else                   g_counter++;
}

void blinkLed(uint8_t times, uint16_t delayMs = 100) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(LED_STATUS_PIN, HIGH); delay(delayMs);
    digitalWrite(LED_STATUS_PIN, LOW);  delay(delayMs);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// PARSING DA RESPOSTA (Phase 2)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Converte o IP local em hex 8 chars (ex.: 192.168.0.100 → "C0A80064").
 */
void localIpToHex(char* out, size_t outSize) {
  IPAddress ip = WiFi.localIP();
  snprintf(out, outSize, "%02X%02X%02X%02X", ip[0], ip[1], ip[2], ip[3]);
}

/**
 * Executa o comando vindo no Resp.
 *   reletBitmap → byte 2 (bits dos relés afetados)
 *   tempoSec    → byte 3 (segundos do pulse)
 *   cmd         → byte 4 (00 ACK, 01 reboot, 02 pulse, 03 hold, 04 release)
 */
void executeCommand(uint8_t releBitmap, uint8_t tempoSec, uint8_t cmd) {
  switch (cmd) {
    case 0x00:
      // ACK — nada a fazer
      break;

    case 0x01:
      Serial.println("[cmd] REBOOT recebido — reiniciando em 1s");
      delay(1000);
      ESP.restart();
      break;

    case 0x02: {
      // Pulse: aciona relés, agenda desligamento via millis (não bloqueia)
      Serial.printf("[cmd] PULSE bitmap=0x%02X dur=%us\n", releBitmap, tempoSec);
      uint32_t now = millis();
      for (uint8_t i = 0; i < 8; i++) {
        if (releBitmap & (1 << i)) {
          setRelay(i, true);
          g_pulseEndMs[i] = now + (tempoSec * 1000UL);
        }
      }
      break;
    }

    case 0x03:
      // Retenção ON (NF) — mantém ligados permanentemente, persiste em NVS
      Serial.printf("[cmd] HOLD bitmap=0x%02X\n", releBitmap);
      for (uint8_t i = 0; i < 8; i++) {
        if (releBitmap & (1 << i)) {
          setRelay(i, true);
          g_pulseEndMs[i] = 0;  // cancela qualquer pulse pendente neste relé
        }
      }
      persistRelays();
      break;

    case 0x04:
      // Retenção OFF (NA) — desliga e persiste
      Serial.printf("[cmd] RELEASE bitmap=0x%02X\n", releBitmap);
      for (uint8_t i = 0; i < 8; i++) {
        if (releBitmap & (1 << i)) {
          setRelay(i, false);
          g_pulseEndMs[i] = 0;
        }
      }
      persistRelays();
      break;

    default:
      Serial.printf("[cmd] !!! comando desconhecido 0x%02X\n", cmd);
  }
}

/**
 * Verifica timers de pulse — se algum expirou, desliga o relé.
 * Chamar em todo loop().
 */
void tickPulseTimers() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 8; i++) {
    if (g_pulseEndMs[i] != 0 && (int32_t)(now - g_pulseEndMs[i]) >= 0) {
      setRelay(i, false);
      g_pulseEndMs[i] = 0;
      Serial.printf("[pulse] relé %u desligado (timer)\n", i + 1);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// NETWORK
// ─────────────────────────────────────────────────────────────────────────────

void connectWifi() {
  Serial.print("[wifi] conectando a "); Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 30000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[wifi] IP: ");  Serial.println(WiFi.localIP());
    Serial.print("[wifi] MAC: "); Serial.println(WiFi.macAddress());
  } else {
    Serial.println("\n[wifi] FALHA — reiniciando em 5s");
    delay(5000);
    ESP.restart();
  }
}

bool sendHeartbeat(uint8_t sensors, uint8_t relays) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // 1) Monta o JSON
  uint8_t b1 = g_bootReset ? 0x01 : 0x00;
  uint8_t b2 = g_counter;
  char statusStr[9];
  buildStatusString(statusStr, sizeof(statusStr), b1, b2, sensors, relays);

  StaticJsonDocument<384> doc;
  doc["user"]   = DEVICE_USER;
  doc["pass"]   = DEVICE_PASS;
  doc["mac"]    = WiFi.macAddress();
  doc["IP"]     = WiFi.localIP().toString();
  doc["status"] = statusStr;
  doc["S232"]   = "";
  doc["TTL"]    = "";

  String payload;
  serializeJson(doc, payload);

  // 2) HTTPS POST
  WiFiClientSecure client;
  client.setInsecure();  // TODO: validar cert em produção

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);

  Serial.printf("[hb] POST status=%s counter=%u sensors=0x%02X relays=0x%02X boot=%u\n",
                statusStr, g_counter, sensors, relays, g_bootReset);

  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("[hb] begin() falhou");
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  String body = http.getString();
  http.end();

  Serial.printf("[hb] HTTP %d body=%s\n", code, body.c_str());

  if (code != 200) {
    blinkLed(3, 50);
    return false;
  }

  // 3) Parse da resposta (Phase 2)
  StaticJsonDocument<256> resp;
  if (deserializeJson(resp, body)) {
    Serial.println("[hb] parse JSON falhou");
    return false;
  }

  const char* host = resp["Host"] | "";
  const char* respHex = resp["Resp"] | "";

  // 3a) Valida Host (anti cross-talk)
  char myHost[9];
  localIpToHex(myHost, sizeof(myHost));
  if (strlen(host) != 8 || strcasecmp(host, myHost) != 0) {
    Serial.printf("[hb] WARN Host divergente (esperado %s, recebido %s) — descartando\n", myHost, host);
    return false;
  }

  // 3b) Valida Resp = 8 chars hex
  if (strlen(respHex) != 8) {
    Serial.printf("[hb] WARN Resp inválido: %s\n", respHex);
    return false;
  }

  // 3c) Decodifica os 4 bytes
  uint8_t r[4] = {0, 0, 0, 0};
  for (uint8_t i = 0; i < 4; i++) {
    char chunk[3] = { respHex[i*2], respHex[i*2 + 1], 0 };
    r[i] = (uint8_t)strtoul(chunk, NULL, 16);
  }
  uint8_t respCounter = r[0];
  uint8_t releBitmap  = r[1];
  uint8_t tempoSec    = r[2];
  uint8_t cmd         = r[3];

  // 3d) Valida counter eco
  if (respCounter != g_counter) {
    Serial.printf("[hb] WARN counter divergente (enviei %u, recebi %u) — descartando\n",
                  g_counter, respCounter);
    return false;
  }

  // 3e) Marca power-on como confirmado
  if (g_bootReset) {
    g_bootReset = false;
    Serial.println("[hb] power-on confirmado pelo servidor");
  }

  blinkLed(1, 50);

  // 4) Executa o comando
  if (cmd != 0x00) {
    executeCommand(releBitmap, tempoSec, cmd);
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP / LOOP
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\n========== Brave Access IoT — ESP32 v2.0 ==========");

  pinMode(LED_STATUS_PIN, OUTPUT);
  digitalWrite(LED_STATUS_PIN, LOW);
  for (uint8_t i = 0; i < 8; i++) {
    pinMode(SENSOR_PINS[i], INPUT_PULLUP);
    pinMode(RELAY_PINS[i],  OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }

  // Restaura estado dos relés salvo em NVS (sobrevive a reset)
  g_prefs.begin("brave-iot", false);
  restoreRelays();

  connectWifi();

  // 1º envio imediato (power-on-reset)
  uint8_t s = readSensors();
  uint8_t r = readRelays();
  sendHeartbeat(s, r);
  incrementCounter();
  g_lastSensors  = s;
  g_lastRelays   = r;
  g_lastHbMillis = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] desconectado — reconectando");
    connectWifi();
  }

  tickPulseTimers();  // desliga relés cujo pulse expirou

  uint8_t s = readSensors();
  uint8_t r = readRelays();

  bool changed = (s != g_lastSensors) || (r != g_lastRelays);
  bool timeUp  = (millis() - g_lastHbMillis) >= HEARTBEAT_INTERVAL_MS;

  if (changed || timeUp) {
    if (changed) {
      delay(80);            // debounce
      s = readSensors();
      r = readRelays();
    }
    if (sendHeartbeat(s, r)) {
      incrementCounter();
    }
    g_lastSensors  = s;
    g_lastRelays   = r;
    g_lastHbMillis = millis();
  }

  delay(50);
}
