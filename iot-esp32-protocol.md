# Brave Access IoT — Protocolo de Comunicação ESP32 ⇄ Cloud

> **Versão:** 2.0 (Rev-2 — leitores RFID Wiegand TTL/S232 + comando de configuração inicial)
> **Endpoint produção:** `https://app.braveaccess.com.br/api/iot/heartbeat`

---

## 1. Visão geral

A placa **Brave-ESP32 Rev-2** envia periodicamente um JSON via HTTPS POST para o servidor da nuvem informando:

- Identificação (MAC + IP local)
- Estado dos 8 sensores de entrada
- Estado dos 8 relés de saída
- Contador rotativo (para o servidor confirmar que recebeu)
- Flag de "power-on-reset" (primeiro envio após boot)
- **Leituras dos 2 leitores RFID Wiegand já decodificadas em W26 pelo firmware:**
  - `TTL` → Leitor 1 (porta TTL do ESP32) → atraca **Relé 1**
  - `S232` → Leitor 2 (porta RS232 do ESP32) → atraca **Relé 2**

O servidor responde com um JSON confirmando o recebimento, ecoando o contador, e podendo entregar **um comando** da fila FIFO da placa (ver Seção 6). Comandos cobrem: ACK, Reboot, Pulse, Hold (NF), Release (NA), e **Enviar Configuração Inicial (0x05)** que devolve IP/máscara/gateway/URL/polling/tempos de pulso.

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
  "TTL":  "00112233",
  "S232": ""
}
```

| Campo    | Tipo   | Obrigatório | Descrição                                                                  |
|----------|--------|-------------|----------------------------------------------------------------------------|
| `user`   | string | Sim         | Usuário fixo da placa (configurável no servidor — default `admin`)         |
| `pass`   | string | Sim         | Senha fixa da placa (default `1234`)                                       |
| `mac`    | string | Sim         | MAC da ESP32 no formato `XX:XX:XX:XX:XX:XX` (uppercase). Identifica o cliente dono no Mongo |
| `IP`     | string | Sim         | IP local que a placa recebeu via DHCP (ou config 0x05)                     |
| `status` | string | Sim         | **String hex de 8 caracteres** com 4 bytes (ver Seção 4)                   |
| `TTL`    | string | Sim (vazio ok) | **Rev-2 · Leitor 1** — cartão RFID W26 (8 dígitos `LLLFFFFF` decodificado pelo firmware). Vazio `""` quando não há leitura nova. Match no Relé 1. |
| `S232`   | string | Sim (vazio ok) | **Rev-2 · Leitor 2** — cartão RFID W26 (8 dígitos `LLLFFFFF`). Vazio `""` quando não há leitura nova. Match no Relé 2. |

### 3.1 — Semântica dos campos RFID (Rev-2)

- O firmware da Rev-2 **decodifica o Wiegand bruto em W26 ANTES de enviar** — a nuvem recebe os 8 dígitos finais (`LLLFFFFF` = Lote 3 dígitos + Facility Code 5 dígitos).
- A nuvem usa esses 8 dígitos **como chave de busca** em `cliente_moradores` (campos minúsculos `cartao_w36`, `codigo_rfid`, `pulseira`, `qrcode`, `codigo_integracao`).
- **Enviar somente quando há leitura nova** — repetir o mesmo valor não causa dano (a nuvem faz dedup por leitor), mas economiza banda enviando `""` quando o leitor está parado.
- Os 2 campos podem chegar simultâneos num mesmo heartbeat (se ambos leitores leem cartões diferentes entre dois envios).

---

## 4. Campo `status` — 4 bytes hex (8 caracteres)

```
"status": "BBCCSSRR"
            │ │ │ └─ Byte 4: estado dos 8 relés (bit 0 = relé 1)
            │ │ └─── Byte 3: estado dos 8 sensores (bit 0 = sensor 1)
            │ └───── Byte 2: contador rotativo (01..FF, voltando a 01 ao chegar em FF)
            └─────── Byte 1: power-on-reset (01 = primeiro envio após boot, 00 nos demais)
