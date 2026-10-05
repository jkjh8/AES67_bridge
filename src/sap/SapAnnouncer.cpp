#include "sap/SapAnnouncer.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/Log.h"

namespace aes67 {
namespace {
constexpr char kSapMcast[] = "239.255.255.255";
constexpr uint16_t kSapPort = 9875;

uint16_t Hash16(const std::string& s) {
  uint16_t h = 0x1234;
  for (char c : s) h = (uint16_t)(h * 31 + (unsigned char)c);
  return h ? h : 1;
}

std::vector<uint8_t> BuildSap(uint32_t origin_ip_host, const std::string& sdp,
                              bool deletion) {
  std::vector<uint8_t> p;
  p.push_back(deletion ? 0x24 : 0x20);
  p.push_back(0x00);
  const uint16_t id = Hash16(sdp);
  p.push_back((uint8_t)(id >> 8));
  p.push_back((uint8_t)(id & 0xff));
  p.push_back((uint8_t)((origin_ip_host >> 24) & 0xff));
  p.push_back((uint8_t)((origin_ip_host >> 16) & 0xff));
  p.push_back((uint8_t)((origin_ip_host >> 8) & 0xff));
  p.push_back((uint8_t)(origin_ip_host & 0xff));
  static const char kType[] = "application/sdp";
  p.insert(p.end(), kType, kType + sizeof(kType));
  p.insert(p.end(), sdp.begin(), sdp.end());
  return p;
}
}

struct SapAnnouncer::Impl {
  SOCKET sock = INVALID_SOCKET;
  std::thread thread;
  std::atomic<bool> running{false};
  std::mutex mtx;
  std::condition_variable cv;

  std::function<std::string()> sdp_provider;
  uint32_t origin_ip_host = 0;
  int interval_sec = 30;
  std::mutex send_mtx;

  void SendOnce(bool deletion) {
    std::lock_guard<std::mutex> lk(send_mtx);
    if (sock == INVALID_SOCKET || !sdp_provider) return;
    const std::string sdp = sdp_provider();
    if (sdp.empty()) return;
    const std::vector<uint8_t> pkt = BuildSap(origin_ip_host, sdp, deletion);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = inet_addr(kSapMcast);
    dst.sin_port = htons(kSapPort);
    sendto(sock, (const char*)pkt.data(), (int)pkt.size(), 0,
           (sockaddr*)&dst, sizeof(dst));
  }
};

SapAnnouncer::SapAnnouncer() : impl_(std::make_unique<Impl>()) {}
SapAnnouncer::~SapAnnouncer() { Stop(); }

bool SapAnnouncer::Start(std::function<std::string()> sdp_provider,
                         uint32_t origin_ip_host, int interval_sec,
                         std::string* err) {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);

  impl_->sdp_provider = std::move(sdp_provider);
  impl_->origin_ip_host = origin_ip_host;
  impl_->interval_sec = interval_sec > 0 ? interval_sec : 30;

  impl_->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (impl_->sock == INVALID_SOCKET) {
    if (err) *err = "SAP socket() failed";
    return false;
  }
  DWORD ttl = 32;
  setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl,
             sizeof(ttl));
  if (origin_ip_host) {
    in_addr ifa;
    ifa.s_addr = htonl(origin_ip_host);
    setsockopt(impl_->sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
  }

  impl_->running = true;
  impl_->thread = std::thread([this] {
    while (impl_->running.load()) {
      impl_->SendOnce(false);
      std::unique_lock<std::mutex> lk(impl_->mtx);
      impl_->cv.wait_for(lk, std::chrono::seconds(impl_->interval_sec),
                         [this] { return !impl_->running.load(); });
    }
  });
  LOGI("sap: announcing every %ds", impl_->interval_sec);
  return true;
}

void SapAnnouncer::Stop() {
  if (!impl_->running.exchange(false)) return;
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->SendOnce(true);
  if (impl_->sock != INVALID_SOCKET) {
    closesocket(impl_->sock);
    impl_->sock = INVALID_SOCKET;
  }
  LOGI("sap: stopped");
}

void SapAnnouncer::Kick() {
  if (!impl_->running.load()) return;
  impl_->SendOnce(false);
}

}
