#include <WiFi.h>
#include "ThingSpeak.h"
#include <time.h>

// Sistema de iluminacion y monitoreo del ciclo de sueno (ESP32 + ThingSpeak)

// horario del usuario
const int DORMIR_HORA = 22;
const int DORMIR_MIN = 0;
const int DESPERTAR_HORA = 5;
const int DESPERTAR_MIN = 0;
const float RAMPA_MIN_DEFAULT = 10.0;
const float INICIO_NOCHE = 19.0;

// aviso de luz roja
const float AVISO_ON_MIN = 10.0;
const float AVISO_OFF_MIN = 20.0;
const float AVISO_MAX_MIN = 240.0;
const float ANTICIPACION_H = 2.0;

// true = reloj acelerado, false = hora real por NTP
#define MODO_DEMO true

const unsigned long long FACTOR_NORMAL = 120;   // 1 s real = 2 min
const unsigned long long FACTOR_RAPIDO = 600;   // 1 s real = 10 min
const float MARGEN_RAPIDO_MIN = 20.0;

// pines
#define PIN_PRESION 13
#define PIN_PIR 27
#define PIN_LDR 34
#define PIN_BOTON 14
#define PIN_LED_R 25
#define PIN_LED_G 26
#define PIN_LED_B 33

const bool LDR_INVERTIDO = false;

// PWM
const int FREQ_PWM = 5000;
const int RES_PWM = 8;

// colores
const int ROJO_AVISO_R = 90;
const int LUZ_COMPLETA_R = 255;
const int LUZ_COMPLETA_G = 190;
const int LUZ_COMPLETA_B = 110;

const char* ID_ESP = "ESP32_SUENO_01";
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ThingSpeak
// canal datos: 1 dia, 2 hora acostarse, 3 latencia, 4 despertar pred, 5 levantado, 6 tiempo apagar, 7 luz, 8 evento
const unsigned long canalDatos = 3592249;
const char* apiKeyDatos = "7N0IX5799A2GHSXI";

// canal predicciones: 1 dormir, 2 despertar, 3 rampa
const unsigned long canalPredicciones = 3592253;

const unsigned long INTERVALO_ENVIO_MIN = 16000;
const unsigned long INTERVALO_PREDICCION = 60000;

WiFiClient client;

float horaDormirPred = DORMIR_HORA + DORMIR_MIN / 60.0;
float horaDespertarPred = DESPERTAR_HORA + DESPERTAR_MIN / 60.0;
float rampaMin = RAMPA_MIN_DEFAULT;

const float VENTANA_ALARMA_H = 3.0;
const unsigned long SEG_QUIETUD_DORMIDO = 300;
const unsigned long TIEMPO_REBOTE = 15;
const unsigned long PENDIENTE_BOTON_MS = 30000;

// estados
const int ESTADO_DIA = 0;
const int ESTADO_NOCHE = 1;
const int ESTADO_AVISO_NOCHE = 2;
const int ESTADO_ESPERA_AVISO = 3;
const int ESTADO_EN_CAMA = 4;
const int ESTADO_DORMIDO = 5;
const int ESTADO_AMANECER = 6;
const int ESTADO_ALARMA = 7;
const char* NOMBRES_ESTADO[8] = {"DIA", "NOCHE", "AVISO_NOCHE", "ESPERA_AVISO", "EN_CAMA", "DORMIDO", "AMANECER", "ALARMA"};

int estado = ESTADO_DIA;

// entrada digital con antirrebote
class Entrada
{
  public:
    int pin;
    bool activoEnBajo;
    bool estado;
    bool lecturaAnterior;
    unsigned long tCambio;
    bool flancoActivo;
    bool flancoInactivo;

    Entrada(int p, bool enBajo)
    {
      pin = p;
      activoEnBajo = enBajo;
      estado = false;
      lecturaAnterior = false;
      tCambio = 0;
      flancoActivo = false;
      flancoInactivo = false;
    }

    bool leerCruda()
    {
      bool alto = (digitalRead(pin) == HIGH);
      if (activoEnBajo)
      {
        return !alto;
      }
      return alto;
    }

