/*
 * ESP32 FFT TIME - WebSocket
 * Copyright (c) 2025 Alvaro Salazar
 * Licensed under the MIT License.
 */

#include <SPIFFS.h>
#include "libwebsocket.h"

// Definición única de las instancias globales
AsyncWebServer server(80);
AsyncWebSocket ws(WS_PATH);

/**
 * @brief Inicializa el WebSocket y lo asocia al servidor.
 *        Debe llamarse en el setup principal.
 */
void setupWebSocket(){
  // Cuando ocurre un evento (conexión, desconexión, etc.) se llama a esta función
  ws.onEvent([](AsyncWebSocket *s, AsyncWebSocketClient*, AwsEventType t, void*, uint8_t*, size_t){});
  server.addHandler(&ws); // Agrega el WebSocket al servidor
  
  // Inicia SPIFFS que es el sistema de archivos SPI (memoria flash)
  if(!SPIFFS.begin(true)){
    Serial.println("ERROR: No se pudo montar SPIFFS");
    return;
  }
  
  // Verifica que el archivo index.html existe
  if(!SPIFFS.exists("/index.html")){
    Serial.println("ERROR: index.html no encontrado en SPIFFS");
    Serial.println("Archivos en SPIFFS:");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while(file){
      Serial.printf("  - %s (%d bytes)\n", file.name(), file.size());
      file = root.openNextFile();
    }
  } else {
    File f = SPIFFS.open("/index.html", "r");
    if(f){
      Serial.printf("index.html encontrado: %d bytes\n", f.size());
      f.close();
    }
  }
  
  // Sirve el archivo index.html que está en SPIFFS (sistema de archivos SPI)
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
  
  // Maneja rutas no encontradas
  server.onNotFound([](AsyncWebServerRequest *request){
    Serial.printf("404: %s\n", request->url().c_str());
    request->send(404, "text/plain", "Not found");
  });
  
  server.begin(); // Inicia el servidor
  Serial.println("Servidor WebSocket iniciado en http://fft32.local");
}
