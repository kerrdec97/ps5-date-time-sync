/*
 * PS5 Date/Time Sync by Deckerr97
 * Copyright (C) 2026 Deckerr97
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file for details.
 *
 * One-shot payload for consoles whose RTC/CMOS backup battery no longer
 * retains the clock across a full power-off.  It obtains UTC from SNTP,
 * corrects for half of the measured round-trip time, sets the system clock,
 * and exits after reporting the result through the notification center.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

extern "C" {
int sceNetInit();
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);

struct NotifyRequest {
  char useless[45];
  char message[3075];
};

int sceKernelSendNotificationRequest(int device, NotifyRequest* request,
                                     size_t size, int unused);

// Keep the payload independent of feature-test macros in the SDK's FreeBSD
// headers.  These symbols are provided by libkernel and the SDK's libc.
int gettimeofday(struct timeval*, struct timezone*);
int settimeofday(const struct timeval*, const struct timezone*);
void arc4random_buf(void*, size_t);
}

namespace {

constexpr const char* kVersion = "1.1.0";

constexpr uint16_t kNtpPort = 123;
constexpr size_t kNtpPacketSize = 48;
constexpr size_t kMaxNtpResponseSize = 512;
constexpr size_t kNtpRootDelayOffset = 4;
constexpr size_t kNtpRootDispersionOffset = 8;
constexpr size_t kNtpReferenceIdOffset = 12;
constexpr size_t kNtpReferenceTimestampOffset = 16;
constexpr size_t kNtpOriginateTimestampOffset = 24;
constexpr size_t kNtpTransmitTimestampOffset = 40;
constexpr size_t kNtpTimestampSize = 8;
constexpr uint8_t kNtpVersion = 4;
constexpr uint8_t kNtpOldestAcceptedVersion = 3;
constexpr uint8_t kNtpClientMode = 3;
constexpr uint8_t kNtpServerMode = 4;
constexpr uint8_t kNtpClientFlags = (kNtpVersion << 3) | kNtpClientMode;
constexpr int kNetworkPoolSize = 64 * 1024;
constexpr int64_t kSocketTimeoutMicroseconds = 3 * 1000000LL;
constexpr int kAttemptsPerServer = 2;
constexpr size_t kMaxAddressesPerServer = 4;
constexpr unsigned int kRetryDelaySeconds = 1;
constexpr int64_t kMaxClockVerificationDeltaMicroseconds = 2 * 1000000LL;
// Same limit as systemd-timesyncd: a server whose own estimated error is
// this large is broken or badly misconfigured.
constexpr int64_t kMaxRootDistanceMicroseconds = 5 * 1000000LL;

// NTP timestamps count from 1900-01-01; Unix timestamps count from
// 1970-01-01.
constexpr uint32_t kNtpToUnixEpoch = 2208988800U;
constexpr int64_t kMinimumUnixTime = 946684800LL;       // 2000-01-01
constexpr int64_t kMaximumUnixTime = 4102444800LL;      // 2100-01-01

const char* const kNtpServers[] = {
    // Cloudflare publishes these anycast addresses for clients that cannot
    // resolve time.cloudflare.com. Keep them first so clock recovery does
    // not depend on working DNS.
    "162.159.200.1",
    "162.159.200.123",
    "time.cloudflare.com",
    "pool.ntp.org",
    "time.google.com",
};

enum class QueryResult {
  kSuccess,
  // Transient failure: another attempt may succeed.
  kRetry,
  // The server refused service or reported that it is unusable.
  kSkipServer,
};

enum class SyncResult {
  kSynchronized,
  kNetworkError,
  kNoResponse,
  kClockError,
};

struct NtpSample {
  // Server time, corrected for half the round trip, at the moment the
  // response was received.
  int64_t unix_microseconds;
  // CLOCK_MONOTONIC reading taken when the response was received.
  int64_t received_monotonic_microseconds;
};

void log_message(const char* format, ...) {
  char line[768] = {};
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (written <= 0) {
    return;
  }

  size_t length = static_cast<size_t>(written);
  if (length >= sizeof(line)) {
    length = sizeof(line) - 1;
  }
  fwrite(line, 1, length, stdout);
  fflush(stdout);
}

void notify_user(SyncResult result, const char* time_text) {
  const char* status = "Time sync failed";
  switch (result) {
    case SyncResult::kSynchronized:
      status = "Time synced";
      break;
    case SyncResult::kNetworkError:
      status = "Time sync failed: network unavailable";
      break;
    case SyncResult::kNoResponse:
      status = "Time sync failed: no NTP server answered (UDP 123 blocked?)";
      break;
    case SyncResult::kClockError:
      status = "Time sync failed: the clock could not be set";
      break;
  }

  NotifyRequest request = {};
  if (result == SyncResult::kSynchronized && time_text && time_text[0]) {
    snprintf(request.message, sizeof(request.message),
             "%s: %s\nPS5 time & date sync v%s by Deckerr97", status,
             time_text, kVersion);
  } else {
    snprintf(request.message, sizeof(request.message),
             "%s\nPS5 time & date sync v%s by Deckerr97", status, kVersion);
  }
  int rc = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
  log_message("[notification] rc=%d: %s\n", rc, request.message);
}

class NetworkSession {
 public:
  NetworkSession() = default;
  NetworkSession(const NetworkSession&) = delete;
  NetworkSession& operator=(const NetworkSession&) = delete;

  ~NetworkSession() {
    if (pool_id_ >= 0) {
      int result = sceNetPoolDestroy(pool_id_);
      log_message("sceNetPoolDestroy rc=%d\n", result);
    }
  }

  bool initialize() {
    int result = sceNetInit();
    if (result != 0) {
      log_message("sceNetInit failed: rc=%d\n", result);
      return false;
    }

    pool_id_ = sceNetPoolCreate("ps5-date-time-sync", kNetworkPoolSize, 0);
    if (pool_id_ < 0) {
      log_message("sceNetPoolCreate failed: rc=%d\n", pool_id_);
      return false;
    }
    return true;
  }

 private:
  int pool_id_ = -1;
};

bool monotonic_microseconds(int64_t* output) {
  if (!output) {
    return false;
  }

  struct timespec value = {};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    log_message("clock_gettime(CLOCK_MONOTONIC) failed: errno=%d\n", errno);
    return false;
  }

  *output = static_cast<int64_t>(value.tv_sec) * 1000000LL +
            static_cast<int64_t>(value.tv_nsec / 1000);
  return true;
}

uint32_t read_big_endian_u32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

// Converts an NTP short-format (16.16 fixed point) value to microseconds.
int64_t ntp_short_to_microseconds(const uint8_t* bytes) {
  return static_cast<int64_t>(
      (static_cast<uint64_t>(read_big_endian_u32(bytes)) * 1000000ULL) >> 16);
}

void write_request_nonce(uint8_t* packet) {
  // The server copies the request's Transmit Timestamp into its Originate
  // Timestamp, which is what ties a response to this exact request.  Use
  // random bytes instead of the local clock: on a console whose clock has
  // just reset, the clock value is easy to guess, and sending it to the
  // server serves no purpose.
  uint8_t* nonce = packet + kNtpTransmitTimestampOffset;
  arc4random_buf(nonce, kNtpTimestampSize);
  // Never send an all-zero value; servers treat it as "no timestamp".
  nonce[kNtpTimestampSize - 1] |= 1;
}

bool set_socket_timeout(int socket_fd, int option, int64_t microseconds) {
  struct timeval timeout = {};
  timeout.tv_sec = static_cast<time_t>(microseconds / 1000000LL);
  timeout.tv_usec = static_cast<suseconds_t>(microseconds % 1000000LL);
  if (setsockopt(socket_fd, SOL_SOCKET, option, &timeout, sizeof(timeout)) !=
      0) {
    log_message("%s could not be set: errno=%d\n",
                option == SO_RCVTIMEO ? "SO_RCVTIMEO" : "SO_SNDTIMEO", errno);
    return false;
  }
  return true;
}

// Fills `addresses` with up to `capacity` IPv4 endpoints for `hostname`.
size_t resolve_server(const char* hostname, struct sockaddr_in* addresses,
                      size_t capacity) {
  if (!hostname || !addresses || capacity == 0) {
    return 0;
  }

  struct sockaddr_in numeric = {};
  int numeric_result = inet_pton(AF_INET, hostname, &numeric.sin_addr);
  if (numeric_result == 1) {
    numeric.sin_family = AF_INET;
    numeric.sin_port = htons(kNtpPort);
    addresses[0] = numeric;
    return 1;
  }
  if (numeric_result < 0) {
    log_message("IPv4 address parsing failed for %s: errno=%d\n", hostname,
                errno);
    return 0;
  }

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  struct addrinfo* results = nullptr;
  int result = getaddrinfo(hostname, nullptr, &hints, &results);
  if (result != 0) {
    log_message("DNS lookup failed for %s: %s\n", hostname,
                gai_strerror(result));
    return 0;
  }

  size_t count = 0;
  for (struct addrinfo* current = results; current && count < capacity;
       current = current->ai_next) {
    if (!current->ai_addr || current->ai_addrlen < sizeof(struct sockaddr_in)) {
      continue;
    }

    memcpy(&addresses[count], current->ai_addr, sizeof(struct sockaddr_in));
    addresses[count].sin_port = htons(kNtpPort);
    ++count;
  }

  freeaddrinfo(results);
  if (count == 0) {
    log_message("DNS lookup for %s returned no IPv4 address\n", hostname);
  }
  return count;
}

QueryResult parse_response(const uint8_t* response, int64_t round_trip,
                           int64_t received_monotonic, NtpSample* output) {
  uint8_t flags = response[0];
  uint8_t leap_indicator = flags >> 6;
  uint8_t version = (flags >> 3) & 0x07;
  uint8_t mode = flags & 0x07;
  uint8_t stratum = response[1];

  if (stratum == 0) {
    // Kiss-o'-Death: the reference ID carries a four-letter code such as
    // RATE or DENY, and the server asks not to be queried again for now.
    char code[5] = {};
    memcpy(code, response + kNtpReferenceIdOffset, 4);
    for (char& character : code) {
      if (character && (character < 0x20 || character > 0x7e)) {
        character = '?';
      }
    }
    log_message("server sent Kiss-o'-Death \"%s\"; skipping it\n", code);
    return QueryResult::kSkipServer;
  }
  if (leap_indicator == 3 || mode != kNtpServerMode ||
      version < kNtpOldestAcceptedVersion || version > kNtpVersion ||
      stratum >= 16) {
    log_message("invalid NTP response: LI=%u version=%u mode=%u stratum=%u\n",
                static_cast<unsigned int>(leap_indicator),
                static_cast<unsigned int>(version),
                static_cast<unsigned int>(mode),
                static_cast<unsigned int>(stratum));
    return QueryResult::kSkipServer;
  }

  int64_t root_distance =
      ntp_short_to_microseconds(response + kNtpRootDelayOffset) / 2 +
      ntp_short_to_microseconds(response + kNtpRootDispersionOffset);
  if (root_distance > kMaxRootDistanceMicroseconds) {
    log_message("NTP server root distance is too large: %lld microseconds\n",
                static_cast<long long>(root_distance));
    return QueryResult::kSkipServer;
  }

  if (read_big_endian_u32(response + kNtpReferenceTimestampOffset) == 0 &&
      read_big_endian_u32(response + kNtpReferenceTimestampOffset + 4) == 0) {
    log_message("NTP server has never been synchronized\n");
    return QueryResult::kSkipServer;
  }

  uint32_t ntp_seconds =
      read_big_endian_u32(response + kNtpTransmitTimestampOffset);
  uint32_t ntp_fraction =
      read_big_endian_u32(response + kNtpTransmitTimestampOffset + 4);
  if (ntp_seconds == 0 && ntp_fraction == 0) {
    log_message("NTP response has an empty transmit timestamp\n");
    return QueryResult::kRetry;
  }

  // NTP's seconds field wrapped in 2036.  A value below the Unix-epoch
  // offset is therefore treated as the beginning of the next NTP era.
  uint64_t full_ntp_seconds = ntp_seconds;
  if (full_ntp_seconds < kNtpToUnixEpoch) {
    full_ntp_seconds += (1ULL << 32);
  }
  int64_t unix_seconds = static_cast<int64_t>(
      full_ntp_seconds - static_cast<uint64_t>(kNtpToUnixEpoch));
  if (unix_seconds < kMinimumUnixTime || unix_seconds > kMaximumUnixTime) {
    log_message("NTP response is outside the accepted date range: %lld\n",
                static_cast<long long>(unix_seconds));
    return QueryResult::kSkipServer;
  }

  int64_t microseconds =
      static_cast<int64_t>((static_cast<uint64_t>(ntp_fraction) * 1000000ULL) >>
                           32);
  if (round_trip > 0) {
    microseconds += round_trip / 2;
  }

  output->unix_microseconds = unix_seconds * 1000000LL + microseconds;
  output->received_monotonic_microseconds = received_monotonic;
  return QueryResult::kSuccess;
}

QueryResult query_socket(int socket_fd, const struct sockaddr_in& server,
                         NtpSample* output) {
  // A connected UDP socket only delivers datagrams from the server's address
  // and port, so the kernel filters out traffic from any other endpoint.
  if (connect(socket_fd, reinterpret_cast<const struct sockaddr*>(&server),
              sizeof(server)) != 0) {
    log_message("connect failed: errno=%d\n", errno);
    return QueryResult::kRetry;
  }
  if (!set_socket_timeout(socket_fd, SO_SNDTIMEO, kSocketTimeoutMicroseconds)) {
    return QueryResult::kRetry;
  }

  uint8_t request[kNtpPacketSize] = {};
  // Leap indicator 0, version 4, client mode 3.
  request[0] = kNtpClientFlags;
  write_request_nonce(request);

  int64_t start_microseconds = 0;
  if (!monotonic_microseconds(&start_microseconds)) {
    return QueryResult::kRetry;
  }

  ssize_t sent = send(socket_fd, request, sizeof(request), 0);
  if (sent != static_cast<ssize_t>(sizeof(request))) {
    log_message("send failed: errno=%d\n", errno);
    return QueryResult::kRetry;
  }

  // Keep reading until a response that matches this request arrives or the
  // timeout expires, so one stray datagram cannot end the attempt early.
  int64_t deadline = start_microseconds + kSocketTimeoutMicroseconds;
  for (;;) {
    int64_t now_microseconds = 0;
    if (!monotonic_microseconds(&now_microseconds)) {
      return QueryResult::kRetry;
    }
    if (now_microseconds >= deadline) {
      log_message("timed out waiting for a matching NTP response\n");
      return QueryResult::kRetry;
    }
    if (!set_socket_timeout(socket_fd, SO_RCVTIMEO,
                            deadline - now_microseconds)) {
      return QueryResult::kRetry;
    }

    uint8_t response[kMaxNtpResponseSize] = {};
    ssize_t received = recv(socket_fd, response, sizeof(response), 0);
    int receive_errno = errno;
    int64_t end_microseconds = 0;
    if (!monotonic_microseconds(&end_microseconds)) {
      return QueryResult::kRetry;
    }

    if (received < 0) {
      if (receive_errno == EINTR) {
        continue;
      }
      if (receive_errno == EAGAIN || receive_errno == EWOULDBLOCK) {
        log_message("timed out waiting for an NTP response\n");
        return QueryResult::kRetry;
      }
      log_message("recv failed: errno=%d\n", receive_errno);
      return QueryResult::kRetry;
    }
    if (received < static_cast<ssize_t>(kNtpPacketSize)) {
      log_message("ignoring short NTP response: %lld bytes\n",
                  static_cast<long long>(received));
      continue;
    }
    if (memcmp(response + kNtpOriginateTimestampOffset,
               request + kNtpTransmitTimestampOffset,
               kNtpTimestampSize) != 0) {
      log_message("ignoring NTP response that does not match the request\n");
      continue;
    }

    return parse_response(response, end_microseconds - start_microseconds,
                          end_microseconds, output);
  }
}

QueryResult request_time(const struct sockaddr_in& server, NtpSample* output) {
  if (!output) {
    return QueryResult::kRetry;
  }

  int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    log_message("socket failed: errno=%d\n", errno);
    return QueryResult::kRetry;
  }

  QueryResult result = query_socket(socket_fd, server, output);
  close(socket_fd);
  return result;
}

bool format_utc(time_t seconds, char* output, size_t output_size) {
  if (!output || output_size == 0) {
    return false;
  }

  struct tm broken_down = {};
  if (!gmtime_r(&seconds, &broken_down)) {
    return false;
  }

  return strftime(output, output_size, "%Y-%m-%d %H:%M:%S UTC", &broken_down) >
         0;
}

bool set_system_time(const NtpSample& sample, struct timeval* applied) {
  // Account for the time spent since the response arrived (validation,
  // logging) so the clock is set to the server's time as of right now.
  int64_t now_monotonic = 0;
  if (!monotonic_microseconds(&now_monotonic)) {
    return false;
  }
  int64_t elapsed = now_monotonic - sample.received_monotonic_microseconds;
  if (elapsed < 0) {
    elapsed = 0;
  }
  int64_t target = sample.unix_microseconds + elapsed;

  struct timeval candidate = {};
  candidate.tv_sec = static_cast<time_t>(target / 1000000LL);
  candidate.tv_usec = static_cast<suseconds_t>(target % 1000000LL);
  if (settimeofday(&candidate, nullptr) != 0) {
    log_message("settimeofday failed: errno=%d\n", errno);
    return false;
  }

  struct timeval verified = {};
  if (gettimeofday(&verified, nullptr) != 0) {
    log_message("gettimeofday verification failed: errno=%d\n", errno);
    return false;
  }

  log_message("applied %lld microseconds of processing delay\n",
              static_cast<long long>(elapsed));

  int64_t difference =
      (static_cast<int64_t>(verified.tv_sec) -
       static_cast<int64_t>(candidate.tv_sec)) *
          1000000LL +
      static_cast<int64_t>(verified.tv_usec) -
      static_cast<int64_t>(candidate.tv_usec);
  if (difference < 0) {
    difference = -difference;
  }
  log_message("clock verification delta: %lld microseconds\n",
              static_cast<long long>(difference));
  if (difference > kMaxClockVerificationDeltaMicroseconds) {
    log_message("clock verification exceeded the allowed delta\n");
    return false;
  }

  *applied = candidate;
  return true;
}

bool query_server(const char* server_name, NtpSample* sample) {
  struct sockaddr_in addresses[kMaxAddressesPerServer] = {};
  size_t address_count =
      resolve_server(server_name, addresses, kMaxAddressesPerServer);
  if (address_count == 0) {
    return false;
  }

  for (int attempt = 0; attempt < kAttemptsPerServer; ++attempt) {
    // Spread the attempts over every address the name resolved to, so one
    // unreachable pool member does not use up all of them.
    const struct sockaddr_in& address =
        addresses[static_cast<size_t>(attempt) % address_count];
    char address_text[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &address.sin_addr, address_text,
                   sizeof(address_text))) {
      snprintf(address_text, sizeof(address_text), "?");
    }
    log_message("querying %s [%s] (attempt %d/%d)\n", server_name,
                address_text, attempt + 1, kAttemptsPerServer);

    QueryResult result = request_time(address, sample);
    if (result == QueryResult::kSuccess) {
      return true;
    }
    if (result == QueryResult::kSkipServer) {
      return false;
    }
    if (attempt + 1 < kAttemptsPerServer) {
      sleep(kRetryDelaySeconds);
    }
  }
  return false;
}

SyncResult synchronize_system_time(char* time_text, size_t time_text_size) {
  log_message("PS5 time & date sync v%s by Deckerr97\n", kVersion);

  NetworkSession network;
  if (!network.initialize()) {
    return SyncResult::kNetworkError;
  }

  NtpSample sample = {};
  bool received_time = false;
  for (const char* server_name : kNtpServers) {
    if (query_server(server_name, &sample)) {
      received_time = true;
      break;
    }
  }

  if (!received_time) {
    log_message("no valid NTP response was received\n");
    return SyncResult::kNoResponse;
  }

  struct timeval applied = {};
  if (!set_system_time(sample, &applied)) {
    return SyncResult::kClockError;
  }

  if (format_utc(applied.tv_sec, time_text, time_text_size)) {
    log_message("system clock set to %s\n", time_text);
  } else {
    time_text[0] = '\0';
  }
  log_message("date/time synchronization completed\n");
  return SyncResult::kSynchronized;
}

}  // namespace

int main() {
  char time_text[64] = {};
  SyncResult result = synchronize_system_time(time_text, sizeof(time_text));
  notify_user(result, time_text);
  return result == SyncResult::kSynchronized ? 0 : 1;
}
