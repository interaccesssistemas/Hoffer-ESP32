# Brave Access IoT — Protocolo de Comunicação ESP32 ⇄ Cloud

> **Versão:** 1.0 (Phase 1 — somente envio de status)
> **Endpoint produção:** `https://app.braveaccess.com.br/api/iot/heartbeat`

## 1. Visão geral

A placa **Brave-ESP32** envia periodicamente um JSON via HTTPS POST para o servidor da nuvem informando:

- Identificação (MAC + IP local)
- Estado dos 8 sensores de entrada
- Estado dos 8 relés de saída
- Contador rotativo (para o servidor confirmar que recebeu)
- Flag de "power-on-reset" (primeiro envio após boot)

O servidor responde com um JSON simples confirmando o recebimento e ecoando o contador. **Phase 1 não envia comandos ainda** — comandos virão na Phase 2.

---

## 2. Endpoint

```
POST  /api/iot/heartbeat
Content-Type: application/json
Accept: application/json
```

> **Importante:** todas as rotas backend são prefixadas com `/api`. Não envie para `/iot/heartbeat` direto — vai cair no frontend React e retornar HTML.

---

## 3. Payload do request (ESP32 → Cloud)

```json
{
  "user": "admin",
  "pass": "1234",
  "mac":  "AA:BB:CC:DD:EE:FF",
  "IP":   "192.168.0.100",
  "status": "010A050E",
  "S232": "",
  "TTL":  ""
}
```

| Campo    | Tipo   | Obrigatório | Descrição                                                                  |
|----------|--------|-------------|----------------------------------------------------------------------------|
| `user`   | string | Sim         | Usuário fixo da placa (configurável no servidor — default `admin`)         |
| `pass`   | string | Sim         | Senha fixa da placa (default `1234`)                                       |
| `mac`    | string | Sim         | MAC da ESP32 no formato `XX:XX:XX:XX:XX:XX` (uppercase). Identifica o cliente dono no Mongo |
| `IP`     | string | Sim         | IP local que a placa recebeu via DHCP                                      |
| `status` | string | Sim         | **String hex de 8 caracteres** com 4 bytes (ver Seção 4)                   |
| `S232`   | string | Não         | Reservado para uso futuro — envie `""`                                     |
| `TTL`    | string | Não         | Reservado para uso futuro — envie `""`                                     |

---

## 4. Campo `status` — 4 bytes hex (8 caracteres)

```
"status": "BBCCSSRR"
            │ │ │ └─ Byte 4: estado dos 8 relés (bit 0 = relé 1)
            │ │ └─── Byte 3: estado dos 8 sensores (bit 0 = sensor 1)
            │ └───── Byte 2: contador rotativo (01..FF, voltando a 01 ao chegar em FF)
            └─────── Byte 1: power-on-reset (01 = primeiro envio após boot, 00 nos demais)
```

### Exemplos práticos

| `status`     | Significado                                                                         |
|--------------|-------------------------------------------------------------------------------------|
| `"01000000"` | **Boot** — placa acabou de ligar, contador 0, nenhum sensor/relé ativo              |
| `"000A0000"` | Keep-alive 10º envio (0x0A), nenhum sensor/relé ativo                               |
| `"000A050E"` | Keep-alive 10º envio · sensores **1 e 3 FECHADOS (ativados)** · relés **2, 3 e 4 ligados** |
| `"00FF0001"` | Último envio antes de zerar o contador · relé **1** ligado                          |
| `"0001FF00"` | Recém pós-boot (contador 1) · **TODOS** os 8 sensores fechados · todos relés OFF    |

### Conversão bit → sensor/relé

```
byte = 0x05 = 0000 0101
              │ │ │ │ │ │ │ └─ bit 0 → sensor 1: ATIVADO (FECHADO)
              │ │ │ │ │ │ └─── bit 1 → sensor 2: standby (ABERTO)
              │ │ │ │ │ └───── bit 2 → sensor 3: ATIVADO (FECHADO)
              ...
```