```

### 4.1 — Comportamento da nuvem em `power_on=True` (Byte 1 = 0x01)

Quando o byte 1 é `0x01`, a nuvem **reseta no MongoDB** o estado lógico desta placa:
- `iot_devices.status_decoded.sensors_bits` → `0` (todos abertos)
- `iot_devices.status_decoded.relays_bits` → `0` (todos desligados)
- `iot_devices.last_l1_value` / `last_l2_value` → `""` (limpa dedup dos leitores)
- Incrementa `power_on_count` e grava `ultimo_power_on`

Isso evita estado "fantasma" no dashboard logo após boot — antes do firmware reportar de fato o estado físico atual via heartbeat seguinte.

### 4.2 — Exemplos práticos

| `status`     | Significado                                                                         |
|--------------|-------------------------------------------------------------------------------------|
| `"01000000"` | **Boot** — placa acabou de ligar, contador 0, nenhum sensor/relé ativo              |
| `"000A0000"` | Keep-alive 10º envio (0x0A), nenhum sensor/relé ativo                               |
| `"000A050E"` | Keep-alive 10º envio · sensores **1 e 3 FECHADOS (ativados)** · relés **2, 3 e 4 ligados** |
| `"00FF0001"` | Último envio antes de zerar o contador · relé **1** ligado                          |
| `"0001FF00"` | Recém pós-boot (contador 1) · **TODOS** os 8 sensores fechados · todos relés OFF    |

### 4.3 — Conversão bit → sensor/relé

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

## 5. Leitores RFID Wiegand — `TTL` e `S232` (Rev-2)

### 5.1 — Decodificação no firmware

A placa Rev-2 tem **2 leitores Wiegand**:
- **Leitor 1**: ligado na porta **TTL** do ESP32 → reportado como campo `"TTL"`.
- **Leitor 2**: ligado na porta **RS232** do ESP32 → reportado como campo `"S232"`.

Quando um cartão é apresentado, o firmware:
1. Captura os pulsos Wiegand.
2. **Decodifica para W26** (8 dígitos `LLLFFFFF`).
3. Envia o valor decodificado no próximo heartbeat (campo `TTL` ou `S232`).
4. Mantém o valor por **1 heartbeat** e limpa (`""`) — não fica repetindo até nova leitura.

### 5.2 — Modos de operação (ONLINE vs OFFLINE)

O modo é configurado **por dispositivo** no campo `SeFaceFuncOnLine` do `cliente_coletores` (toggle "Configurado On-Line" na tela `/cliente/coletores`, aba Geral).

| Modo                | `SeFaceFuncOnLine` | Comportamento ao receber cartão                                                                                |
|---------------------|--------------------|----------------------------------------------------------------------------------------------------------------|
| **OFFLINE (default)** | `false`            | Nuvem **enfileira pulse imediato** no relé correspondente (1 ou 2) com `TempoS1`/`TempoS2` do coletor. **Não valida** o cartão. |
| **ONLINE**            | `true`             | Nuvem busca o cartão em `cliente_moradores`. **Se encontrado** → enfileira pulse. **Se não encontrado** → registra `denied_not_registered` no histórico, mas **NÃO atraca o relé**. Dashboard exibe "Tag RFID não cadastrada". |

### 5.3 — Match no banco (campos minúsculos · CASE-SENSITIVE)

A busca é feita em `cliente_moradores` com:

```javascript
{ cliente_id: "<id do cliente dono do MAC>", $or: [
    { cartao_w36: "<valor>" },     // string
    { cartao_w36: <valor int> },   // integer (alguns cadastros legados)
    { codigo_rfid: "<valor>" },
    { codigo_rfid: <valor int> },
    { pulseira: "<valor>" },
    { qrcode: "<valor>" },
    { codigo_integracao: "<valor>" },
] }
```

> ⚠ **Atenção:** MongoDB é **case-sensitive**. Os campos no banco estão em minúsculas; usar `Cartao_W36` ou `CodigoRfid` falha o match.

### 5.4 — Dedup por leitor

A nuvem armazena `last_l1_value` e `last_l2_value` no `iot_devices`. Se o cartão recebido for **idêntico** ao último processado naquele leitor, a nuvem **ignora** (não enfileira pulso novo). Isso impede flood de pulsos caso o firmware envie a mesma leitura em múltiplos heartbeats consecutivos.

> 💡 O `power_on=True` (Seção 4.1) limpa esses dois campos — após boot, a primeira leitura sempre é processada.

### 5.5 — Mapeamento leitor → relé

| Leitor     | Campo JSON | Relé acionado | Tempo de pulso       |
|------------|------------|---------------|----------------------|
| L1 · TTL   | `"TTL"`    | **Relé 1**    | `TempoS1` (ms → s)   |
| L2 · S232  | `"S232"`   | **Relé 2**    | `TempoS2` (ms → s)   |

Os tempos de pulso individuais (`TempoS1..TempoS8` em ms, 1000..255000) são configurados em `/cliente/coletores` aba **Saídas**, e enviados à placa via comando `0x05` (Seção 6.2).

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

> **Quando o comando entregue é `0x05` (Config Inicial)**, a resposta inclui também os campos `ESP32_IP`, `ESP32_MASCARA`, `ESP32_GATEWAY`, `ESP32_URL`, `ESP32_POLLING_MS`, `ESP32_TEMPO_R1..R8`. Ver Seção 6.6.

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
| `05`   | **Config Inicial (Rev-2)** | **Reconfigurar IP/máscara/gateway/URL/polling/tempos de pulso.** Ler os campos `ESP32_*` extras na resposta (Seção 6.6), **gravar em NVS** e aplicar (reconfigura rede + atualiza intervalo de polling + atualiza durações dos pulsos por relé). Byte 2 e Byte 3 são ignorados. |

> **Persistência (comandos 03, 04 e 05):** ao receber retenção ON/OFF ou Config Inicial, a placa deve **gravar em NVS/EEPROM** o novo estado. Após reboot espontâneo (queda de luz, watchdog), restaurar do NVS no setup.

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
| Pulso relé 1 por 4s (cartão W26 lido em TTL OFFLINE)           | `0A010402`    | counter=0A, relé 1 (0x01), 4s, cmd=pulse                   |
| Pulso relé 2 por 6s (cartão W26 lido em S232 OFFLINE)          | `0A020602`    | counter=0A, relé 2 (0x02), 6s, cmd=pulse                   |
| Pulso relé 7 por 3s                                            | `0A400302`    | counter=0A, relé 7 (0x40 = 0b01000000), 3s, cmd=pulse      |
| Pulso relé 4 por 60s                                           | `0A083C02`    | counter=0A, relé 4 (0x08), 60s (0x3C), cmd=pulse           |
| Reter relé 4 ON (NF)                                           | `0A080003`    | counter=0A, relé 4, sem tempo, cmd=hold                    |
| Soltar relé 4 OFF (NA)                                         | `0A080004`    | counter=0A, relé 4, sem tempo, cmd=release                 |
| Reboot                                                         | `0A000001`    | counter=0A, sem relé/tempo, cmd=reboot                     |
| Pulso múltiplos relés 1+2+3 (0b00000111=0x07) por 2s           | `0A070202`    | counter=0A, relés 1+2+3 simultâneos, 2s                    |
| **Rev-2 Config Inicial**                                       | `0A000005`    | counter=0A, byte2/3 ignorados, cmd=config. Resposta JSON contém `ESP32_*` (ver 6.6) |

### 6.5 — Como a placa deve agir conforme o byte 1 (counter eco)

| Situação                                          | Ação no ESP32                                                                    |
|---------------------------------------------------|----------------------------------------------------------------------------------|
| `Resp[byte1]` == counter do último status enviado | Resposta é desta requisição → aplicar o comando                                  |
| `Resp[byte1]` != counter                          | Possível resposta cacheada / cruzada → **ignorar** e aguardar próximo heartbeat |
| `Host` != IP local da placa                       | Resposta é para outra placa → **ignorar**                                        |

### 6.6 — Campos extras na resposta do comando `0x05` (Config Inicial · Rev-2)

Quando `Resp` tem o byte 4 igual a `05`, a nuvem inclui os seguintes campos no JSON da resposta:

```json
{
  "user": "admin",
  "pass": "1234",
  "Host": "C0A80064",
  "Resp": "0A000005",
  "ESP32_IP":         "192.168.0.50",
  "ESP32_MASCARA":    "255.255.255.0",
  "ESP32_GATEWAY":    "192.168.0.1",
  "ESP32_URL":        "https://app.braveaccess.com.br/api/iot/heartbeat",
  "ESP32_POLLING_MS": 15000,
  "ESP32_TEMPO_R1":   4000,
  "ESP32_TEMPO_R2":   6000,
  "ESP32_TEMPO_R3":   4000,
  "ESP32_TEMPO_R4":   4000,
  "ESP32_TEMPO_R5":   4000,
  "ESP32_TEMPO_R6":   4000,
  "ESP32_TEMPO_R7":   4000,
  "ESP32_TEMPO_R8":   8000
}
```

| Campo               | Origem no Mongo                                                         | Significado |
|---------------------|-------------------------------------------------------------------------|-------------|
| `ESP32_IP`          | `cliente_coletores.IPColetor`                                           | IP estático que a placa deve assumir |
| `ESP32_MASCARA`     | `cliente_coletores.MascaraIP`                                           | Máscara de rede da placa |
| `ESP32_GATEWAY`     | `cliente_coletores.Gateway`                                             | Gateway padrão da placa |
| `ESP32_URL`         | `cliente_servidor.placa_esp32_url`                                      | URL completa do endpoint de heartbeat |
| `ESP32_POLLING_MS`  | `cliente_servidor.placa_esp32_polling_ms` (default 30000, mín 500)      | Intervalo entre heartbeats em ms |
| `ESP32_TEMPO_R1..R8`| `cliente_coletores.TempoS1..TempoS8` (1000..255000 ms, default 4000)    | Duração do pulso de cada relé em ms |

**Ação esperada no firmware ao receber `0x05`:**
1. Validar `Host` e contador eco (Seção 6.5).
2. Ler os 12+ campos `ESP32_*` do JSON da resposta.
3. **Gravar todos em NVS** (`Preferences.putString` para os strings, `Preferences.putUInt` para os ms).
4. Aplicar `ESP32_TEMPO_R1..R8` na lógica de pulso dos relés (usar como duração para o cmd `0x02`).
5. Aplicar `ESP32_POLLING_MS` no loop principal (`vTaskDelay(pdMS_TO_TICKS(ESP32_POLLING_MS))`).
6. Reconfigurar Wi-Fi/STA com `WiFi.config(IP, GW, MASK)` (ou agendar reconfig no próximo boot).
7. Continuar enviando heartbeats normalmente — **não precisa reiniciar**.

> 💡 **Quando enviar o comando 0x05?** O administrador dispara via Dashboard `/iot` → botão **"Enviar Configuração Inicial"** no card da placa, ou via API: `POST /api/iot/devices/{mac}/enviar-config-inicial` (Seção 7.13). A nuvem enfileira o cmd e o entrega no próximo heartbeat da placa.

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
    "TTL":  "",
    "S232": ""
  }'
```