    void iniciar()
    {
      if (activoEnBajo)
      {
        pinMode(pin, INPUT_PULLUP);
      }
      else
      {
        pinMode(pin, INPUT);
      }
      estado = leerCruda();
      lecturaAnterior = estado;
      tCambio = millis();
    }

    void actualizar()
    {
      bool actual = leerCruda();

      if (actual != lecturaAnterior)
      {
        lecturaAnterior = actual;
        tCambio = millis();
      }

      if (actual != estado && (millis() - tCambio) >= TIEMPO_REBOTE)
      {
        estado = actual;
        if (estado)
        {
          flancoActivo = true;
        }
        else
        {
          flancoInactivo = true;
        }
      }
    }

    bool tomarActivo()
    {
      bool v = flancoActivo;
      flancoActivo = false;
      return v;
    }

    bool tomarInactivo()
    {
      bool v = flancoInactivo;
      flancoInactivo = false;
      return v;
    }
};

Entrada entradaPresion(PIN_PRESION, false);
Entrada entradaBoton(PIN_BOTON, true);

// paquete para imprimir en el monitor serial
class Paquete
{
  public:
    String sensores[10];
    String datos_sensores[10];
    String unidades[10];
    int cantidadDatos;

    Paquete()
    {
      cantidadDatos = 0;
    }

    void AgregarDato(String nombre, float valor, String unidad)
    {
      if (cantidadDatos < 10)
      {
        sensores[cantidadDatos] = nombre;
        datos_sensores[cantidadDatos] = String(valor, 2);
        unidades[cantidadDatos] = unidad;
        cantidadDatos++;
      }
    }

    String ImprimirCadena()
    {
      String cadena = "";
      for (int i = 0; i < cantidadDatos; i++)
      {
        cadena += sensores[i] + ":" + datos_sensores[i] + unidades[i];
        if (i < cantidadDatos - 1)
        {
          cadena += ",";
        }
      }
      cadena += ",timestamp:" + String(millis());
      return cadena;
    }

    void Limpiar()
    {
      cantidadDatos = 0;
    }
};

Paquete paquete;

// sensores
bool presion = false;
bool movimiento = false;
bool presenciaCama = false;
int luzCruda = 0;
float luzPct = 0;

// ciclo de sueno
bool sesionSueno = false;
bool durmiendo = false;
bool quieto = false;
bool alarmaActiva = false;
bool alarmaApagadaHoy = false;
bool enCicloMatutinoAnterior = false;

unsigned long tAcostarse = 0;
unsigned long tInicioQuietud = 0;
unsigned long tInicioAlarma = 0;

float horaAnterior = 0;
float horaAcostarse = 0;
int diaAcostarse = 1;
float latenciaMin = 0;
float horaLevantado = 0;
float tiempoApagarMin = 0;

bool pendienteDormirse = false;
bool pendienteLevantarse = false;

bool botonPendienteCama = false;
unsigned long tBotonPendiente = 0;

// reloj de simulacion
float horaInicioDemo = 21.5;
unsigned long long simMs = 0;
unsigned long ultimoMillisReloj = 0;
unsigned long long factorActual = FACTOR_NORMAL;
const int DIA_INICIO_DEMO = 1;

// temporizadores
unsigned long tiempoAnterior = 0;
const unsigned long intervalo = 100;
unsigned long ultimoImpreso = 0;
const unsigned long intervaloImpresion = 2500;
unsigned long ultimoEnvio = 0;
unsigned long ultimaPrediccion = 0;

// reloj
unsigned long segundosTotales()
{
  if (MODO_DEMO)
  {
    return (unsigned long)(simMs / 1000ULL);
  }
  return (unsigned long)time(nullptr);
}

void avanzarRelojDemo()
{
  unsigned long ahora = millis();
  simMs += (unsigned long long)(ahora - ultimoMillisReloj) * factorActual;
  ultimoMillisReloj = ahora;
}