> **Importante — semântica do contato seco:**
> - **Sensor bit=1 (ATIVADO)** = contato **FECHADO** (passou corrente — botoeira pressionada, sensor magnético acoplado, fim-de-curso encostado, etc.)
> - **Sensor bit=0 (standby)** = contato **ABERTO** = estado de repouso, sem corrente. **Este é o estado normal** de quase todos os sensores.
> - **Relé bit=1** = relé LIGADO (bobina energizada, contato comutou).
> - **Relé bit=0** = relé DESLIGADO (estado de repouso).

---

## 5. Cadência do envio

| Situação              | Quando enviar                                                                |
|-----------------------|------------------------------------------------------------------------------|
| **Power-on**          | Logo após conectar ao Wi-Fi (envia com `byte1=01`)                           |
| **Keep-alive**        | A cada **30 segundos** (envia com `byte1=00`)                                |
| **Mudança de sensor** | Envio imediato (não esperar 30 s) — quando algum bit de `sensor` mudar      |
| **Mudança de relé**   | Envio imediato — quando o firmware acionar um relé localmente                |

**Anti-spam:** se a placa enviar mais de 5 heartbeats por segundo, o servidor não retorna 4xx (sempre devolve 200) mas a coleção `iot_heartbeats` cresce mais rápido. Use `30 s` como base com debounce de 100 ms para mudanças.

---

## 6. Resposta do servidor (Cloud → ESP32)

**Sempre HTTP 200** — mesmo em caso de erro, para não disparar retry da placa.

```json
{
  "user": "admin",
  "pass": "1234",
  "Host": "C0A80064",
  "Resp": "0A400302"
}
```

| Campo  | Tipo   | Descrição                                                                                            |
|--------|--------|------------------------------------------------------------------------------------------------------|
| `user` | string | Eco do `user` configurado no servidor                                                                |
| `pass` | string | Eco do `pass` configurado no servidor                                                                |
| `Host` | string | **IP do ESP32 (campo `IP` do request) em hex 4 bytes** (8 chars uppercase). Defende contra entrega cruzada — a placa deve verificar antes de aplicar o comando |
| `Resp` | string | **4 bytes hex (8 chars uppercase)** com o comando — ver Seção 6.1                                    |

### 6.1 — Campo `Resp` (4 bytes hex)

```
"Resp": "CCRRTTCC"
          │ │ │ └─ Byte 4: comando (00..FF) — ver tabela 6.2
          │ │ └─── Byte 3: tempo em segundos (00..FF) · usado pelo comando 02 (pulse)
          │ └───── Byte 2: bitmap dos relés (bit 0 = relé 1, bit 1 = relé 2, etc.)
          └─────── Byte 1: contador eco — repete o byte 2 do `status` recebido
                            (a placa usa para confirmar que a resposta é desta requisição)
```

### 6.2 — Tabela de comandos (Byte 4)

| Byte 4 | Comando         | Ação no ESP32                                                                              |
|--------|-----------------|--------------------------------------------------------------------------------------------|
| `00`   | OK (ACK)        | Nada a fazer · servidor está vivo e recebeu o heartbeat                                    |
| `01`   | Reboot          | Reiniciar a placa (equivalente a `ESP.restart()`)                                          |
| `02`   | Pulso           | Acionar relés indicados no Byte 2 pelo tempo do Byte 3 (segundos), depois soltar           |
| `03`   | Reter (NF)      | Manter relés indicados no Byte 2 **acionados** permanentemente (NF = normally fechado)     |
| `04`   | Soltar (NA)     | **Desligar** relés indicados no Byte 2 (NA = normally aberto = repouso)                    |

> **Persistência (comandos 03 e 04):** ao receber retenção ON ou OFF, a placa deve **gravar em NVS/EEPROM** o novo estado dos relés. Após reboot espontâneo (queda de luz, watchdog), restaurar o estado salvo no setup. Sem isso, perde-se o estado entre boots.

