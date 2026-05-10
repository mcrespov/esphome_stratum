#pragma once

#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <cstring>
#include <sys/time.h>

// ============================================================
// Servidor NTP minimalista para ESPHome (ESP32-C6 / ESP-IDF)
// Escucha en UDP/123 y responde con la hora del sistema
// (que ESPHome ya sincroniza desde el GPS)
// ============================================================

static const char *const TAG_NTP = "ntp_server";

// Offset entre la época UNIX (1970) y la época NTP (1900) en segundos
static const uint64_t NTP_UNIX_OFFSET = 2208988800ULL;

static int ntp_sock_fd = -1;

// Estructura del paquete NTP (48 bytes, sin extensiones ni MAC)
struct ntp_packet {
  uint8_t  li_vn_mode;       // Leap Indicator, Version, Mode
  uint8_t  stratum;
  uint8_t  poll;
  int8_t   precision;
  uint32_t root_delay;
  uint32_t root_dispersion;
  uint32_t ref_id;
  uint32_t ref_ts_sec;
  uint32_t ref_ts_frac;
  uint32_t orig_ts_sec;
  uint32_t orig_ts_frac;
  uint32_t recv_ts_sec;
  uint32_t recv_ts_frac;
  uint32_t tx_ts_sec;
  uint32_t tx_ts_frac;
} __attribute__((packed));

// Convierte struct timeval (UNIX) a segundos+fracción NTP en network byte order
// Devuelve los valores por puntero (no referencia) para que funcione con campos packed
static inline void timeval_to_ntp(const struct timeval &tv, uint32_t *sec_ne, uint32_t *frac_ne) {
  uint32_t sec  = (uint32_t)(tv.tv_sec + NTP_UNIX_OFFSET);
  // fracción: 1 segundo = 2^32. Convertimos us a fracción de 2^32
  uint32_t frac = (uint32_t)((double)tv.tv_usec * 4294.967296);  // 2^32 / 1e6
  *sec_ne  = htonl(sec);
  *frac_ne = htonl(frac);
}

void ntp_server_setup() {
  ntp_sock_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (ntp_sock_fd < 0) {
    ESP_LOGE(TAG_NTP, "No se pudo crear el socket UDP (errno=%d)", errno);
    return;
  }

  // No bloqueante
  int flags = fcntl(ntp_sock_fd, F_GETFL, 0);
  fcntl(ntp_sock_fd, F_SETFL, flags | O_NONBLOCK);

  // Permitir reusar puerto si reinicia
  int yes = 1;
  setsockopt(ntp_sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr{};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(123);

  if (::bind(ntp_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG_NTP, "Fallo al hacer bind en UDP/123 (errno=%d)", errno);
    ::close(ntp_sock_fd);
    ntp_sock_fd = -1;
    return;
  }

  ESP_LOGI(TAG_NTP, "Servidor NTP escuchando en UDP/123");
}

void ntp_server_loop() {
  if (ntp_sock_fd < 0) return;

  ntp_packet req{};
  struct sockaddr_in client{};
  socklen_t client_len = sizeof(client);

  // Lectura no bloqueante; si no hay paquete, salimos
  int n = ::recvfrom(ntp_sock_fd, &req, sizeof(req), 0,
                     (struct sockaddr *)&client, &client_len);
  if (n < (int)sizeof(ntp_packet)) {
    return;  // nada o paquete inválido
  }

  // Timestamp de RECEPCIÓN: lo más cerca posible del recvfrom
  struct timeval recv_tv;
  gettimeofday(&recv_tv, nullptr);

  // Guarda: no respondas con horas absurdas (antes del año 2023)
  // Esto evita servir basura mientras el GPS aún no tiene fix
  if (recv_tv.tv_sec < 1700000000) {
    ESP_LOGD(TAG_NTP, "Hora aun no valida, descartando peticion");
    return;
  }

  // Construimos respuesta en variables locales (no podemos pasar
  // referencias a campos packed, asi que rellenamos por separado)
  uint32_t ref_sec_ne, ref_frac_ne;
  uint32_t recv_sec_ne, recv_frac_ne;
  uint32_t tx_sec_ne, tx_frac_ne;

  timeval_to_ntp(recv_tv, &ref_sec_ne,  &ref_frac_ne);
  timeval_to_ntp(recv_tv, &recv_sec_ne, &recv_frac_ne);

  // Transmit timestamp = ahora mismo, justo antes de enviar
  struct timeval tx_tv;
  gettimeofday(&tx_tv, nullptr);
  timeval_to_ntp(tx_tv, &tx_sec_ne, &tx_frac_ne);

  ntp_packet resp{};
  // LI=0, VN=4, Mode=4 (server)  -> 0b00 100 100 = 0x24 
  resp.li_vn_mode = (0 << 6) | (4 << 3) | 4;
  resp.stratum    = 1;        // stratum 1 (reloj de referencia primario: GPS)
  resp.poll       = req.poll; // devolvemos el poll del cliente
  resp.precision  = -20;      // ~1 microsegundo (aprox.)

  resp.root_delay      = htonl(0);
  resp.root_dispersion = htonl(0x00000010);
  // ref_id: 4 chars ASCII para stratum 1; "GPS\0"
  resp.ref_id          = htonl(0x47505300);  // 'G''P''S''\0'

  resp.ref_ts_sec   = ref_sec_ne;
  resp.ref_ts_frac  = ref_frac_ne;

  // Originate = el transmit timestamp que envio el cliente, tal cual
  resp.orig_ts_sec  = req.tx_ts_sec;
  resp.orig_ts_frac = req.tx_ts_frac;

  resp.recv_ts_sec  = recv_sec_ne;
  resp.recv_ts_frac = recv_frac_ne;

  resp.tx_ts_sec    = tx_sec_ne;
  resp.tx_ts_frac   = tx_frac_ne;

  ::sendto(ntp_sock_fd, &resp, sizeof(resp), 0,
           (struct sockaddr *)&client, client_len);

  ESP_LOGD(TAG_NTP, "Respondido NTP a %s:%u",
           inet_ntoa(client.sin_addr), ntohs(client.sin_port));
}