**Resposta esperada (fila vazia):**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"01000000","_result":"ok"}
```
- `Host=C0A80064` confirma que 192.168.0.100 = `0xC0 0xA8 0x00 0x64`.
- `Resp=01000000` = counter eco 0x01, sem relé, sem tempo, cmd=00 (ACK).
- Nuvem reseta sensors/relays no dashboard (Seção 4.1).

### 7.2 — Keep-alive com sensores e relés ativos (sem cartão, sem comandos)

10º envio (`byte2=0A`), sensores 1 e 3 **fechados (ativados)** (`byte3=05`), relés 2, 3 e 4 ligados (`byte4=0E`):

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000A050E","TTL":"","S232":""}'
```

### 7.3 — **Rev-2** · Cartão W26 lido no Leitor 1 (TTL) · OFFLINE

Modo OFFLINE (`SeFaceFuncOnLine=false` no coletor): a nuvem atraca direto o relé 1.

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000B0000","TTL":"00112233","S232":""}'
```

**Resposta esperada (com `TempoS1=4000` no coletor):**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"0B010402","_result":"ok"}
```
- `Resp=0B010402` → counter=0B, relé 1 (0x01), 4s, cmd=02 (pulse).

### 7.4 — **Rev-2** · Cartão W26 no Leitor 2 (S232) · ONLINE com match