### 6.3 — Codificação do `Host` (IP → hex)

```
192.168.0.100  →  192=0xC0  168=0xA8  0=0x00  100=0x64  →  "C0A80064"
192.168.0.150  →  192=0xC0  168=0xA8  0=0x00  150=0x96  →  "C0A80096"
10.0.0.5       →  10 =0x0A  0  =0x00  0=0x00  5  =0x05  →  "0A000005"
```

Pseudocódigo no ESP32:
```c
char host[9];
IPAddress ip = WiFi.localIP();
snprintf(host, sizeof(host), "%02X%02X%02X%02X", ip[0], ip[1], ip[2], ip[3]);
```

### 6.4 — Exemplos de respostas

| Cenário                                                        | `Resp`        | Decodificação                                              |
|----------------------------------------------------------------|---------------|------------------------------------------------------------|
| ACK (nada a fazer)                                             | `0A000000`    | counter=0A, OK                                             |
| Pulso relé 1 por 4s                                            | `0A010402`    | counter=0A, relé 1 (0x01), 4s, cmd=pulse                   |
| Pulso relé 7 por 3s                                            | `0A400302`    | counter=0A, relé 7 (0x40 = 0b01000000), 3s, cmd=pulse      |
| Pulso relé 4 por 60s                                           | `0A083C02`    | counter=0A, relé 4 (0x08), 60s (0x3C), cmd=pulse           |
| Reter relé 4 ON (NF)                                           | `0A080003`    | counter=0A, relé 4, sem tempo, cmd=hold                    |
| Soltar relé 4 OFF (NA)                                         | `0A080004`    | counter=0A, relé 4, sem tempo, cmd=release                 |
| Reboot                                                         | `0A000001`    | counter=0A, sem relé/tempo, cmd=reboot                     |
| Pulso múltiplos relés 1+2+3 (0b00000111=0x07) por 2s           | `0A070202`    | counter=0A, relés 1+2+3 simultâneos, 2s                    |

### 6.5 — Como a placa deve agir conforme o byte 1 (counter eco)

| Situação                                          | Ação no ESP32                                                                    |
|---------------------------------------------------|----------------------------------------------------------------------------------|
| `Resp[byte1]` == counter do último status enviado | Resposta é desta requisição → aplicar o comando                                  |
| `Resp[byte1]` != counter                          | Possível resposta cacheada / cruzada → **ignorar** e aguardar próximo heartbeat |
| `Host` != IP local da placa                       | Resposta é para outra placa → **ignorar**                                        |

---

## 7. Como testar o endpoint via cURL

Use esses exemplos para validar a conexão **antes mesmo de gravar o firmware** — abra um terminal (Linux/macOS/WSL) ou Git Bash no Windows.

> ⚠ **Atenção CMD/Windows:** os exemplos 7.1–7.9 usam sintaxe Bash (continuação com `\` e aspas simples `'...'`). No **CMD do Windows** isso **não funciona** — pule direto para a seção 7.10 (CMD com aspas escapadas), 7.11 (PowerShell) ou 7.12 (JSON em arquivo).

### 7.1 — Boot OK (primeiro envio após ligar)

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{
    "user": "admin",
    "pass": "1234",
    "mac":  "AA:BB:CC:DD:EE:FF",
    "IP":   "192.168.0.100",
    "status": "01010000",
    "S232": "",
    "TTL":  ""
  }'
```

**Resposta esperada (fila vazia):**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"01000000"}
```
- `Host=C0A80064` confirma que 192.168.0.100 = `0xC0 0xA8 0x00 0x64`.
- `Resp=01000000` = counter eco 0x01, sem relé, sem tempo, cmd=00 (ACK).

### 7.2 — Keep-alive com sensores e relés ativos (sem comandos pendentes)

10º envio (`byte2=0A`), sensores 1 e 3 **fechados (ativados)** (`byte3=05`), relés 2, 3 e 4 ligados (`byte4=0E`):

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000A050E","S232":"","TTL":""}'
```