float horaDelDia()
{
  if (MODO_DEMO)
  {
    unsigned long seg = segundosTotales() % 86400UL;
    return seg / 3600.0;
  }

  struct tm t;
  if (!getLocalTime(&t, 10))
  {
    return horaAnterior;
  }
  return t.tm_hour + t.tm_min / 60.0 + t.tm_sec / 3600.0;
}

// 1 = lunes ... 7 = domingo
int diaSemana()
{
  if (MODO_DEMO)
  {
    unsigned long dias = segundosTotales() / 86400UL;
    return (int)((DIA_INICIO_DEMO - 1 + dias) % 7) + 1;
  }

  struct tm t;
  if (!getLocalTime(&t, 10))
  {
    return 1;
  }
  return (t.tm_wday + 6) % 7 + 1;
}

String formatoHora(float h)
{
  int hh = (int)h;
  int mm = (int)((h - hh) * 60.0 + 0.5);

  if (mm == 60)
  {
    hh++;
    mm = 0;
  }
  hh = hh % 24;

  String s = "";
  if (hh < 10) s += "0";
  s += String(hh) + ":";
  if (mm < 10) s += "0";
  s += String(mm);
  return s;
}

float normalizarHora(float h)
{
  while (h < 0) h += 24.0;
  while (h >= 24.0) h -= 24.0;
  return h;
}

float minutosDesde(float h, float ref)
{
  float dif = normalizarHora(h) - normalizarHora(ref);
  if (dif < 0) dif += 24.0;
  return dif * 60.0;
}

// true si h esta entre ini y fin (cruza medianoche)
bool enVentana(float h, float ini, float fin)
{
  ini = normalizarHora(ini);
  fin = normalizarHora(fin);

  if (ini <= fin)
  {
    return (h >= ini && h < fin);
  }
  return (h >= ini || h < fin);
}

// LED RGB
void inicializarPWM()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LED_R, FREQ_PWM, RES_PWM);
  ledcAttach(PIN_LED_G, FREQ_PWM, RES_PWM);
  ledcAttach(PIN_LED_B, FREQ_PWM, RES_PWM);
#else
  ledcSetup(0, FREQ_PWM, RES_PWM);
  ledcSetup(1, FREQ_PWM, RES_PWM);
  ledcSetup(2, FREQ_PWM, RES_PWM);
  ledcAttachPin(PIN_LED_R, 0);
  ledcAttachPin(PIN_LED_G, 1);
  ledcAttachPin(PIN_LED_B, 2);
#endif
}

void ponerColor(float r, float g, float b)
{
  int ri = constrain((int)r, 0, 255);
  int gi = constrain((int)g, 0, 255);
  int bi = constrain((int)b, 0, 255);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LED_R, ri);
  ledcWrite(PIN_LED_G, gi);
  ledcWrite(PIN_LED_B, bi);
#else
  ledcWrite(0, ri);
  ledcWrite(1, gi);
  ledcWrite(2, bi);
#endif
}

void actualizarLED(float h)
{
  if (estado == ESTADO_AVISO_NOCHE)
  {
    ponerColor(ROJO_AVISO_R, 0, 0);
  }
  else if (estado == ESTADO_AMANECER)
  {
    float minutosHasta = 1440.0 - minutosDesde(h, horaDespertarPred);
    float p = 1.0 - (minutosHasta / rampaMin);
    if (p < 0) p = 0;
    if (p > 1) p = 1;

    ponerColor(LUZ_COMPLETA_R * p, LUZ_COMPLETA_G * p * p, LUZ_COMPLETA_B * p * p * p);
  }
  else if (estado == ESTADO_ALARMA)
  {
    ponerColor(LUZ_COMPLETA_R, LUZ_COMPLETA_G, LUZ_COMPLETA_B);
  }
  else
  {
    ponerColor(0, 0, 0);
  }
}

// wifi y thingspeak
void inicializarWiFi()
{
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
  }

  Serial.println("\nWiFi conectado");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

void inicializarReloj()
{
  if (MODO_DEMO)
  {
    Serial.println("Reloj: modo demo");
    return;
  }

  configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");

  Serial.print("Sincronizando hora");
  int intentos = 0;
  while (time(nullptr) < 100000 && intentos < 20)
  {
    delay(500);
    Serial.print(".");
    intentos++;
  }
  Serial.println();
}