Modo ONLINE (`SeFaceFuncOnLine=true`): a nuvem busca o cartão `99887766` em `cliente_moradores` (campos minúsculos). Se achar, atraca o relé 2.

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000C0000","TTL":"","S232":"99887766"}'
```

**Resposta (com `TempoS2=6000`):**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"0C020602","_result":"ok"}
```
- `Resp=0C020602` → counter=0C, relé 2 (0x02), 6s, cmd=02 (pulse). Match registrado em `iot_heartbeats.rfid_events[].action=pulse_online_match`.

### 7.5 — **Rev-2** · Cartão ONLINE sem cadastro → NÃO atraca

Mesmo cenário do 7.4 mas com cartão não cadastrado:

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000D0000","TTL":"FFFFFFFF","S232":""}'
```

**Resposta (nenhum relé acionado):**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"0D000000","_result":"ok"}
```
- `Resp=0D000000` → counter=0D, sem relé, ACK. Mas em `iot_heartbeats.rfid_events[].action="denied_not_registered"` fica registrado, e o Dashboard mostra **"Tag RFID não cadastrada"** em vermelho no card do Leitor 1.

### 7.6 — Auth fail (senha errada)

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"errada","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"00010000","TTL":"","S232":""}'
```

**Resposta:**
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"01000000","_result":"auth_fail"}
```