**Resposta esperada:**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"0A000000"}
```
- `Resp=0A000000` = counter eco 0x0A, ACK (nada a fazer).

### 7.3 — Auth fail (senha errada)

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"errada","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"00010000","S232":"","TTL":""}'
```

**Resposta esperada:**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"01000000","_result":"auth_fail"}
```

> Note que o servidor **ainda retorna HTTP 200** — somente `_result` muda (campo de diagnóstico). A placa pode usar `_result` para acender LED de erro, mas não é obrigatório.

### 7.4 — MAC não cadastrado no Cloud

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"FF:FF:FF:FF:FF:FF","IP":"1.2.3.4","status":"01010000","S232":"","TTL":""}'
```

**Resposta esperada:**
```json
{"user":"admin","pass":"1234","Host":"01020304","Resp":"01000000","_result":"mac_not_found"}
```

### 7.5 — Enfileirar comando e ver a entrega no próximo heartbeat

Esse fluxo é o coração da Phase 2 — administrador enfileira via API (ou Dashboard `/iot`), a placa recebe no próximo POST.

**Passo 1 — Login admin para obter token:**

```bash
TOKEN=$(curl -s -X POST https://app.braveaccess.com.br/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"email":"emerson@interacess.com.br","senha":"SUA_SENHA"}' | jq -r .token)
```

**Passo 2 — Enfileirar pulso no relé 7 por 3 segundos:**

```bash
curl -X POST "https://app.braveaccess.com.br/api/iot/devices/AA:BB:CC:DD:EE:FF/enqueue" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"cmd":"pulse","rele":7,"duracao":3}'
```

Resposta:
```json
{"enqueued":true,"command":{"id":"...","mac":"AA:BB:CC:DD:EE:FF","rele_byte":64,"tempo_byte":3,"cmd_byte":2,"descricao":"Pulso relé 7 por 3s","status":"pending",...}}
```

**Passo 3 — Próximo heartbeat da placa recebe o comando:**

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000B0000"}'
```

Resposta:
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"0B400302"}
```
- `Resp=0B400302` → counter=0B (eco), relé 7 (0x40), 3s, cmd=02 (pulse) ✓

### 7.6 — Outros comandos via enqueue

| Comando                | cURL `-d`                                        | Resp esperada (counter genérico XX) |
|------------------------|--------------------------------------------------|-------------------------------------|
| Pulso relé 1 por 4s    | `{"cmd":"pulse","rele":1,"duracao":4}`           | `XX010402`                          |
| Pulso relé 4 por 60s   | `{"cmd":"pulse","rele":4,"duracao":60}`          | `XX083C02`                          |
| Retenção ON relé 4     | `{"cmd":"hold","rele":4}`                        | `XX080003`                          |
| Retenção OFF relé 4    | `{"cmd":"release","rele":4}`                     | `XX080004`                          |
| Reboot da placa        | `{"cmd":"reboot"}`                               | `XX000001`                          |

Endpoint para todos: `POST /api/iot/devices/{mac}/enqueue` (header `Authorization: Bearer $TOKEN`).

### 7.7 — Modo Alarme (sirene em loop)

Quando ligado, o backend **automaticamente enfileira um pulso a cada heartbeat** (relé + duração configurados). Útil para simular sirene tocando.

```bash
# Liga modo alarme: relé 1, pulso 2s a cada heartbeat
curl -X POST "https://app.braveaccess.com.br/api/iot/devices/AA:BB:CC:DD:EE:FF/alarm-mode" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"enabled":true,"rele":1,"duracao":2}'

# Próximo heartbeat:
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"00100000"}'
# Resp: 10010202  (counter=10, relé 1, 2s, pulse)

# Desliga modo alarme
curl -X POST "https://app.braveaccess.com.br/api/iot/devices/AA:BB:CC:DD:EE:FF/alarm-mode" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"enabled":false}'
```

