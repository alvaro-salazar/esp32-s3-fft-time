/*
 * ESP32 FFT TIME - DSP
 * Copyright (c) 2025 Alvaro Salazar
 * Licensed under the MIT License.
 */

#include <driver/adc.h>
#include <driver/timer.h>
#include <Arduino.h>
#include <string.h>
#include <arduinoFFT.h>
#include "libdisplay.h"
#include "libwebsocket.h"
#include "libdsp.h"

#define ADC_CH        ADC1_CHANNEL_0  // Canal ADC (GPIO1 en ESP32-S3)
#define SR_HZ         1000            // Frecuencia de muestreo (Hz)
#define N_SAMPLES     1024            // Cantidad de muestras por bloque
#define TIMER_DIVIDER 80              // Divisor del timer (80MHz / 80 = 1MHz = 1us por tick)
#define TIMER_SCALE   (TIMER_BASE_CLK / TIMER_DIVIDER)  // Escala del timer

static uint16_t  rawBuf[N_SAMPLES];   // Buffer para almacenar las muestras
static uint16_t  block_copy[N_SAMPLES]; // Buffer para copiar antes de enviar (evita stack overflow)
static uint16_t  dummy[N_SAMPLES];    // Buffer para descartar bloques antiguos (evita stack overflow)
static QueueHandle_t qFFT;            // Cola para envío de bloques de muestras
static hw_timer_t *timer = NULL;      // Timer para muestreo preciso
static volatile uint32_t sample_idx = 0; // Índice de muestra (volatile para interrupción)
static volatile bool buffer_ready = false; // Flag para indicar que el buffer está listo

static double    vReal[N_SAMPLES];    // Buffer para magnitudes reales de la FFT
static double    vImag[N_SAMPLES];    // Buffer para magnitudes imaginarias de la FFT

ArduinoFFT<double> FFT(vReal, vImag, N_SAMPLES, SR_HZ); // Objeto FFT

// Interrupción del timer para muestreo preciso
void IRAM_ATTR onTimer() {
  if(sample_idx < N_SAMPLES) {
    rawBuf[sample_idx++] = (uint16_t)adc1_get_raw(ADC_CH);
    if(sample_idx >= N_SAMPLES) {
      buffer_ready = true;
      sample_idx = 0;
    }
  }
}

/**
 * @brief Configura el ADC y el timer para muestreo preciso con DMA (ESP32-S3)
 * 
 */
void setupAdc(){
  // Configura el ancho del ADC a 12 bits
  adc1_config_width(ADC_WIDTH_BIT_12);
  
  // Configura el canal ADC y el atenuador (0-3.3V)
  adc1_config_channel_atten(ADC_CH, ADC_ATTEN_DB_12);
  
  // Configura el timer para muestreo preciso
  timer = timerBegin(0, TIMER_DIVIDER, true);  // Timer 0, divisor 80, cuenta hacia arriba
  timerAttachInterrupt(timer, onTimer, true);  // Asocia la interrupción (true = edge triggered)
  timerAlarmWrite(timer, TIMER_SCALE / SR_HZ, true); // Configura el período (1000us para 1000Hz)
  timerAlarmEnable(timer);                      // Habilita la alarma del timer
}

/**
 * @brief Define la cola para envío de bloques de muestras
 */
void setQueue(){
  qFFT = xQueueCreate(2, N_SAMPLES*sizeof(uint16_t)); // aqui se define la cola para envío de bloques de muestras
}

/**
 * @brief Tarea para la lectura del ADC con timer e interrupciones (ESP32-S3)
 *        Usa un timer de hardware para muestreo preciso y envía bloques a la cola
 */
void taskADC(void *){
  for(;;){
    // Espera a que el buffer esté listo (muestreado por la interrupción del timer)
    if(buffer_ready){
      // Desactiva temporalmente las interrupciones para copiar el buffer de forma segura
      portDISABLE_INTERRUPTS();
      memcpy(block_copy, rawBuf, N_SAMPLES * sizeof(uint16_t));
      buffer_ready = false;
      portENABLE_INTERRUPTS();
      
      // Envía bloque completo a la cola (sin bloquear si la cola está llena)
      BaseType_t queue_result = xQueueSend(qFFT, block_copy, 0);
      if(queue_result != pdTRUE){
        // Si la cola está llena, descarta el bloque anterior y envía el nuevo
        // Esto previene que se acumulen datos y cause "pegado"
        xQueueReceive(qFFT, dummy, 0); // Descarta el bloque más antiguo
        xQueueSend(qFFT, block_copy, 0);  // Envía el nuevo bloque
      }
    }
    // Pequeño delay para evitar saturar el CPU
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/**
 * @brief Tarea para la FFT y el WebSocket
 */
void taskFFT(void *){
  uint16_t block[N_SAMPLES]; // Buffer para almacenar el bloque de muestras

  for(;;){
    // Recibe bloque de muestras (con timeout para evitar bloqueos)
    if(xQueueReceive(qFFT, block, pdMS_TO_TICKS(100))==pdTRUE){
      double mean = 0; // Media de las muestras
      for (uint16_t i=0;i<N_SAMPLES;i++) mean += block[i]; // Calcula la media de las muestras
      mean /= N_SAMPLES; // Divide la suma por el número de muestras para obtener la media
      
      // Aquí puedes aplicar un filtro digital sobre 'block' antes de la FFT
      // Por ejemplo: filtro pasa bajos, pasa altos, etc.
      // for (uint16_t i=0; i<N_SAMPLES; i++) block[i] = filtro(block[i]);
      
      for (uint16_t i=0;i<N_SAMPLES;i++){ // Recorre todas las muestras
        vReal[i] = block[i] - mean;   // quita DC
        vImag[i] = 0; // Inicializa la parte imaginaria a 0
      }
      FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD); // Aplica la ventana de Hamming
      FFT.compute(FFT_FORWARD); // Realiza la FFT
      FFT.complexToMagnitude(); // Convierte a magnitud

      /* armar JSON (solo 0‑500 Hz para no saturar) */
      String json = "{\"time\":[";
      for(uint16_t i=0;i<N_SAMPLES;i++){ // Recorre todas las muestras
        json += String(block[i]); // Agrega el valor de la muestra al JSON
        if(i<N_SAMPLES-1) json += ","; // Agrega una coma si no es la última muestra
      }
      json += "],\"freq\":["; // Agrega la etiqueta "freq" al JSON
      uint16_t limit = N_SAMPLES/2; // Límite para recorrer las muestras
      for(uint16_t i=0;i<limit;i++){ // Recorre las muestras hasta el límite
        json += String(vReal[i], 1);     // magnitud
        if(i<limit-1) json += ","; // Agrega una coma si no es la última muestra
      }
      json += "]}"; // Agrega el cierre del JSON

      // Envía el JSON a todos los clientes (sin bloquear)
      ws.textAll(json);   // Envía el JSON

      drawFFT(vReal, vImag); // Dibuja el espectro FFT en la pantalla OLED
    }
    // Pequeño delay para evitar saturar el CPU
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}