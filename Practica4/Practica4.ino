#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>
#include "DHT.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ---------------------------------------------------------------------------
// 1) CREDENCIALES DE RED Y WEBSOCKET
// ---------------------------------------------------------------------------


const char* ssid     = "Nombre_de_Red";
const char* password = "Contraseña_de_Red";

WebSocketsServer webSocket = WebSocketsServer(81);

// ---------------------------------------------------------------------------
// 2) MOTORES (TB6612FNG)
// ---------------------------------------------------------------------------
#define IN1 11
#define IN2 12
#define IN3 13
#define IN4 14
#define PWM_A 15
#define PWM_B 16

#define PWM_FREQ 5000
#define PWM_RES  8

uint8_t velAvanzar = 170;
uint8_t velGiro    = 170;

struct ComandoMover {
  char accion[16];
  uint32_t ms;
};

QueueHandle_t colaMover;
TaskHandle_t hTareaMover = NULL;
char accionActual[16] = "parar";

// ---------------------------------------------------------------------------
// 3) ASIGNACIÓN DE PINES Y VARIABLES DE CONTROL
// ---------------------------------------------------------------------------
const int PIN_SERVO    = 8;   
const int TRIG_PIN     = 9;   
const int ECHO_PIN     = 10;  
const int BUZZER_PIN   = 18;  
const int NEOPIXEL_PIN = 4;    // Matriz WS2812B 
#define NUM_LEDS 64

// Sensor de Temperatura DHT22
#define DHTPIN 7              // Sensor DHT22 
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