### 7.8 — Verbose: ver headers HTTP e timing

Útil para diagnosticar lentidão de rede ou cert SSL:

```bash
curl -v -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"01010000","S232":"","TTL":""}' \
  -w "\n\n--- timing ---\ndns:     %{time_namelookup}s\nconnect: %{time_connect}s\ntls:     %{time_appconnect}s\ntotal:   %{time_total}s\n"
```

### 7.9 — Sequência completa simulando boot + 3 keep-alives (Bash loop)

> Esse script é Bash (Linux/macOS/WSL/Git Bash). No CMD do Windows não funciona — use PowerShell (7.8) ou WSL.

```bash
URL="https://app.braveaccess.com.br/api/iot/heartbeat"
MAC="AA:BB:CC:DD:EE:FF"
IP_LOCAL="192.168.0.100"

# Boot
curl -s -X POST $URL -H "Content-Type: application/json" \
  -d "{\"user\":\"admin\",\"pass\":\"1234\",\"mac\":\"$MAC\",\"IP\":\"$IP_LOCAL\",\"status\":\"01010000\",\"S232\":\"\",\"TTL\":\"\"}"
echo

# 3 keep-alives com sensor 1 alternando aberto/fechado
for c in 02 03 04; do
  SENSORS=$([ "$c" = "03" ] && echo "01" || echo "00")
  STATUS="00${c}${SENSORS}00"
  curl -s -X POST $URL -H "Content-Type: application/json" \
    -d "{\"user\":\"admin\",\"pass\":\"1234\",\"mac\":\"$MAC\",\"IP\":\"$IP_LOCAL\",\"status\":\"$STATUS\",\"S232\":\"\",\"TTL\":\"\"}"
  echo
  sleep 2
done
```

Depois confira no Dashboard https://app.braveaccess.com.br/iot — deve aparecer 4 linhas no memo "Protocolos Recebidos" e a placa marcada como **ATIVA**.

### 7.10 — CMD do Windows (sem WSL)

⚠ No CMD do Windows o `\` no fim da linha **NÃO é continuação** (é literal) e aspas simples `'...'` não funcionam. Use aspas duplas com escape `\"` e `^` para quebra de linha.

**Forma simples (tudo numa linha):**

```cmd
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat -H "Content-Type: application/json" -d "{\"user\":\"admin\",\"pass\":\"1234\",\"mac\":\"AA:BB:CC:DD:EE:FF\",\"IP\":\"192.168.0.100\",\"status\":\"000A050E\",\"S232\":\"\",\"TTL\":\"\"}"
```

**Com quebra de linha usando `^`:**

```cmd
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat ^
  -H "Content-Type: application/json" ^
  -d "{\"user\":\"admin\",\"pass\":\"1234\",\"mac\":\"AA:BB:CC:DD:EE:FF\",\"IP\":\"192.168.0.100\",\"status\":\"000A050E\",\"S232\":\"\",\"TTL\":\"\"}"
```

**Resposta esperada:**
```
{"ack":true,"result":"ok","ack_counter":10}
```

### 7.11 — PowerShell (Windows sem WSL — recomendado)

```powershell
$body = @{
  user   = "admin"
  pass   = "1234"
  mac    = "AA:BB:CC:DD:EE:FF"
  IP     = "192.168.0.100"
  status = "01010000"
  S232   = ""
  TTL    = ""
} | ConvertTo-Json

Invoke-RestMethod -Method POST `
  -Uri "https://app.braveaccess.com.br/api/iot/heartbeat" `
  -ContentType "application/json" `
  -Body $body
