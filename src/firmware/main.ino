#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>
#include "config.h"

// -------------------- PIN SENSORE TILT --------------------
const int tiltPin = D5;
const int DEBOUNCE_MS = 50; // tempo minimo tra due eventi consecutivi

// -------------------- DISPLAY LCD --------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// -------------------- STATO DISPOSITIVO --------------------
int lastTiltState = HIGH; // ultimo stato letto dal sensore
unsigned long lastPublishTime = 0;

// Conteggi allarmi ricevuti da AWS
int alarmLastHour = 0;
int alarmLast12h = 0;
int alarmLast24h = 0;

// -------------------- CLIENT MQTT --------------------
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// -------------------- DICHIARAZIONE FUNZIONI --------------------
void connectWiFi();
void syncNTP();
void connectMQTT();
void publishTilt();
void callback(char* topic, byte* payload, unsigned int length);
void updateDisplay();

// SETUP
void setup() {
  Serial.begin(115200);
  pinMode(tiltPin, INPUT_PULLUP);

  // Inizializza display I2C
  Wire.begin(D2, D1);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connessione...");
  lcd.setCursor(0, 1);
  lcd.print("WiFi e NTP");

  // Connessione WiFi
  connectWiFi();

  // Sincronizzazione NTP (necessaria per timestamp Unix)
  syncNTP();

  // Configura client MQTT
  wifiClient.setTrustAnchors(new BearSSL::X509List(rootCA));
  wifiClient.setClientRSACert(new BearSSL::X509List(deviceCert), new BearSSL::PrivateKey(privateKey));
  mqttClient.setServer(AWS_ENDPOINT, AWS_PORT);
  mqttClient.setBufferSize(1024); // buffer a 1KB
  mqttClient.setCallback(callback);

  // Connessione ad AWS IoT Core
  connectMQTT();

  // Mostra schermata iniziale
  updateDisplay();
}

// LOOP
void loop() {
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  // Lettura sensore tilt con debounce
  int currentState = digitalRead(tiltPin);
  unsigned long now = millis();

  // Se lo stato cambia e il tempo di debounce è passato
  if (currentState != lastTiltState && (now - lastPublishTime > DEBOUNCE_MS)) {
    lastTiltState = currentState;
    lastPublishTime = now;

    // Pubblica l'evento
    if (currentState == LOW) {
      publishTilt();
    }
  }
}

// CONNESSIONE WiFi
void connectWiFi() {
  delay(10);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connessione WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println(".");
  }
  Serial.println("\nWiFi connesso, IP: " + WiFi.localIP().toString());
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi OK");
}

// SINCRONIZZAZIONE NTP
void syncNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sincronizzazione NTP");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\nNTP sincronizzato");
  lcd.setCursor(0, 1);
  lcd.print("NTP OK");
  delay(1000);
}

// CONNESSIONE MQTT AD AWS IOT
void connectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connessione MQTT...");
    if (mqttClient.connect(CLIENT_ID)) {
      Serial.println("connesso!");

      // Sottoscrizione al topic shadow delta
      String shadowTopic = "$aws/things/" + String(CLIENT_ID) + "/shadow/update/accepted";
      mqttClient.subscribe(shadowTopic.c_str());
      Serial.println("Sottoscritto a " + shadowTopic);

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("AWS Connesso");
      delay(1000);
    } else {
      Serial.print("fallito, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" riprovo tra 5 secondi");
      delay(5000);
    }
  }
}

// PUBBLICAZIONE EVENTO TILT
void publishTilt() {
  // Costruisco il payload JSON
  StaticJsonDocument<128> doc;
  doc["deviceId"] = CLIENT_ID;
  doc["timestamp"] = time(nullptr); // Unix timestamp (da NTP)

  String payload;
  serializeJson(doc, payload);

  // Topic di pubblicazione
  String pubTopic = "devices/" + String(CLIENT_ID) + "/tilt";
  if (mqttClient.publish(pubTopic.c_str(), payload.c_str())) {
    Serial.println("Evento tilt pubblicato: " + payload);
  } else {
    Serial.println("Pubblicazione fallita");
  }
}


void callback(char* topic, byte* payload, unsigned int length) {
  Serial.println("\n--- AGGIORNAMENTO RICEVUTO ---");

  // documento JSON
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("Errore parsing JSON: ");
    Serial.println(error.f_str());
    return;
  }

  // leggo il JSON ricevuto: state -> desired -> alarmCounts
  JsonObject counts = doc["state"]["desired"]["alarmCounts"];
  
  if (!counts.isNull()) {
    // Estrazione Intervallo 1
    int h1 = counts["lastH1"]["label"];
    int c1 = counts["lastH1"]["count"];
    // Estrazione Intervallo 2
    int h2 = counts["lastH2"]["label"];
    int c2 = counts["lastH2"]["count"];
    // Estrazione Intervallo 3
    int h3 = counts["lastH3"]["label"];
    int c3 = counts["lastH3"]["count"];

    // Debug su Seriale
    Serial.printf("Intervalli letti: %dh(%d), %dh(%d), %dh(%d)\n", h1, c1, h2, c2, h3, c3);

    // AGGIORNAMENTO LCD
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(String(h1) + "h:" + String(c1));
    lcd.setCursor(8, 0);
    lcd.print(String(h2) + "h:" + String(c2));

    lcd.setCursor(0, 1);
    lcd.print(String(h3) + "h:" + String(c3));
    
    Serial.println("Display LCD aggiornato correttamente.");
  } else {
    Serial.println("Messaggio ricevuto, ma non contiene dati alarmCounts.");
  }
}

// AGGIORNAMENTO DISPLAY CON I CONTTEGGI ALLARMI
void updateDisplay() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("1h:");
  lcd.print(alarmLastHour);
  lcd.setCursor(8, 0);
  lcd.print("12h:");
  lcd.print(alarmLast12h);

  lcd.setCursor(0, 1);
  lcd.print("24h:");
  lcd.print(alarmLast24h);
}