void leerPredicciones()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  int codigo = ThingSpeak.readMultipleFields(canalPredicciones);

  if (codigo == 200)
  {
    float dormir = ThingSpeak.getFieldAsFloat(1);
    float despertar = ThingSpeak.getFieldAsFloat(2);
    float rampa = ThingSpeak.getFieldAsFloat(3);

    if (dormir > 0 && dormir < 24)
    {
      horaDormirPred = dormir;
    }

    if (despertar > 0 && despertar < 24)
    {
      horaDespertarPred = despertar;
    }

    if (rampa >= 1 && rampa <= 60)
    {
      rampaMin = rampa;
    }

    Serial.println("Predicciones -> dormir " + formatoHora(horaDormirPred) +
                   ", despertar " + formatoHora(horaDespertarPred) +
                   ", rampa " + String(rampaMin, 0) + " min");
  }
  else
  {
    Serial.println("No se pudieron leer predicciones, error " + String(codigo));
  }
}

// evento 1 = se durmio, evento 2 = se levanto
bool enviarThingSpeak(int evento)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi desconectado, reconectando...");
    WiFi.reconnect();
    return false;
  }

  ThingSpeak.setField(1, diaAcostarse);
  ThingSpeak.setField(2, horaAcostarse);
  ThingSpeak.setField(3, latenciaMin);
  ThingSpeak.setField(4, horaDespertarPred);
  ThingSpeak.setField(5, evento == 2 ? horaLevantado : 0.0f);
  ThingSpeak.setField(6, evento == 2 ? tiempoApagarMin : 0.0f);
  ThingSpeak.setField(7, luzPct);
  ThingSpeak.setField(8, evento);

  Serial.println("Enviando evento " + String(evento) + " a ThingSpeak...");

  int x = ThingSpeak.writeFields(canalDatos, apiKeyDatos);

  if (x == 200)
  {
    Serial.println("Datos enviados");
    return true;
  }

  Serial.println("Error " + String(x) + ", se reintenta en 16 s");
  return false;
}

// sensores y logica
void leerSensores()
{
  presion = entradaPresion.estado;
  movimiento = (digitalRead(PIN_PIR) == HIGH);
  presenciaCama = (presion || movimiento);

  luzCruda = analogRead(PIN_LDR);
  luzPct = luzCruda * 100.0 / 4095.0;

  if (LDR_INVERTIDO)
  {
    luzPct = 100.0 - luzPct;
  }
}

void registrarAcostarse(unsigned long t, float h)
{
  sesionSueno = true;
  durmiendo = false;
  quieto = false;
  latenciaMin = 0;
  tAcostarse = t;
  horaAcostarse = h;
  diaAcostarse = diaSemana();
  botonPendienteCama = false;

  Serial.println("[EVENTO] Se acosto a las " + formatoHora(h));
}

