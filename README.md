# ESP32-S3 Cisco Out-of-Band Web Console

Out-of-Band-Zugriff auf Cisco Switches (Catalyst, Nexus, IE usw.) über ein Web-Terminal im Browser.

**Kein UART, kein MAX3232, keine GPIO-Leitungen** — die serielle Konsole läuft ausschließlich über den **nativen USB-C OTG-Port** des ESP32-S3.

<img width="1244" height="956" alt="Screenshot 2026-06-27 at 14 48 33" src="https://github.com/user-attachments/assets/b18984b9-f86d-40e8-a734-1e18753518d0" />


## Architektur

```
Browser  ←WebSocket/HTTP→  ESP32-S3 (WiFi STA)   ←USB-C OTG (Host)→  Cisco USB Console
                                     │
                            Ethernet (W5500 SPI)  ←RJ45→  LAN-Switch
```

Der ESP32-S3 kann **gleichzeitig** über WiFi und über den W5500-Ethernet-Adapter erreichbar sein. Das Web-Terminal startet, sobald eine der beiden Schnittstellen eine IP-Adresse erhält.

Der ESP32-S3 arbeitet als **USB-Host** (OTG). Der Cisco Switch erscheint als **USB-Gerät** (CDC-ACM). Daten vom Switch werden per WebSocket an den Browser durchgereicht, Tastatureingaben gehen den gleichen Weg zurück.

## W5500 SPI-Ethernet

### Anschluss W5500-Modul ↔ Seeed XIAO ESP32-S3

| W5500-Pin | XIAO-Pin | GPIO  | Funktion   |
|-----------|----------|-------|------------|
| MOSI      | D10      | GPIO9 | SPI-Daten  |
| MISO      | D9       | GPIO8 | SPI-Daten  |
| SCLK      | D8       | GPIO7 | SPI-Takt   |
| CS        | D0       | GPIO1 | Chip-Select |
| INT       | D1       | GPIO2 | Interrupt   |
| RST       | D2       | GPIO3 | Reset (opt.)|
| 3V3       | 3V3      | —     | Versorgung  |
| GND       | GND      | —     | Masse       |

> Pins `CS`, `INT` und `RST` können in `main/eth_w5500.h` über die Defines
> `W5500_CS_GPIO`, `W5500_INT_GPIO` und `W5500_RST_GPIO` angepasst werden.
> `W5500_RST_GPIO -1` bedeutet: RST nicht angeschlossen.

### Empfohlene W5500-Module

Jedes fertige **W5500-SPI-Breakout-Board** funktioniert (z. B. WIZnet W5500-EVB-Pico-HAT-Clone, WaveShare, robuste China-Breakouts).
Achte auf **3,3 V Versorgung** — das XIAO liefert nur 3,3 V am 3V3-Pin.

---

## Hardware & Anschluss

### Ziel-Board: Seeed XIAO ESP32-S3

Dieses Projekt ist für das **Seeed Studio XIAO ESP32-S3** ausgelegt (Chip: ESP32-S3R8, 8 MB Flash, 8 MB PSRAM).

| Dokument | Link |
|----------|------|
| ESP32-S3 Datasheet (Espressif) | [PDF](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32S3/res/esp32-s3_datasheet.pdf) |
| XIAO ESP32-S3 Wiki (Seeed) | [Getting Started](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) |

Das XIAO hat **nur einen USB-C-Anschluss** — kein separater UART-Port wie am DevKitC-1. USB-Daten laufen intern über **GPIO20 (D+)** und **GPIO19 (D−)** des ESP32-S3 (Full-Speed OTG).

### Verkabelung

```
XIAO USB-C  ──OTG-Kabel/Adapter──►  Cisco USB Console (USB-A / Mini-USB)
     │
  WiFi ─┴──► Browser (http://<ip>/)
```

- Das XIAO arbeitet als **USB-Host**, der Cisco Switch als **USB-Gerät** (CDC-ACM, typ. VID `0x05f9` / PID `0x4004`)
- **OTG-Adapter** oder Kabel mit Host-Rolle nötig (USB-C → USB-A)
- **Kein** MAX3232, **kein** UART an GPIO — nur der USB-C

### Stromversorgung & VBUS (wichtig am XIAO)

