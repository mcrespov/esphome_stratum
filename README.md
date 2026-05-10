# ESPHome NTP Server con GPS

Servidor NTP autónomo basado en **ESP32-C6** y **ESPHome**, que sirve la hora obtenida de un módulo GPS por UART a cualquier cliente de la red local. Se integra además con Home Assistant para monitorización (señal WiFi, temperatura interna, satélites detectados, fix GPS y desviación entre el reloj GPS y el de Home Assistant).

El servidor responde como **stratum 1** (reloj de referencia primario `GPS`) en el puerto UDP/123 estándar, por lo que cualquier dispositivo de la red puede usarlo como fuente de hora sin configuración adicional más allá de apuntar a su IP.

## Características
 
- Servidor NTP funcional en UDP/123, respondiendo paquetes NTPv4 conformes al RFC 5905.
- Hora obtenida directamente de satélites GPS vía UART (NMEA a 9600 bps por defecto).
- Stratum 1 con `ref_id = "GPS"`.
- Integración completa con Home Assistant vía API nativa de ESPHome.
- Sensores expuestos a HA:
  - Satélites conectados
  - Fix GPS (binary sensor, true con ≥4 satélites)
  - Temperatura interna del SoC
  - Señal WiFi
  - Desviación entre la hora GPS y la hora de Home Assistant
- Logging cada 5 s con la hora reportada por ambas fuentes (GPS y HA) y la deriva entre ellas.
- Descarte automático de peticiones mientras la hora del sistema no sea válida (antes de fix GPS), para no servir tiempos basura.
- Hotspot fallback para reconfiguración si pierde la WiFi.
- OTA habilitado para actualizaciones inalámbricas.
- Botón reinicio

## Hardware necesario

| Componente | Notas |
|---|---|
| ESP32-C6 DevKitC-1 (o compatible) | El YAML usa la variante `esp32c6` y framework `esp-idf` |
| Módulo GPS con salida NMEA por UART | Probado a 9600 bps; cualquiera con TX serie sirve (NEO-6M, NEO-7M, ATGM336H, etc.) |
| Antena GPS | Imprescindible para obtener fix, especialmente en interior |

### Conexionado GPS

| GPS | ESP32-C6 |
|---|---|
| TX  | GPIO 6 (RX del UART) |
| RX  | GPIO 7 (TX del UART, opcional si no se reconfigura el GPS) |
| VCC | 3.3 V o 5 V * |
| GND | GND |

El PIN del voltaje se utilizará el que necesite el módulo.
Los pines UART se pueden cambiar en la sección `uart:` del YAML.

## Estructura del repositorio

```
.
├── ntpserver.yaml          # Configuración principal de ESPHome
├── ntp_server.h            # Implementación C++ del servidor NTP (UDP/123)
├── secrets_example.yaml    # Plantilla de secretos, para tener en cuenta en secrets.yaml
├── .gitignore
└── README.md
```

## Configuración

### 1. Clonar el repositorio

### 2. Crear el archivo de secretos

Copia la plantilla y rellena con tus datos reales:

```bash
cp secrets_example.yaml secrets.yaml
```

Edita `secrets.yaml` con tus credenciales:

```yaml
wifi_ssid: "tu_ssid"
wifi_password: "tu_password"
fallback_ap_password: "password_del_hotspot_fallback"
api_encryption_key_ntpserver: "clave_base64_de_32_bytes"
ota_password_ntpserver: "password_ota"
```

> [!IMPORTANT]
> El archivo `secrets.yaml` está incluido en `.gitignore` y **nunca** debe subirse al repositorio. Si necesitas generar una clave de encriptación válida para la API, puedes usar:
> ```bash
> openssl rand -base64 32
> ```

### 3. Compilar y flashear

Con ESPHome instalado (ya sea como add-on de Home Assistant, contenedor Docker o `pip install esphome`):

```bash
esphome run ntpserver.yaml
```

La primera vez será necesario flashear por USB. Las siguientes actualizaciones pueden hacerse por OTA.

## Uso

Una vez flasheado y conectado a la red, el dispositivo:

1. Se registra automáticamente en Home Assistant vía descubrimiento de la API.
2. Espera a obtener fix GPS (binary sensor `GPS con Señal (Fix)` se pondrá en `on` con ≥4 satélites).
3. Comienza a responder peticiones NTP en UDP/123.

### Verificar el servidor NTP

Desde Linux o macOS:

```bash
ntpdate -q IP_ESP32
# o con chrony:
chronyd -Q "server IP_ESP32 iburst"
```

Desde Windows (PowerShell como administrador):

```powershell
w32tm /stripchart /computer:IP_ESP32 /samples:5
```

Si el dispositivo responde, verás la diferencia de tiempo entre el reloj local y el del servidor NTP.

## Detalles técnicos

### Cómo funciona el servidor NTP

El archivo `ntp_server.h` implementa un servidor NTP minimalista en C++ que se compila como parte del firmware:

- **`ntp_server_setup()`** se invoca una vez en el `on_boot` del YAML. Crea un socket UDP en modo no bloqueante (`O_NONBLOCK`) y lo bindea al puerto 123 en todas las interfaces.
- **`ntp_server_loop()`** se llama desde un `interval: 20ms` de ESPHome. Cada llamada intenta leer un paquete con `recvfrom` no bloqueante; si no hay nada, retorna inmediatamente. Si llega una petición de un cliente NTP, construye y envía la respuesta marcando timestamps `recv` y `tx` con `gettimeofday()`.
- La hora del sistema (`gettimeofday`) es sincronizada por el componente `time: gps` de ESPHome automáticamente cuando el GPS reporta una hora válida, así que no hace falta acceder al GPS desde el código del servidor NTP.

### Limitaciones de precisión

La precisión efectiva está limitada por:

1. **El intervalo de polling de 20 ms** del bucle principal de ESPHome. Esto añade un jitter de hasta ±10 ms en el `recv_timestamp`.
2. **La latencia de NMEA por UART a 9600 bps**, que es de ~100 ms entre el flanco del segundo y la trama parseada. Sin pin PPS conectado, esta es la principal fuente de deriva absoluta.
3. **No hay control por PPS**: la implementación actual no usa el pin `PPS` del GPS para refinar la marca del segundo. Para precisión sub-milisegundo habría que añadir esa lógica.

Para uso doméstico la precisión es suficiente, equivalente a un servidor NTP público vía internet sin la dependencia de conexión externa.

## Personalización

### Cambiar los pines UART del GPS

Edita la sección `uart:` en `ntpserver.yaml`:

```yaml
uart:
  - id: gps_uart
    rx_pin: 6     # Cambia aquí
    tx_pin: 7     # Cambia aquí
    baud_rate: 9600
```

### Cambiar el intervalo de polling NTP

Edita el `interval` en `ntpserver.yaml`. Valores más bajos reducen el jitter pero aumentan la carga de CPU:

```yaml
interval:
  - interval: 20ms     # Reducir a 10ms para menos jitter
    then:
      - lambda: ntp_server_loop();
```

### Subir el nivel de log para depurar

En `ntpserver.yaml`, cambia:

```yaml
logger:
  level: DEBUG
```

Con `DEBUG` verás un mensaje cada vez que el servidor responda a una petición NTP, incluyendo IP y puerto del cliente.

## Licencia

MIT.

## Créditos

- [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus) (usado internamente por el componente `gps` de ESPHome).
- RFC 5905 (Network Time Protocol Version 4) como referencia del protocolo.