Adafruit_NeoPixel matriz(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

Servo servo;
int anguloActual = 90;
const int pulsoMinimo = 500;
const int pulsoMaximo = 2400;

float distanciaActual = 100.0;
unsigned long ultimoMuestreoUS = 0;
unsigned long ultimoAnimacionNeo = 0;
uint8_t pasoAnimacion = 0;

enum ModoMatriz { MODO_NORMAL, MODO_RADAR, MODO_AGUJA };
ModoMatriz modoMatrizActual = MODO_NORMAL;
unsigned long tiempoFinModoEspecial = 0;

const float DISTANCIA_SEGURIDAD_CM = 25.0; 
bool alarmaHabilitada = true; 
bool matrizHabilitada = true;  // Control de la casilla "Matriz" en la app Qt

// ---------------------------------------------------------------------------
// MAPAS DE BIT 8x8 EN PROGMEM
// ---------------------------------------------------------------------------
const uint8_t PROGMEM flechaAbajo_1[8] = {
  B00000000, B00011000, B00111100, B01111110, B00011000, B00011000, B00011000, B00000000
};
const uint8_t PROGMEM flechaAbajo_2[8] = {
  B00011000, B00111100, B01111110, B00011000, B00011000, B00011000, B00000000, B00000000
};

const uint8_t PROGMEM flechaArriba_1[8] = {
  B00000000, B00011000, B00011000, B00011000, B01111110, B00111100, B00011000, B00000000
};
const uint8_t PROGMEM flechaArriba_2[8] = {
  B00000000, B00000000, B00011000, B00011000, B00011000, B01111110, B00111100, B00011000
};

const uint8_t PROGMEM flechaDerecha[8] = {
  B00001000, B00011000, B00111000, B01111111, B01111111, B00111000, B00011000, B00001000
};

const uint8_t PROGMEM flechaIzquierda[8] = {
  B00010000, B00011000, B00011100, B11111110, B11111110, B00011100, B00011000, B00010000
};

const uint8_t PROGMEM puntoCentral[8] = {
  B00000000, B00000000, B00000000, B00011000, B00011000, B00000000, B00000000, B00000000
};

const uint8_t PROGMEM ecualizador_1[8] = {
  B00000000, B00000000, B01000010, B01000010, B01011010, B01011010, B01111110, B01111110
};
const uint8_t PROGMEM ecualizador_2[8] = {
  B01000010, B01011010, B01011010, B01111110, B01111110, B01011010, B01000010, B00000000
};

const uint8_t PROGMEM agujaIzquierda[8] = {
  B10000000, B01000000, B00100000, B00010000, B00001000, B00000100, B00000010, B00000001
};
const uint8_t PROGMEM agujaCentro[8] = {
  B00011000, B00011000, B00011000, B00011000, B00011000, B00011000, B00011000, B00011000
};
const uint8_t PROGMEM agujaDerecha[8] = {
  B00000001, B00000010, B00000100, B00001000, B00010000, B00100000, B01000000, B10000000
};

// ---------------------------------------------------------------------------
// FUNCIONES DE MATRIZ NEOPIXEL 
// ---------------------------------------------------------------------------
void renderizarIcono(const uint8_t icono[8], uint32_t color) {
  matriz.clear();
  for (int y = 0; y < 8; y++) {
    uint8_t fila = pgm_read_byte(&(icono[y]));
    for (int x = 0; x < 8; x++) {
      if (fila & (1 << (7 - x))) {
        int nuevoX = y;
        int nuevoY = 7 - x;
        int index = nuevoY * 8 + nuevoX;
        matriz.setPixelColor(index, color);
      }
    }
  }
  matriz.show();
}

void renderizarRadar(int angulo) {
  matriz.clear();
  int columnaOriginal = map(constrain(angulo, 0, 180), 0, 180, 0, 7);
  uint32_t verdeRadar = matriz.Color(0, 255, 50);

  for (int y = 0; y < 8; y++) {
    int nuevoX = y;
    int nuevoY = 7 - columnaOriginal;
    int index = nuevoY * 8 + nuevoX;
    matriz.setPixelColor(index, verdeRadar);
  }
  matriz.show();
}

void renderizarAgujaServo(int angulo) {
  uint32_t cCyan = matriz.Color(0, 200, 255);
  if (angulo < 60) {
    renderizarIcono(agujaIzquierda, cCyan);
  } else if (angulo > 120) {
    renderizarIcono(agujaDerecha, cCyan);
  } else {
    renderizarIcono(agujaCentro, cCyan);
  }
}

void actualizarMatrizNeoPixel() {
  // Si la matriz está desactivada desde la app Qt, apagar LEDs y no renderizar nada
  if (!matrizHabilitada) {
    matriz.clear();
    matriz.show();
    return;
  }

  // Modo Patrulla/Policía en caso de frenado por obstáculo
  if (distanciaActual <= DISTANCIA_SEGURIDAD_CM) {
    if (pasoAnimacion == 0) {
      matriz.fill(matriz.Color(255, 0, 0), 0, NUM_LEDS);
    } else {
      matriz.fill(matriz.Color(0, 0, 255), 0, NUM_LEDS);
    }
    matriz.show();
    return;
  }

  // Animación de ecualizador si suena la alarma sonora
  if (alarmaHabilitada && digitalRead(BUZZER_PIN) == HIGH) {
    uint32_t cAmarillo = matriz.Color(255, 200, 0);
    if (pasoAnimacion == 0) renderizarIcono(ecualizador_1, cAmarillo);
    else renderizarIcono(ecualizador_2, cAmarillo);
    return;
  }

  if (modoMatrizActual != MODO_NORMAL && millis() > tiempoFinModoEspecial) {
    modoMatrizActual = MODO_NORMAL;
  }

  if (modoMatrizActual == MODO_RADAR) {
    renderizarRadar(anguloActual);
    return;
  }
  if (modoMatrizActual == MODO_AGUJA) {
    renderizarAgujaServo(anguloActual);
    return;
  }

  uint32_t verde = matriz.Color(0, 255, 0);
  uint32_t ambar = matriz.Color(255, 180, 0);
  uint32_t azul  = matriz.Color(0, 100, 255);

  if (strcmp(accionActual, "avanzar") == 0) {
    if (pasoAnimacion == 0) renderizarIcono(flechaArriba_1, verde);
    else renderizarIcono(flechaArriba_2, verde);
  } 
  else if (strcmp(accionActual, "retroceder") == 0) {
    if (pasoAnimacion == 0) renderizarIcono(flechaAbajo_1, verde);
    else renderizarIcono(flechaAbajo_2, verde);
  } 
  else if (strcmp(accionActual, "izquierda") == 0) {
    if (pasoAnimacion == 0) renderizarIcono(flechaIzquierda, ambar);
    else matriz.clear(), matriz.show();
  } 
  else if (strcmp(accionActual, "derecha") == 0) {
    if (pasoAnimacion == 0) renderizarIcono(flechaDerecha, ambar);
    else matriz.clear(), matriz.show();
  } 
  else {
    renderizarIcono(puntoCentral, azul);
  }
}

// ---------------------------------------------------------------------------
// FUNCIONES DE CONTROL DE MOTORES
// ---------------------------------------------------------------------------
static inline void motores_init_pines() {
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  ledcAttach(PWM_A, PWM_FREQ, PWM_RES);
  ledcAttach(PWM_B, PWM_FREQ, PWM_RES);

  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  ledcWrite(PWM_A, 0); ledcWrite(PWM_B, 0);
}

static inline void set_velocidad(uint8_t vA, uint8_t vB) {
  ledcWrite(PWM_A, vA);
  ledcWrite(PWM_B, vB);
}

static inline void motores_parar_total() {
  set_velocidad(0, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

static inline void mover_avanzar() {
  set_velocidad(velAvanzar, velAvanzar);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

static inline void mover_retroceder() {
  set_velocidad(velAvanzar, velAvanzar);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

static inline void mover_izquierda() {
  set_velocidad(velGiro, velGiro);
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

static inline void mover_derecha() {
  set_velocidad(velGiro, velGiro);
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

// ---------------------------------------------------------------------------
// MEDICIÓN ULTRASÓNICA RÁPIDA
// ---------------------------------------------------------------------------
float medirDistanciaRapida() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duracion = pulseIn(ECHO_PIN, HIGH, 12000);

  if (duracion > 0) {
    return (duracion * 0.0343) / 2.0;
  }
  return 400.0;
}

// ---------------------------------------------------------------------------
// TAREA FREERTOS DE MOVIMIENTO
// ---------------------------------------------------------------------------
void tareaEjecutarMovimiento(void *pvParameters) {
  ComandoMover cmd;

  for (;;) {
    if (xQueueReceive(colaMover, &cmd, portMAX_DELAY) == pdTRUE) {
      strncpy(accionActual, cmd.accion, sizeof(accionActual) - 1);

      if (strcmp(cmd.accion, "avanzar") == 0) {
        if (distanciaActual > 0.0 && distanciaActual <= DISTANCIA_SEGURIDAD_CM) {
          motores_parar_total();
          strcpy(accionActual, "parar");
        } else {
          mover_avanzar();
        }
      } else if (strcmp(cmd.accion, "retroceder") == 0) {
        mover_retroceder();
      } else if (strcmp(cmd.accion, "izquierda") == 0) {
        mover_izquierda();
      } else if (strcmp(cmd.accion, "derecha") == 0) {
        mover_derecha();
      } else {
        motores_parar_total();
      }

      if (cmd.ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cmd.ms));
        motores_parar_total();
        strcpy(accionActual, "parar");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// MANEJADOR WEBSOCKET
// ---------------------------------------------------------------------------
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      motores_parar_total();
      strcpy(accionActual, "parar");
      break;

    case WStype_CONNECTED: {
      JsonDocument docSaludo;
      docSaludo["tipo"] = "saludo";
      docSaludo["nombre_host"] = "ESP32-S3-Robot";
      
      String res;
      serializeJson(docSaludo, res);
      webSocket.sendTXT(num, res);
      break;
    }

    case WStype_TEXT: {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload, length);
      if (error) return;

      const char* tipo = doc["tipo"];
      if (!tipo) return;

      if (strcmp(tipo, "mover") == 0) {
        ComandoMover cmd;
        const char* accion = doc["accion"] | "parar";
        strncpy(cmd.accion, accion, sizeof(cmd.accion) - 1);
        cmd.accion[sizeof(cmd.accion) - 1] = '\0';
        cmd.ms = doc["ms"] | 0;

        xQueueReset(colaMover);
        xQueueSend(colaMover, &cmd, portMAX_DELAY);
      } 
      else if (strcmp(tipo, "servo") == 0) {
        int ang = doc["angulo"] | 90;
        anguloActual = constrain(ang, 0, 180);
        servo.write(anguloActual);

        modoMatrizActual = MODO_AGUJA;
        tiempoFinModoEspecial = millis() + 800;
      }
      else if (strcmp(tipo, "leer_distancia") == 0) {
        JsonDocument docDist;
        docDist["tipo"] = "distancia_lectura";
        docDist["d"] = round(distanciaActual * 10.0) / 10.0;
        
        String res;
        serializeJson(docDist, res);
        webSocket.sendTXT(num, res);
      }
      else if (strcmp(tipo, "scan") == 0) {
        int ang = doc["angulo"] | 90;
        anguloActual = constrain(ang, 0, 180);
        servo.write(anguloActual);

        modoMatrizActual = MODO_RADAR;
        tiempoFinModoEspecial = millis() + 1000;

        vTaskDelay(pdMS_TO_TICKS(100));
        distanciaActual = medirDistanciaRapida();

        JsonDocument docScan;
        docScan["tipo"] = "scan";
        docScan["angulo"] = anguloActual;
        docScan["d"] = round(distanciaActual * 10.0) / 10.0;
        
        String res;
        serializeJson(docScan, res);
        webSocket.sendTXT(num, res);
      }
      else if (strcmp(tipo, "leer_dht") == 0) {
        float temp = dht.readTemperature();
        JsonDocument docDht;
        docDht["tipo"] = "dht_lectura";
        
        if (isnan(temp)) {
          docDht["dht_ok"] = false;
          docDht["temperatura"] = nullptr;
        } else {
          docDht["dht_ok"] = true;
          docDht["temperatura"] = round(temp * 10.0) / 10.0;
        }
        
        String res;
        serializeJson(docDht, res);
        webSocket.sendTXT(num, res);
      }
      else if (strcmp(tipo, "alarma") == 0) {
        alarmaHabilitada = doc["estado"] | true;
        if (!alarmaHabilitada) {
          digitalWrite(BUZZER_PIN, LOW);
        }
      }
      else if (strcmp(tipo, "matriz") == 0) {
        matrizHabilitada = doc["estado"] | true;
        if (!matrizHabilitada) {
          matriz.clear();
          matriz.show();
        }
      }
      else if (strcmp(tipo, "latido") == 0) {
        JsonDocument docLatido;
        docLatido["tipo"] = "latido_ok";
        
        String res;
        serializeJson(docLatido, res);
        webSocket.sendTXT(num, res);
      }
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------
// SETUP Y LOOP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // Inicialización del sensor DHT22
  dht.begin();

  matriz.begin();
  matriz.setBrightness(30);
  matriz.clear();
  matriz.show();

  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);
  servo.attach(PIN_SERVO, pulsoMinimo, pulsoMaximo);
  servo.write(anguloActual);

  motores_init_pines();

  colaMover = xQueueCreate(5, sizeof(ComandoMover));
  xTaskCreatePinnedToCore(tareaEjecutarMovimiento, "TareaMover", 2048, NULL, 1, &hTareaMover, 1);

  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
  }
  
  Serial.println("\nWiFi Conectado!");
  Serial.print("IP asignada: ");
  Serial.println(WiFi.localIP());

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
}

void loop() {
  webSocket.loop();

  unsigned long ahora = millis();

  if (ahora - ultimoMuestreoUS >= 60) {
    ultimoMuestreoUS = ahora;
    distanciaActual = medirDistanciaRapida();

    if (strcmp(accionActual, "avanzar") == 0 && distanciaActual <= DISTANCIA_SEGURIDAD_CM) {
      motores_parar_total();
      strcpy(accionActual, "parar");
    }
  }

  if (ahora - ultimoAnimacionNeo >= 120) {
    ultimoAnimacionNeo = ahora;
    pasoAnimacion = !pasoAnimacion;
    actualizarMatrizNeoPixel();
  }

  if (alarmaHabilitada && distanciaActual <= DISTANCIA_SEGURIDAD_CM) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}