> Servidor **ainda retorna HTTP 200**. A placa pode usar `_result` para LED de erro.

### 7.7 — MAC não cadastrado no Cloud

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"FF:FF:FF:FF:FF:FF","IP":"1.2.3.4","status":"01010000","TTL":"","S232":""}'
```

**Resposta:**
```json
{"user":"admin","pass":"1234","Host":"01020304","Resp":"01000000","_result":"mac_not_found"}
```

### 7.8 — Enfileirar comando e ver a entrega no próximo heartbeat

Fluxo Phase 2 — administrador enfileira via API (ou Dashboard `/iot`), a placa recebe no próximo POST.

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

**Passo 3 — Próximo heartbeat recebe o comando:**

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000B0000","TTL":"","S232":""}'
```

Resposta:
```json
{"user":"admin","pass":"1234","Host":"C0A80064","Resp":"0B400302","_result":"ok"}
```
- `Resp=0B400302` → counter=0B (eco), relé 7 (0x40), 3s, cmd=02 (pulse) ✓

### 7.9 — Outros comandos via enqueue

| Comando                | cURL `-d`                                        | Resp esperada (counter genérico XX) |
|------------------------|--------------------------------------------------|-------------------------------------|
| Pulso relé 1 por 4s    | `{"cmd":"pulse","rele":1,"duracao":4}`           | `XX010402`                          |
| Pulso relé 4 por 60s   | `{"cmd":"pulse","rele":4,"duracao":60}`          | `XX083C02`                          |
| Retenção ON relé 4     | `{"cmd":"hold","rele":4}`                        | `XX080003`                          |
| Retenção OFF relé 4    | `{"cmd":"release","rele":4}`                     | `XX080004`                          |
| Reboot da placa        | `{"cmd":"reboot"}`                               | `XX000001`                          |

Endpoint: `POST /api/iot/devices/{mac}/enqueue` (header `Authorization: Bearer $TOKEN`).

### 7.10 — Modo Alarme (sirene em loop)

Quando ligado, o backend **automaticamente enfileira um pulso a cada heartbeat** (relé + duração configurados).

```bash
# Liga modo alarme: relé 1, pulso 2s a cada heartbeat
curl -X POST "https://app.braveaccess.com.br/api/iot/devices/AA:BB:CC:DD:EE:FF/alarm-mode" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"enabled":true,"rele":1,"duracao":2}'

# Desliga
curl -X POST "https://app.braveaccess.com.br/api/iot/devices/AA:BB:CC:DD:EE:FF/alarm-mode" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"enabled":false}'
```

### 7.11 — CMD do Windows (sem WSL)

⚠ No CMD do Windows o `\` no fim da linha **NÃO é continuação** (é literal) e aspas simples `'...'` não funcionam. Use aspas duplas com escape `\"` e `^` para quebra de linha.

```cmd
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat -H "Content-Type: application/json" -d "{\"user\":\"admin\",\"pass\":\"1234\",\"mac\":\"AA:BB:CC:DD:EE:FF\",\"IP\":\"192.168.0.100\",\"status\":\"000A050E\",\"TTL\":\"\",\"S232\":\"\"}"
```

### 7.12 — PowerShell (Windows sem WSL — recomendado)

```powershell
$body = @{
  user   = "admin"
  pass   = "1234"
  mac    = "AA:BB:CC:DD:EE:FF"
  IP     = "192.168.0.100"
  status = "01010000"
  TTL    = ""
  S232   = ""
} | ConvertTo-Json

Invoke-RestMethod -Method POST `
  -Uri "https://app.braveaccess.com.br/api/iot/heartbeat" `
  -ContentType "application/json" `
  -Body $body