void actualizarLogica()
{
  unsigned long t = segundosTotales();
  float h = horaDelDia();
  horaAnterior = h;

  float inicioRampa = horaDespertarPred - rampaMin / 60.0;

  bool enRampa = enVentana(h, inicioRampa, horaDespertarPred);
  bool enVentanaAlarma = enVentana(h, horaDespertarPred, horaDespertarPred + VENTANA_ALARMA_H);
  bool enCicloMatutino = enVentana(h, inicioRampa, horaDespertarPred + VENTANA_ALARMA_H);

  // al terminar la manana se reinicia todo
  if (!enCicloMatutino)
  {
    alarmaApagadaHoy = false;
    alarmaActiva = false;
  }

  if (enCicloMatutinoAnterior && !enCicloMatutino)
  {
    sesionSueno = false;
    durmiendo = false;
    quieto = false;
  }
  enCicloMatutinoAnterior = enCicloMatutino;

  // cama
  if (entradaPresion.tomarActivo())
  {
    Serial.println("[CAMA] Presion detectada a las " + formatoHora(h));
  }

  if (entradaPresion.tomarInactivo())
  {
    Serial.println("[CAMA] Dejo la cama a las " + formatoHora(h));

    if (sesionSueno && !enCicloMatutino)
    {
      sesionSueno = false;
      durmiendo = false;
      quieto = false;

      Serial.println("[EVENTO] Sesion cancelada, salio de la cama");
    }
  }

  // alarma
  if (!alarmaActiva && !alarmaApagadaHoy && enVentanaAlarma)
  {
    alarmaActiva = true;
    tInicioAlarma = t - (unsigned long)(minutosDesde(h, horaDespertarPred) * 60.0);

    Serial.println("[EVENTO] Hora de despertar: " + formatoHora(horaDespertarPred));
  }

  // boton: acostarse en la noche, apagar alarma en la manana
  bool botonPulsado = entradaBoton.tomarActivo();

  if (botonPulsado)
  {
    if (!alarmaApagadaHoy && (alarmaActiva || enRampa))
    {
      if (alarmaActiva)
      {
        tiempoApagarMin = (t - tInicioAlarma) / 60.0;
      }
      else
      {
        tiempoApagarMin = 0;
      }

      horaLevantado = h;
      alarmaActiva = false;
      alarmaApagadaHoy = true;
      sesionSueno = false;
      durmiendo = false;
      quieto = false;
      pendienteLevantarse = true;

      Serial.println("[EVENTO] Se levanto a las " + formatoHora(h) +
                     ", tardo " + String(tiempoApagarMin, 1) + " min en apagar la alarma");
    }
    else if (sesionSueno)
    {
      Serial.println("[BOTON] Ya estas acostado desde las " + formatoHora(horaAcostarse));
    }
    else if (!enVentana(h, horaDormirPred - ANTICIPACION_H, inicioRampa))
    {
      Serial.println("[BOTON] Fuera del horario de sueno");
    }
    else if (!presenciaCama)
    {
      botonPendienteCama = true;
      tBotonPendiente = millis();

      Serial.println("[BOTON] Se confirma cuando se detecte la cama (30 s)");
    }
    else
    {
      registrarAcostarse(t, h);
    }
  }

  if (botonPendienteCama)
  {
    if (millis() - tBotonPendiente > PENDIENTE_BOTON_MS)
    {
      botonPendienteCama = false;

      Serial.println("[BOTON] Se vencio, presiona de nuevo en la cama");
    }
    else if (sesionSueno || alarmaActiva)
    {
      botonPendienteCama = false;
    }
    else if (presenciaCama)
    {
      registrarAcostarse(t, h);
    }
  }

  // se duerme cuando esta en la cama sin moverse 5 min
  if (sesionSueno && !durmiendo)
  {
    if (presion && !movimiento)
    {
      if (!quieto)
      {
        quieto = true;
        tInicioQuietud = t;
      }

      if (t - tInicioQuietud >= SEG_QUIETUD_DORMIDO)
      {
        durmiendo = true;
        latenciaMin = (tInicioQuietud - tAcostarse) / 60.0;
        pendienteDormirse = true;

        Serial.println("[EVENTO] Se durmio, latencia " + String(latenciaMin, 1) + " min");
      }
    }
    else
    {
      quieto = false;
    }
  }

  // aviso nocturno
  float minAviso = minutosDesde(h, horaDormirPred - AVISO_ON_MIN / 60.0);
  bool enAviso = (minAviso < AVISO_MAX_MIN);

  float ciclo = AVISO_ON_MIN + AVISO_OFF_MIN;
  float posCiclo = minAviso - ciclo * (int)(minAviso / ciclo);
  bool faseEncendida = (posCiclo < AVISO_ON_MIN);

  // dia o noche
  float inicioAviso = horaDormirPred - AVISO_ON_MIN / 60.0;
  float inicioNoche = INICIO_NOCHE;

  if (minutosDesde(inicioAviso, inicioNoche) > 720.0)
  {
    inicioNoche = inicioAviso - 1.0;
  }

  bool esNoche = enVentana(h, inicioNoche, horaDespertarPred);

  // estado de la habitacion
  if (alarmaActiva)
  {
    estado = ESTADO_ALARMA;
  }
  else if (!alarmaApagadaHoy && enRampa)
  {
    estado = ESTADO_AMANECER;
  }
  else if (sesionSueno)
  {
    if (durmiendo)
    {
      estado = ESTADO_DORMIDO;
    }
    else
    {
      estado = ESTADO_EN_CAMA;
    }
  }
  else if (enAviso)
  {
    if (faseEncendida)
    {
      estado = ESTADO_AVISO_NOCHE;
    }
    else
    {
      estado = ESTADO_ESPERA_AVISO;
    }
  }
  else if (esNoche)
  {
    estado = ESTADO_NOCHE;
  }
  else
  {
    estado = ESTADO_DIA;
  }

  // velocidad del reloj de la demo
  if (MODO_DEMO)
  {
    float minParaAviso = minutosDesde(inicioAviso, h);
    float minParaRampa = minutosDesde(inicioRampa, h);

    bool sinActividad = (estado == ESTADO_DIA || estado == ESTADO_NOCHE || estado == ESTADO_DORMIDO);

    if (sinActividad && minParaAviso > MARGEN_RAPIDO_MIN && minParaRampa > MARGEN_RAPIDO_MIN)
    {
      factorActual = FACTOR_RAPIDO;
    }
    else
    {
      factorActual = FACTOR_NORMAL;
    }
  }

  actualizarLED(h);
}