| Versorgung | Pin `5V` (VBUS) | Cisco am USB-C |
|------------|-----------------|----------------|
| USB-C am PC/Ladegerät | 5 V verfügbar | funktioniert |
| Nur LiPo-Batterie | **kein** 5 V am Pin | **extern 5 V** an Pin `5V` einspeisen |

Bei Batteriebetrieb liefert das XIAO **kein VBUS** an den Cisco-Port. Dann 5 V von außen an den **`5V`-Pin** (VBUS) legen — z. B. über ein Power-Bank-Modul. Siehe [Seeed Forum](https://forum.seeedstudio.com/t/xiao-esp32s3-as-usb-host-while-battery-powered/292637).

### Pinout (relevante Pins)

Draufsicht XIAO ESP32-S3 — nur die für dieses Projekt wichtigen Anschlüsse:

```
                    ┌─────────────────────────┐
                    │      U.FL (Antenne)     │
    USB-C ◄─────────┤  [==== XIAO ESP32-S3 ===│
                    │                         │
         5V (VBUS) ─┤ 5V                  D0  │
              GND ──┤ GND                 D1  │
              3V3 ──┤ 3V3                 ... │
                    │                         │
    UART Debug ─────┤ D6  (TX / GPIO43)  D10 │
                    │ D7  (RX / GPIO44)  D11 │
                    │                         │
    LiPo ───────────┤ BAT+ / BAT-            │
                    │  [Boot]  [Reset]        │
                    └─────────────────────────┘

    Intern (nicht an Header):  GPIO19 = USB D− , GPIO20 = USB D+
```

| XIAO-Pin | GPIO | Funktion in diesem Projekt |
|----------|------|--------------------------|
| **USB-C** | 19, 20 | Cisco-Konsole (OTG Host) |
| **5V** | VBUS | 5-V-Versorgung für Cisco-USB (bei Batteriebetrieb von außen) |
| **GND** | — | Masse |
| **D6** | 43 | UART TX → optional USB-UART-Adapter für `idf.py monitor` |
| **D7** | 44 | UART RX ← optional USB-UART-Adapter |
| **BAT+ / BAT−** | — | 3,7-V-LiPo (optional, für mobilen OOB-Einsatz) |
| **Boot** | 0 | Bootloader (halten beim Reset → manuelles Flashen) |

Vollständiges Pinout: [Seeed Pinout Sheet](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) (Abschnitt Hardware Overview).

### Schaltung: Batteriebetrieb mit Cisco USB

Am XIAO liegt am Pin `5V` bei reiner Batterieversorgung **keine Spannung** an — der Cisco-Console-Port braucht aber **5 V auf VBUS**. Lösung: 5-V-Einspeisung über den `5V`-Pin, während das XIAO selbst von der LiPo läuft.

```mermaid
flowchart TB
    subgraph power [Stromversorgung]
        LiPo["LiPo 3,7 V"]
        Boost["5-V-Boost-Modul\n(z. B. IP5306 / FM5324GA)"]
    end

    subgraph xiao [XIAO ESP32-S3]
        BAT["BAT+/BAT−"]
        PIN5V["Pin 5V (VBUS)"]
        USBC["USB-C OTG"]
        WIFI["WiFi"]
    end

    Cisco["Cisco USB Console"]
    Browser["Browser / Web-Terminal"]

    LiPo --> BAT
    LiPo --> Boost
    Boost -->|"5 V über Schottky-Diode"| PIN5V
    PIN5V --> USBC
    USBC -->|"OTG-Kabel"| Cisco
    WIFI --> Browser
```

**Verdrahtung (Batteriebetrieb):**

```
  LiPo 3,7 V
      │
      ├──► BAT+ / BAT− am XIAO          (XIAO versorgen)
      │
      └──► 5-V-Boost-Modul (Eingang)
                │
                └──► 5 V ──[ Schottky ]──► Pin 5V am XIAO
                                              │
  XIAO USB-C ──── OTG-Kabel ────────────────┴──► Cisco Console
  XIAO GND ─────────────────────────────────────► Cisco GND (über USB-Kabel)
```

**Hinweise zur Schaltung:**

| Punkt | Empfehlung |
|-------|------------|
| Schottky-Diode | Zwischen Boost-Ausgang und Pin `5V`: **Anode** → Boost, **Kathode** → `5V` (laut Seeed-Wiki bei externer Einspeisung) |
| Masse | GND von Boost, XIAO und Cisco gemeinsam |
| USB-C am PC | Beim Batteriebetrieb **nicht** gleichzeitig am PC und Cisco hängen — PHY-Konflikt, siehe oben |
| Strombedarf | Cisco-USB-Console typ. 50–100 mA; Boost-Modul mit ≥ 500 mA wählen |
| Laden | Viele Boost/Lade-Module laden die LiPo über einen eigenen USB-C-Eingang, während der Last-Ausgang das XIAO + VBUS versorgt |

**Einfachster Fall (ohne Batterie):** XIAO per USB-C am Netzteil/PC → Pin `5V` hat VBUS → Cisco direkt am selben USB-C über OTG-Adapter (**nur** wenn das Kabel/Modul Host-Rolle unterstützt; oft besser: XIAO am Netzteil, OTG-Adapter am USB-C).

### USB-PHY: ein Port, zwei Funktionen

Am ESP32-S3 teilen sich **USB Serial/JTAG** und **USB OTG** dieselbe interne PHY. Gleichzeitig Flashen/Monitor **und** Cisco-Host am selben USB-C geht nicht.

| Modus | USB-C | Debug-Ausgabe |
|-------|-------|---------------|
| **Normal** (Cisco OOB) | OTG Host → Cisco | UART über **D6/D7** (GPIO43/44) oder nur Web-UI |
| **Flash-Modus** (Web-Button) | Serial/JTAG → PC | `idf.py flash monitor` wie gewohnt |

**Erstes Flashen:** XIAO per USB-C am PC, `idf.py flash`. Danach normal booten → USB-C für Cisco reservieren.

**Erneut flashen:** Im Web-UI **Flash-Modus** wählen → Reboot → XIAO erscheint am PC als serieller Port → flashen → normal rebooten.

### Andere ESP32-S3-Boards (z. B. DevKitC-1)

Boards mit **zwei** USB-Anschlüssen: Cisco an **USB-OTG**, Flashen/Monitor am **UART/JTAG-Port** — kein Moduswechsel nötig.

## Features

- Web-Terminal mit xterm.js (dunkles Theme)
- WebSocket `/ws` für bidirektionale Konsole
- Break-Signal (250 ms) für ROMMON / Unterbrechung
- Baudrate: 9600 / 19200 / 38400 / 57600 / 115200
- **Settings-Drawer** (⚙-Button): WLAN, Baudrate und WireGuard inline konfigurieren
- Bis zu 4 gleichzeitige WebSocket-Clients
- **Dual-Stack-Netzwerk**: WiFi STA + W5500 SPI-Ethernet gleichzeitig
- Status-Anzeige in der Web-UI: WiFi-IP, ETH-IP und WireGuard-IP
- **WiFi-Fallback-AP**: Kein WLAN erreichbar → automatischer Hotspot-Modus
- **WireGuard VPN**: optionaler VPN-Tunnel zur sicheren Fernwartung

## Projektstruktur

```
esp32s3_serialweb/
├── CMakeLists.txt
├── partitions.csv          ← 2 MB App-Partition (für WireGuard + OTG)
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── main.c              ← HTTP-Server, WiFi, AP-Fallback, Events
    ├── config.c/h          ← NVS-Konfiguration (WiFi + WireGuard)
    ├── eth_w5500.c/h       ← W5500 SPI-Ethernet-Treiber + Pin-Defines
    ├── wg_client.c/h       ← WireGuard-Client (trombik/esp_wireguard)
    └── web_terminal.html   ← Weboberfläche (eingebettet ins Binary)
```

## Build & Flash

```bash
cd esp32s3_serialweb
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Nach dem Flash:

```
http://<ip-des-esp32>/
```

## Erste Konfiguration

1. Beim ersten Start verbindet sich der ESP mit SSID `net1` (Standard-Passwort in `config.c`).
2. IP im Serial Monitor ablesen oder Router-DHCP prüfen.
3. Im Browser `http://<ip>/` öffnen → **⚙** (Settings) anklicken → Tab **Netzwerk & Seriell** → WLAN-Daten eintragen → Speichern.
4. Cisco per **OTG-Kabel** an den USB-C des XIAO anschließen (XIAO = Host).

### WiFi-Fallback: AP-Modus

Wenn das konfigurierte WLAN nach **8 Verbindungsversuchen** nicht erreichbar ist, wechselt der ESP32 automatisch in den Hotspot-Modus:

| Parameter | Wert |
|-----------|------|
| **SSID** | `ESP32S3_AP` |
| **Passwort** | `DefaultPass!` |
| **IP des ESP32** | `192.168.4.1` |

Im Browser `http://192.168.4.1/` öffnen → **⚙** → Tab **Netzwerk & Seriell** → neue WLAN-Daten eintragen → **Speichern**. Der ESP startet automatisch neu und verbindet sich als STA mit dem neuen Netz.

> Im Topbar wird der AP-Modus durch **⚠ AP-Modus** (gelb) angezeigt.

### Flashen am XIAO

```bash
# Erstes Flash / nach Flash-Modus:
idf.py -p /dev/cu.usbmodem* flash monitor

# Im Normalbetrieb (OTG aktiv): Monitor optional über UART an D6/D7:
idf.py -p /dev/cu.usbserial* monitor
```

## API

| Endpoint | Methode | Beschreibung |
|----------|---------|--------------|
| `/` | GET | Web-Terminal (HTML) |
| `/ws` | GET | WebSocket Konsole |
| `/status` | GET | JSON: IP, ETH-IP, WireGuard-IP, Baud, USB, AP-Modus |
| `/break` | POST | Break-Signal senden |
| `/baud` | POST | Baudrate setzen (Body: Zahl als Text) |
| `/config` | POST | WiFi + Baudrate speichern; `{"ok":true}` oder `{"ok":true,"rebooting":true}` im AP-Modus |
| `/config-json` | GET | Aktuelle WiFi/Baud-Konfig als JSON (kein Passwort) |
| `/wg` | POST | WireGuard-Konfig speichern + Tunnel neu starten |
| `/wg-json` | GET | Aktuelle WireGuard-Konfig als JSON (kein Privkey) |
| `/reboot` | POST | ESP neu starten |

### WireGuard VPN

Der ESP32 kann optional einen WireGuard-Tunnel zu einem VPN-Server aufbauen, um auch über das öffentliche Internet sicher erreichbar zu sein.

Konfiguration unter **⚙ → WireGuard VPN**:

| Feld | Beschreibung |
|------|-------------|
| **Aktiviert** | Tunnel ein-/ausschalten |
| **Privater Schlüssel (ESP32)** | Base64-codierter privater Schlüssel des ESP32 |
| **ESP32 Public Key** | Anzeige des öffentlichen Schlüssels (für die Server-Konfiguration) |
| **Server Public Key** | Öffentlicher Schlüssel des WireGuard-Servers |
| **Server Endpoint** | IP oder Hostname des Servers |
| **Port** | UDP-Port (Standard: 51820) |
| **VPN-IP (ESP32)** | Adresse des ESP32 im VPN-Netz (z. B. `10.8.0.3`) |
| **Subnetzmaske** | Maske des VPN-Netzes (z. B. `255.255.255.0`) |
| **Keepalive** | Sekunden zwischen Keep-Alive-Paketen (Standard: 25) |

**Server-seitige Peer-Konfiguration (Beispiel):**

```ini
[Peer]
PublicKey = YEcZ19DyakGAOoBD6u8RRwre8phDfjNt2cbAG84I+xk=
AllowedIPs = 10.8.0.3/32
PersistentKeepalive = 25
```

**Hinweise:**

- Der ESP32 synchronisiert die Systemzeit via **SNTP (NTP)** bevor WireGuard startet. Das ist notwendig, da WireGuard TAI64N-Timestamps in Handshake-Paketen verwendet — ohne korrekte Uhrzeit verwirft der Server die Initiierung (Replay-Schutz).
- Der WireGuard-Status (`VPN-IP ✓` oder `…`) wird im Topbar angezeigt.
- Komponente: [`trombik/esp_wireguard`](https://github.com/trombik/esp_wireguard) (lokal in `components/` für IDF-6.0-Patches).

## Anforderungen

- ESP-IDF 6.0 (getestet mit 6.0.1)
- ESP32-S3 als Target
- Management-Netz ohne Web-Auth (keine Login-Seite — nur im vertrauenswürdigen Netz einsetzen)

## Lizenz

Eigenes Projekt — frei verwendbar.