```

### 7.13 — **Rev-2** · Enviar Configuração Inicial (comando 0x05)

Dispara o cmd `0x05`. No próximo heartbeat, a resposta da nuvem traz todos os campos `ESP32_*` (Seção 6.6) para a placa reconfigurar IP/máscara/gateway/URL/polling/tempos de relé.

**Passo 1 — Enfileirar o cmd 0x05:**

```bash
curl -X POST "https://app.braveaccess.com.br/api/iot/devices/AA:BB:CC:DD:EE:FF/enviar-config-inicial" \
  -H "Authorization: Bearer $TOKEN"
```

Resposta:
```json
{"enqueued":true,"command":{"id":"...","mac":"AA:BB:CC:DD:EE:FF","rele_byte":0,"tempo_byte":0,"cmd_byte":5,"descricao":"Enviar configuração inicial (IP/Máscara/Gateway/URL/Polling/Tempos R1..R8)","status":"pending",...}}
```

**Passo 2 — Próximo heartbeat da placa recebe o cmd + payload:**

```bash
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat \
  -H "Content-Type: application/json" \
  -d '{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"000C0000","TTL":"","S232":""}'
```

Resposta (exemplo):
```json
{
  "user": "admin", "pass": "1234",
  "Host": "C0A80064",
  "Resp": "0C000005",
  "_result": "ok",
  "ESP32_IP":         "192.168.0.50",
  "ESP32_MASCARA":    "255.255.255.0",
  "ESP32_GATEWAY":    "192.168.0.1",
  "ESP32_URL":        "https://app.braveaccess.com.br/api/iot/heartbeat",
  "ESP32_POLLING_MS": 15000,
  "ESP32_TEMPO_R1":   4000, "ESP32_TEMPO_R2": 6000,
  "ESP32_TEMPO_R3":   4000, "ESP32_TEMPO_R4": 4000,
  "ESP32_TEMPO_R5":   4000, "ESP32_TEMPO_R6": 4000,
  "ESP32_TEMPO_R7":   4000, "ESP32_TEMPO_R8": 8000
}
```

A placa lê os `ESP32_*`, grava em NVS e aplica (ver Seção 6.6).

### 7.14 — Dica: salvar o JSON em arquivo

Se a sintaxe de escape estiver te enlouquecendo, salve o JSON num arquivo e passe com `-d @arquivo.json`:

**1. Crie `heartbeat.json`:**
```json
{"user":"admin","pass":"1234","mac":"AA:BB:CC:DD:EE:FF","IP":"192.168.0.100","status":"01010000","TTL":"","S232":""}
```

**2. Envie:**
```cmd
curl -X POST https://app.braveaccess.com.br/api/iot/heartbeat -H "Content-Type: application/json" -d @heartbeat.json
```

---

## 8. Checklist para o programador do ESP32

### Conexão e envio
- [ ] Conectar ao Wi-Fi via `WiFi.begin(ssid, password)` (ou `WiFi.config(IP, GW, MASK)` se já houver config salva do cmd 0x05)
- [ ] Aguardar `WL_CONNECTED` antes do primeiro POST
- [ ] Ler MAC com `WiFi.macAddress()` (formato `XX:XX:XX:XX:XX:XX`)
- [ ] Ler IP local com `WiFi.localIP().toString()`
- [ ] Manter contador local (`uint8_t`) iniciando em `1` e incrementando — quando passar de 255, voltar a 1
- [ ] Manter flag `bootReset = true` no setup, setar `false` **APÓS o primeiro POST OK**
- [ ] Montar `status` com `snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", b1, b2, b3, b4)` — sempre uppercase, 8 chars
- [ ] HTTPS: use `WiFiClientSecure` com `client.setInsecure()` para começar (em produção, valide o certificado)
- [ ] Timeout do `http.POST()` de 5 segundos
- [ ] Em caso de falha de rede, **NÃO** retentar imediatamente — espere o próximo ciclo (default 30s ou `ESP32_POLLING_MS` se salvo)
- [ ] Watchdog do ESP32 ativo (`esp_task_wdt_init`) para auto-reset em caso de hang

