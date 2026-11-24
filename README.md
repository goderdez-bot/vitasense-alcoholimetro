# Alcoholímetro experimental – NodeMCU ESP8266 + MQ-3

## Contenido
- `arduino/alcoholimetro_esp8266.ino`: firmware para NodeMCU.
- `public/index.html`: panel web adaptable.
- `netlify/functions/reading.mjs`: API para recibir y consultar lecturas.
- La web incluye un modo de precalentamiento seleccionable de 1 a 48 horas.
- `netlify.toml`, `package.json`: configuración de despliegue.

## Antes de conectar
1. MQ-3: alimentar VCC con 5 V adecuados; GND común.
2. No conectar AO directamente a A0. Instalar divisor: AO -- 150 kΩ -- A0 -- 33 kΩ -- GND.
   El divisor limita aproximadamente a 0,90 V si AO llega a 5 V.
3. OLED I2C: VCC 3V3, GND, SDA D2, SCL D1.
4. Buzzer activo: positivo 3V3, negativo al colector del 2N2222; emisor GND;
   D5 -- 1 kΩ -- base; añadir 10 kΩ entre base y GND.
5. Verificar el pinout E-B-C del transistor exacto. No asumir que todos los 2N2222
   tienen el mismo orden de patas. Si el buzzer es electromagnético, añadir diodo
   de rueda libre (cátodo a positivo).

## Firmware
Instalar en Arduino IDE:
- Placa: NodeMCU 1.0 (ESP-12E Module) / ESP8266
- Librerías: Adafruit GFX Library, Adafruit SSD1306, ArduinoJson (v6 o v7)

Editar en el `.ino`:
- `WIFI_SSID` y `WIFI_PASSWORD`
- `API_BASE_URL`: URL final de Netlify, sin barra final.
- `DEVICE_TOKEN`: debe coincidir con la variable de entorno de Netlify.

Subir el firmware después de desplegar el sitio.

## Despliegue Netlify
1. Crear un repositorio o subir esta carpeta a GitHub.
2. En Netlify: Add new site > Import an existing project y seleccionar el repositorio.
3. Build command: dejar vacío. Publish directory: `public`.
4. En Site configuration > Environment variables, crear:
   `DEVICE_TOKEN` = una clave larga propia (por ejemplo, 32 caracteres aleatorios).
5. Desplegar. La función usa Netlify Blobs para conservar la última lectura.
6. Copiar el dominio HTTPS asignado y pegarlo en `API_BASE_URL` del firmware.
7. Volver a cargar el firmware.

La web consulta `/.netlify/functions/reading` cada 2 segundos. El dispositivo envía
una lectura aproximadamente cada 2 segundos y consulta configuración cada 5 segundos.
El usuario puede iniciar desde la web el ciclo de calentamiento; el NodeMCU recibe la orden,
cuenta el tiempo localmente y publica el tiempo restante.

## Calibración y límites
- La web permite iniciar un ciclo de precalentamiento de 1 a 48 horas.
- El firmware bloquea índice/alarmas como medición válida durante el calentamiento y muestra la cuenta regresiva en OLED.
- Si el NodeMCU pierde alimentación o reinicia, el temporizador vuelve a comenzar desde cero porque el sensor dejó de estar alimentado.
- El periodo de hasta 48 h es de acondicionamiento inicial indicado para el MQ-3; no garantiza por sí solo exactitud ni sustituye una verificación de estabilidad.
- Después de estabilizar, registrar el valor en aire limpio y calibrar con mezclas
  patrón o un instrumento de referencia apropiado.
- En el firmware `CALIBRATION_VALID` queda en `false` por defecto. La web mostrará
  índice relativo, no mg/L, hasta calibrar.
- Para habilitar mg/L, establecer `CALIBRATION_VALID = true` y ajustar
  `CAL_ZERO_ADC` y `CAL_FULL_ADC` con una calibración documentada. La interpolación
  lineal incluida es solo una aproximación experimental entre esos dos puntos;
  no sustituye una curva Rs/R0 ni un alcoholímetro certificado.
- No usar el prototipo para decidir si alguien puede conducir, ni para diagnóstico,
  sanciones o decisiones legales.

## Seguridad
El endpoint de lectura exige `DEVICE_TOKEN`. El endpoint de configuración de la
interfaz es público en esta versión; no expongas información sensible y añade
autenticación si se usa fuera de una demostración controlada. No publiques el token
en la web. La consulta de lecturas es pública.
