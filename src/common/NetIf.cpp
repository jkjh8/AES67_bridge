#include "common/NetIf.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <cstdio>

namespace aes67 {
namespace {

std::string Narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

uint32_t DefaultRouteIpHost() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return 0;
  sockaddr_in probe{};
  probe.sin_family = AF_INET;
  probe.sin_addr.s_addr = inet_addr("8.8.8.8");
  probe.sin_port = htons(53);
  uint32_t ip = 0;
  if (connect(s, (sockaddr*)&probe, sizeof(probe)) == 0) {
    sockaddr_in local{};
    int len = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &len) == 0) ip = ntohl(local.sin_addr.s_addr);
  }
  closesocket(s);
  return ip;
}

}

std::vector<NetIf> EnumNetIfs() {
  std::vector<NetIf> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf;
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
    buf.resize(size);
    rc = GetAdaptersAddresses(AF_INET,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
  }
  if (rc != NO_ERROR) return out;
  for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    uint32_t ip = 0;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
      if (u->Address.lpSockaddr->sa_family == AF_INET) {
        ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
        break;
      }
    if (!ip || (ip >> 16) == 0xA9FE) continue;
    NetIf n;
    n.id = a->AdapterName ? a->AdapterName : "";
    n.name = Narrow(a->FriendlyName);
    n.description = Narrow(a->Description);
    n.ip_host = ip;
    char t[32];
    snprintf(t, sizeof(t), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
             ip & 0xFF);
    n.ip = t;
    if (a->PhysicalAddressLength == 6) {
      memcpy(n.mac, a->PhysicalAddress, 6);
      n.has_mac = true;
      snprintf(t, sizeof(t), "%02X:%02X:%02X:%02X:%02X:%02X", n.mac[0], n.mac[1], n.mac[2],
               n.mac[3], n.mac[4], n.mac[5]);
      n.mac_text = t;
    }
    n.speed_bps = a->TransmitLinkSpeed == (ULONG64)-1 ? 0 : a->TransmitLinkSpeed;
    n.wireless = a->IfType == IF_TYPE_IEEE80211;
    out.push_back(std::move(n));
  }
  return out;
}

NetIf ResolveNetIf(const std::string& id, const std::string& ip, bool* is_auto) {
  const std::vector<NetIf> all = EnumNetIfs();
  if (is_auto) *is_auto = false;
  if (!id.empty())
    for (const auto& n : all)
      if (n.id == id) return n;
  if (!ip.empty())
    for (const auto& n : all)
      if (n.ip == ip) return n;
  if (is_auto) *is_auto = true;
  const uint32_t def = DefaultRouteIpHost();
  for (const auto& n : all)
    if (n.ip_host == def) return n;
  if (!all.empty()) return all.front();
  NetIf none;
  none.ip_host = def;
  return none;
}

}
