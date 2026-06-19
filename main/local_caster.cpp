#include "local_caster.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>

#include "app_config.hpp"
#include "esp_log.h"
#include "lwip/sockets.h"

namespace {

constexpr char kTag[] = "local_caster";

uint32_t peer_ipv4(const sockaddr_storage &source) {
    if (source.ss_family != AF_INET) return 0;
    const auto *addr = reinterpret_cast<const sockaddr_in *>(&source);
    return addr->sin_addr.s_addr;
}

void ipv4_to_text(uint32_t ipv4, char *output, size_t output_size) {
    if (output_size == 0) return;
    if (!ipv4) {
        snprintf(output, output_size, "unknown");
        return;
    }
    const uint32_t host = ntohl(ipv4);
    snprintf(output, output_size, "%u.%u.%u.%u",
             static_cast<unsigned>((host >> 24) & 0xff),
             static_cast<unsigned>((host >> 16) & 0xff),
             static_cast<unsigned>((host >> 8) & 0xff),
             static_cast<unsigned>(host & 0xff));
}

}  // namespace

LocalCaster::~LocalCaster() {
    stop();
}

esp_err_t LocalCaster::start() {
    stopping_ = false;
    queue_ = xQueueCreate(kQueueDepth, sizeof(Packet));
    if (!queue_) return ESP_ERR_NO_MEM;
    // Priority 3: below httpd (5) so local NTRIP broadcast never delays
    // HTTPS responses. Unpinned to let the scheduler balance freely.
    if (xTaskCreate(task_entry, "local_ntrip", 5120, this, 3, &task_) != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void LocalCaster::stop() {
    stopping_ = true;
    while (task_) vTaskDelay(pdMS_TO_TICKS(10));
    close_all();
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

void LocalCaster::set_suspended(bool suspended) {
    suspended_ = suspended;
    if (suspended) reset_clients_ = true;
}

void LocalCaster::push(const uint8_t *data, size_t length) {
    if (!data || length == 0 || !queue_ || suspended_) return;
    Packet packet{};
    packet.length = std::min(length, kMaxPacket);
    memcpy(packet.data, data, packet.length);
    if (xQueueSend(queue_, &packet, 0) != pdTRUE) reset_clients_ = true;
}

int LocalCaster::client_count() const {
    return client_count_;
}

LocalCaster::ClientSnapshot LocalCaster::client_snapshot() const {
    ClientSnapshot snapshot{};
    for (const auto &ipv4 : client_ipv4_) {
        const uint32_t value = ipv4.load(std::memory_order_relaxed);
        if (!value) continue;
        snapshot.ipv4[snapshot.count++] = value;
    }
    return snapshot;
}

void LocalCaster::task_entry(void *argument) {
    auto *caster = static_cast<LocalCaster *>(argument);
    caster->run();
    caster->task_ = nullptr;
    vTaskDelete(nullptr);
}

void LocalCaster::run() {
    Packet packet{};
    while (!stopping_) {
        if (suspended_) {
            close_all();
            if (queue_) xQueueReset(queue_);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (reset_clients_.exchange(false)) {
            close_clients();
            if (queue_) xQueueReset(queue_);
        }
        if (listener_ < 0 && !open_listener()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        accept_client();
        if (xQueueReceive(queue_, &packet, pdMS_TO_TICKS(20)) == pdTRUE) {
            broadcast(packet);
        }
    }
    close_all();
}

bool LocalCaster::open_listener() {
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener_ < 0) return false;
    int reuse = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config::kLocalNtripPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
            0 ||
        listen(listener_, kMaxClients) < 0) {
        close_all();
        return false;
    }
    const int flags = fcntl(listener_, F_GETFL, 0);
    fcntl(listener_, F_SETFL, flags | O_NONBLOCK);
    ESP_LOGI(kTag, "Listening on port %u /%s",
             config::kLocalNtripPort, config::kLocalMountpoint);
    return true;
}

void LocalCaster::accept_client() {
    sockaddr_storage source{};
    socklen_t source_length = sizeof(source);
    const int incoming = accept(
        listener_, reinterpret_cast<sockaddr *>(&source), &source_length);
    if (incoming < 0) return;

    timeval timeout{1, 0};
    setsockopt(incoming, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char request_buffer[512]{};
    const int received = recv(
        incoming, request_buffer, sizeof(request_buffer) - 1, 0);
    const std::string request =
        received > 0 ? std::string(request_buffer, received) : std::string();
    const std::string prefix =
        "GET /" + std::string(config::kLocalMountpoint) + " HTTP/";
    if (request.rfind(prefix, 0) != 0) {
        const std::string table =
            "SOURCETABLE 200 OK\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "STR;" + std::string(config::kLocalMountpoint) +
            ";GPS Base;RTCM 3.2;1005(5),1074(1),1084(1),1094(1),1124(1);"
            "2;GPS+GLO+GAL+BDS;NONE;0;0;N;N;0;\r\nENDSOURCETABLE\r\n";
        send_all(
            incoming, reinterpret_cast<const uint8_t *>(table.data()),
            table.size());
        close(incoming);
        return;
    }
    int slot = -1;
    for (int i = 0; i < kMaxClients; ++i) {
        if (clients_[i] < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        close(incoming);
        return;
    }
    const char response[] = "ICY 200 OK\r\n\r\n";
    if (!send_all(
            incoming, reinterpret_cast<const uint8_t *>(response),
            sizeof(response) - 1)) {
        close(incoming);
        return;
    }
    clients_[slot] = incoming;
    client_ipv4_[slot].store(peer_ipv4(source), std::memory_order_relaxed);
    recount_clients();
    char ip_text[16];
    ipv4_to_text(client_ipv4_[slot].load(std::memory_order_relaxed),
                 ip_text, sizeof(ip_text));
    ESP_LOGI(kTag, "Client %d connected from %s", slot, ip_text);
}

void LocalCaster::broadcast(const Packet &packet) {
    for (int i = 0; i < kMaxClients; ++i) {
        int &client = clients_[i];
        if (client >= 0 && !send_all(client, packet.data, packet.length)) {
            shutdown(client, SHUT_RDWR);
            close(client);
            client = -1;
            client_ipv4_[i].store(0, std::memory_order_relaxed);
        }
    }
    recount_clients();
}

void LocalCaster::close_clients() {
    for (int i = 0; i < kMaxClients; ++i) {
        int &client = clients_[i];
        if (client >= 0) {
            shutdown(client, SHUT_RDWR);
            close(client);
            client = -1;
        }
        client_ipv4_[i].store(0, std::memory_order_relaxed);
    }
    recount_clients();
}

void LocalCaster::close_all() {
    close_clients();
    if (listener_ >= 0) {
        close(listener_);
        listener_ = -1;
    }
}

void LocalCaster::recount_clients() {
    int count = 0;
    for (const int client : clients_) {
        if (client >= 0) ++count;
    }
    client_count_ = count;
}

bool LocalCaster::send_all(
    int socket_fd, const uint8_t *data, size_t length) {
    size_t offset = 0;
    int stalls = 0;
    while (offset < length) {
        const int sent = send(
            socket_fd, data + offset, length - offset, MSG_DONTWAIT);
        if (sent > 0) {
            offset += sent;
            stalls = 0;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(socket_fd, &write_set);
            timeval timeout{0, 100000};
            const int ready = select(
                socket_fd + 1, nullptr, &write_set, nullptr, &timeout);
            if (ready > 0) continue;
            if (ready < 0 && errno == EINTR) continue;
            if (++stalls < 10) continue;
        }
        return false;
    }
    return true;
}