### Leitores RFID Wiegand (Rev-2)
- [ ] Configurar interrupts dos pinos Wiegand para Leitor 1 (TTL) e Leitor 2 (RS232)
- [ ] Decodificar pulsos Wiegand para W26 (8 dígitos `LLLFFFFF`)
- [ ] Buffer 1 cartão por leitor — quando ler, marcar "pendente envio"
- [ ] No próximo heartbeat:
  - `TTL` = cartão pendente do Leitor 1 (string 8 dígitos) ou `""` se nada novo
  - `S232` = cartão pendente do Leitor 2 ou `""`
- [ ] **Após enviar** com sucesso (HTTP 200), limpar o buffer do leitor — não reenviar o mesmo cartão
- [ ] Se a leitura ocorreu **entre dois heartbeats**, enviar imediatamente (não esperar os 30s)

### Parsing da resposta
- [ ] Deserializar JSON com `ArduinoJson` — extrair `Host`, `Resp` e (se cmd=05) os `ESP32_*`
- [ ] **Validar `Host`** — converter IP local em hex e comparar. Se diferente → **descartar** (cross-talk de cache/proxy)
- [ ] **Validar `Resp[byte1]`** — comparar com o contador enviado. Se diferente → descartar
- [ ] Decodificar `Resp` em 4 bytes: `byte2`=bitmap relés, `byte3`=segundos, `byte4`=comando
- [ ] Switch por `byte4`:
  - `0x00` → ACK, nada a fazer
  - `0x01` → `ESP.restart()`
  - `0x02` → para cada bit ativo em `byte2`, acionar o relé, aguardar `byte3` segundos, soltar (usar `millis()` para não bloquear)
  - `0x03` → manter os relés do `byte2` ON · **gravar estado em NVS** (`Preferences.putUChar`)
  - `0x04` → desligar os relés do `byte2` · **atualizar NVS**
  - `0x05` (**Rev-2**) → ler `ESP32_IP`/`MASCARA`/`GATEWAY`/`URL`/`POLLING_MS`/`TEMPO_R1..R8` do JSON, gravar em NVS e aplicar (ver Seção 6.6)

### Persistência (NVS / EEPROM)
- [ ] No `setup()`, abrir Preferences com namespace `"brave-iot"`
- [ ] Ler `relays_persistent` (uint8_t) salvo e aplicar nos pinos antes de subir
- [ ] Ler config de rede (`cfg_ip`, `cfg_mask`, `cfg_gw`, `cfg_url`, `cfg_polling`, `cfg_tempo_r1..r8`) e usar — senão usa defaults compilados
- [ ] Sempre que comando 03/04 chegar, atualizar a variável e gravar com `prefs.putUChar("relays_persistent", state)`
- [ ] Sempre que comando 05 chegar, gravar **todos** os 12+ campos em NVS antes de aplicar
- [ ] Garantir que após reboot espontâneo (queda de luz), a placa volta ao estado salvo (relés + config de rede)

---

## 9. Roadmap

**Phase 1 — Heartbeat (IMPLEMENTADA)**: envio periódico de status + identificação por MAC.

**Phase 2 — Comandos via fila FIFO (IMPLEMENTADA)**: pulse, hold (NF), release (NA), reboot, Modo Alarme — via Dashboard `/iot` ou `POST /api/iot/devices/{mac}/enqueue`.

**Rev-2 — Leitores RFID + Config Inicial (IMPLEMENTADA · Fev/2026)**:
- 2 leitores Wiegand TTL/S232 decodificados em W26 pelo firmware.
- Modo ONLINE (`SeFaceFuncOnLine=true`) valida no `cliente_moradores`; OFFLINE atraca direto.
- Dedup por leitor (`last_l1_value` / `last_l2_value`).
- Reset de estado em `power_on=True`.
- Comando `0x05` (Config Inicial) entrega IP/máscara/gateway/URL/polling/tempos via JSON.
- Endpoint admin: `POST /api/iot/devices/{mac}/enviar-config-inicial`.

**Phase 3 — Schedules + Alexa (PLANEJADA)**: abertura automática por horário ("relé 1 abre 07:00, fecha 22:00 seg-sex") e skill Alexa reusando a mesma API.

---

## 10. Suporte

Dúvidas ou bugs:
- **E-mail:** emerson@interacess.com.br
- **Dashboard de monitoramento:** https://app.braveaccess.com.br/iot