void imprimirEstado()
{
  paquete.Limpiar();
  paquete.AgregarDato("dia", diaSemana(), "");
  paquete.AgregarDato("presion", presion, "");
  paquete.AgregarDato("movimiento", movimiento, "");
  paquete.AgregarDato("luz", luzPct, "%");

  Serial.println(formatoHora(horaDelDia()) + " | " + NOMBRES_ESTADO[estado] + " | " + paquete.ImprimirCadena());
}

void setup()
{
  Serial.begin(115200);

  entradaPresion.iniciar();
  entradaBoton.iniciar();

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_LDR, INPUT);

  inicializarPWM();
  ponerColor(0, 0, 0);

  Serial.println("SISTEMA DE ILUMINACION Y MONITOREO DEL SUENO");
  Serial.println(ID_ESP);

  inicializarWiFi();

  ThingSpeak.begin(client);

  inicializarReloj();

  leerPredicciones();

  // la demo arranca 30 min antes de la hora de dormir
  horaInicioDemo = normalizarHora(horaDormirPred - 0.5);
  simMs = (unsigned long long)(horaInicioDemo * 3600.0) * 1000ULL;
  ultimoMillisReloj = millis();

  horaAnterior = horaDelDia();

  Serial.println("Horario -> dormir " + formatoHora(horaDormirPred) +
                 ", despertar " + formatoHora(horaDespertarPred) +
                 ", amanecer " + String(rampaMin, 0) + " min");
}

void loop()
{
  unsigned long tiempoActual = millis();

  entradaPresion.actualizar();
  entradaBoton.actualizar();

  if (tiempoActual - tiempoAnterior >= intervalo)
  {
    tiempoAnterior = tiempoActual;

    if (MODO_DEMO)
    {
      avanzarRelojDemo();
    }

    leerSensores();
    actualizarLogica();
  }

  if (tiempoActual - ultimoImpreso >= intervaloImpresion)
  {
    ultimoImpreso = tiempoActual;
    imprimirEstado();
  }

  // envio a ThingSpeak, si falla se reintenta
  if (tiempoActual - ultimoEnvio >= INTERVALO_ENVIO_MIN)
  {
    if (pendienteDormirse)
    {
      ultimoEnvio = tiempoActual;
      if (enviarThingSpeak(1))
      {
        pendienteDormirse = false;
      }
    }
    else if (pendienteLevantarse)
    {
      ultimoEnvio = tiempoActual;
      if (enviarThingSpeak(2))
      {
        pendienteLevantarse = false;
      }
    }
  }

  // predicciones del modelo, solo de dia
  if (tiempoActual - ultimaPrediccion >= INTERVALO_PREDICCION && estado == ESTADO_DIA)
  {
    ultimaPrediccion = tiempoActual;
    leerPredicciones();
  }
}
