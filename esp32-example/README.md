# Sketch ESP32 — Brave Access IoT

Sketch Arduino de referência para o programador do firmware do Brave-ESP32.

## Como usar

1. Abra `brave_iot_esp32.ino` na **Arduino IDE 2.x**.
2. Instale a biblioteca `ArduinoJson` (Library Manager · Benoit Blanchon · v6.x).
3. Selecione a Board: `ESP32 Dev Module` (ou seu modelo).
4. Ajuste no topo do arquivo:
   - `WIFI_SSID` / `WIFI_PASS` — sua rede local.
   - `BACKEND_URL` — produção (`https://app.braveaccess.com.br/api/iot/heartbeat`) ou preview.
   - `DEVICE_USER` / `DEVICE_PASS` — credenciais combinadas com o servidor.
   - `SENSOR_PINS[]` / `RELAY_PINS[]` — pinos do seu PCB.
5. Upload e abra o **Serial Monitor a 115200 baud**.

## Comportamento esperado no log

```
========== Brave Access IoT — ESP32 ==========
[wifi] conectando a SuaRedeWiFi...
[wifi] IP: 192.168.0.100
[wifi] MAC: AA:BB:CC:DD:EE:FF
[hb] POST status=01010000 counter=1 sensors=0x0 relays=0x0 boot=1
[hb] HTTP 200 body={"ack":true,"result":"ok","ack_counter":1}
[hb] result=ok ack_counter=1
[hb] power-on confirmado pelo servidor
```

A partir daí, um POST a cada 30 s (ou imediato se algum sensor mudar).

## LED de status

| Padrão                | Significado                              |
|-----------------------|------------------------------------------|
| 1 piscada curta (50ms)| Heartbeat OK                             |
| 3 piscadas curtas     | Erro de rede / HTTP ≠ 200                |
| 2 piscadas longas     | MAC não cadastrado no Cloud              |
| 5 piscadas médias     | AUTH FAIL — user/pass errado             |

## Próximas fases

Quando a Phase 2 (comandos) for liberada pelo Cloud, este sketch precisará:
1. Ler `cmds[]` da resposta JSON.
2. Executar cada comando (pulse, on, off, reboot).
3. Enviar `acked_ids: [1, 2, ...]` no próximo heartbeat.

## Documentação completa do protocolo

Veja [`../iot-esp32-protocol.md`](../iot-esp32-protocol.md).