```

Ou linha única:

```powershell
Invoke-RestMethod -Method POST -Uri "https://app.braveaccess.com.br/api/iot/heartbeat" -ContentType "application/json" -Body '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"01010000","S232":"","TTL":""}'
```

### 7.12 — Dica: salvar o JSON em arquivo (funciona em qualquer SO)

Se a sintaxe de escape estiver te enlouquecendo, salve o JSON num arquivo e passe com `-d @arquivo.json`:

**1. Crie `heartbeat.json`:**
```json
{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"01010000","S232":"","TTL":""}
```

**2. Envie:**
```cmd
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat -H "Content-Type: application/json" -d @heartbeat.json
```

---

## 8. Checklist para o programador do ESP32

### Conexão e envio
- [ ] Conectar ao Wi-Fi via `WiFi.begin(ssid, password)`
- [ ] Aguardar `WL_CONNECTED` antes do primeiro POST
- [ ] Ler MAC com `WiFi.macAddress()` (formato `XX:XX:XX:XX:XX:XX`)
- [ ] Ler IP local com `WiFi.localIP().toString()`
- [ ] Manter contador local (`uint8_t`) iniciando em `1` e incrementando — quando passar de 255, voltar a 1
- [ ] Manter flag `bootReset = true` no setup, e setar `false` **APÓS o primeiro POST OK**
- [ ] Montar `status` com `snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", b1, b2, b3, b4)` — sempre uppercase, sempre 8 chars
- [ ] HTTPS: use `WiFiClientSecure` com `client.setInsecure()` para começar (em produção, valide o certificado)
- [ ] Timeout do `http.POST()` de 5 segundos
- [ ] Em caso de falha de rede, **NÃO** retentar imediatamente — espere o próximo ciclo de 30 s
- [ ] Watchdog do ESP32 ativo (`esp_task_wdt_init`) para auto-reset em caso de hang

### Parsing da resposta (Phase 2)
- [ ] Deserializar JSON com `ArduinoJson` — extrair `Host` e `Resp`
- [ ] **Validar `Host`** — converter IP local em hex (`snprintf "%02X%02X%02X%02X"`) e comparar. Se diferente → **descartar a resposta** (cross-talk de cache/proxy)
- [ ] **Validar `Resp[byte1]`** — comparar com o contador enviado. Se diferente → descartar (resposta de heartbeat anterior)
- [ ] Decodificar `Resp` em 4 bytes: `byte2`=bitmap relés, `byte3`=segundos, `byte4`=comando
- [ ] Switch por `byte4`:
  - `0x00` → ACK, nada a fazer
  - `0x01` → `ESP.restart()`
  - `0x02` → para cada bit ativo em `byte2`, acionar o relé, `delay(byte3 * 1000)`, soltar (usar millis para não bloquear)
  - `0x03` → manter os relés do `byte2` ON · **gravar estado em NVS** (`Preferences.putUChar`)
  - `0x04` → desligar os relés do `byte2` · **atualizar NVS**

### Persistência (NVS / EEPROM)
- [ ] No `setup()`, abrir Preferences com namespace `"brave-iot"`
- [ ] Ler `relays_persistent` (uint8_t) salvo e aplicar nos pinos antes de subir
- [ ] Sempre que comando 03/04 chegar, atualizar a variável e gravar com `prefs.putUChar("relays_persistent", state)`
- [ ] Garantir que após reboot espontâneo (queda de luz), a placa volta ao estado salvo

---

## 9. Roadmap (Phase 3+)

**Phase 2 — Comandos via fila (IMPLEMENTADA)**: o servidor mantém uma fila `iot_commands` por MAC. Cada heartbeat consome 1 comando (FIFO). Pulse, Hold (NF), Release (NA), Reboot, Modo Alarme (sirene em loop) — todos via Dashboard `/iot` ou API `POST /api/iot/devices/{mac}/enqueue`.

**Phase 3 — Schedules + Alexa**: abertura automática por horário configurada no Dashboard ("relé 1 abre 07:00, fecha 22:00 seg-sex") e skill Alexa que reusa essa mesma API.

---

## 10. Suporte

Dúvidas ou bugs:
- **E-mail:** emerson@interacess.com.br
- **Dashboard de monitoramento:** https://app.braveaccess.com.br/iot
