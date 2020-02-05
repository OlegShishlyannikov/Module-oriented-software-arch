#ifndef BASE_SOCKET_HPP
#define BASE_SOCKET_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>

#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <resolv.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "fmt/format.h"
#include "hook.hpp"
#include "libcidr.hpp"
#include "property.hpp"

template <uint32_t family, uint32_t socktype, uint32_t protocol> struct base_socket {
public:
  using this_t = base_socket<family, socktype, protocol>;
  static constexpr bool is_network = family == AF_INET || family == AF_INET6;
  static constexpr bool is_domain = family == AF_UNIX;
  static constexpr bool is_ipv6 = is_network && !is_domain && family == AF_INET6;
  static constexpr int32_t addrlen = this_t::is_ipv6 ? INET6_ADDRSTRLEN : INET_ADDRSTRLEN;

  struct iface_netinfo_t {
    std::array<char, this_t::addrlen> host_addr{0};
    std::array<char, this_t::addrlen> netmask{0};
    std::array<char, this_t::addrlen> broadcast{0};
    uint32_t pflen = 0;
    uint32_t scopeid;
  };

  template <bool network = is_network, bool domain = is_domain>
  explicit base_socket(const std::string &iface, typename std::enable_if<network && !domain, bool>::type * = nullptr)
      : if_(iface), threads_cnt_(0u), iface_path_info_(get_iface_info_(iface)){};

  template <bool network = is_network, bool domain = is_domain>
  explicit base_socket(const std::string &path, typename std::enable_if<!network && domain, bool>::type * = nullptr)
      : if_(path), threads_cnt_(0u){};

  template <bool network = is_network, bool domain = is_domain>
  explicit base_socket(typename std::enable_if<!network && domain, bool>::type * = nullptr) : threads_cnt_(0u){};

  template <bool network = is_network, typename RetType = iface_netinfo_t>
  const typename std::enable_if<network, RetType>::type &iface() const {
    return iface_path_info_;
  }

  template <bool domain = is_domain, typename RetType = const std::string &>
  const typename std::enable_if<domain, RetType>::type &path() const {
    return iface_path_info_;
  }

  static constexpr uint32_t epoll_max_events() { return epoll_max_events_; }
  static constexpr uint32_t send_timeout() { return send_timeout_ms_; }
  static constexpr uint32_t receive_timeout() { return receive_timeout_ms_; }
  static constexpr uint32_t connect_timeout() { return connect_timeout_ms_; }
  static constexpr uint32_t accept_timeout() { return accept_timeout_ms_; }
  static constexpr int32_t sock_family() { return family; }
  static constexpr int32_t sock_socktype() { return socktype; }
  static constexpr int32_t sock_protocol() { return protocol; }

  std::condition_variable &cv() { return cv_; }
  std::mutex &mtx() { return mtx_; }

  void stop_threads() {
    if (threads_cnt_) {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_.wait(lock, [this]() -> bool { return threads_cnt_ == 0u; });
    }
  }

  virtual ~base_socket() { stop_threads(); };

private:
  static constexpr uint32_t epoll_max_events_ = 32u;
  static constexpr uint32_t send_timeout_ms_ = 1000u;
  static constexpr uint32_t receive_timeout_ms_ = 1000u;
  static constexpr uint32_t connect_timeout_ms_ = 1000u;
  static constexpr uint32_t accept_timeout_ms_ = 1000u;

  const std::string if_;
  std::conditional_t<is_network, const struct iface_netinfo_t,
                     std::conditional_t<is_domain, const std::string, uint8_t>>
      iface_path_info_;

  std::condition_variable cv_;
  std::atomic<uint64_t> threads_cnt_;
  std::mutex mtx_;
  
  template <typename RetType = iface_netinfo_t>
  const typename std::enable_if<is_network, RetType>::type get_iface_info_(const std::string &ifname) {
    using addr_inet_t = std::conditional_t<this_t::is_ipv6, in6_addr, in_addr>;
    using sockaddr_inet_t = std::conditional_t<is_ipv6, struct sockaddr_in6, struct sockaddr_in>;
    using num_addr_t = std::conditional_t<this_t::is_ipv6, __uint128_t, uint32_t>;
    struct ifaddrs *interfaces = nullptr;

    if (!::getifaddrs(&interfaces)) {
      struct ifaddrs *temp_addr = interfaces;
      struct iface_netinfo_t info;

      while (temp_addr) {
        if (temp_addr->ifa_addr) {
          if (temp_addr->ifa_addr->sa_family == family) {
            if (temp_addr->ifa_name == ifname) {
              addr_inet_t *p_addrstr;
              addr_inet_t *p_netmaskstr;
              uint32_t scopeid;

              if constexpr (this_t::is_ipv6) {
                p_addrstr = &reinterpret_cast<sockaddr_inet_t *>(temp_addr->ifa_addr)->sin6_addr;
                p_netmaskstr = &reinterpret_cast<sockaddr_inet_t *>(temp_addr->ifa_netmask)->sin6_addr;
                scopeid = reinterpret_cast<sockaddr_inet_t *>(temp_addr->ifa_addr)->sin6_scope_id;

              } else if constexpr (!this_t::is_ipv6) {
                p_addrstr = &reinterpret_cast<sockaddr_inet_t *>(temp_addr->ifa_addr)->sin_addr;
                p_netmaskstr = &reinterpret_cast<sockaddr_inet_t *>(temp_addr->ifa_netmask)->sin_addr;
              }

              if (::inet_ntop(family, p_addrstr, info.host_addr.data(), this_t::addrlen) != info.host_addr.data())
                throw std::runtime_error(
                    fmt::format("Error during converting host address, ({0}), {1}:{2}", __func__, __FILE__, __LINE__));

              if (::inet_ntop(family, p_netmaskstr, info.netmask.data(), this_t::addrlen) != info.netmask.data())
                throw std::runtime_error(
                    fmt::format("Error during converting netmask, ({0}), {1}:{2}", __func__, __FILE__, __LINE__));

              num_addr_t num_addr;
              ::inet_pton(family, info.netmask.data(), &num_addr);
              while (num_addr > 0u) {
                num_addr = num_addr >> 1u;
                info.pflen++;
              }

              CIDR *cidr_addr =
                  cidr_from_str((std::string(info.host_addr.data()) + "/" + std::to_string(info.pflen)).c_str());
              CIDR *cidr_broadcast = cidr_addr_broadcast(cidr_addr);
              char *broadcast_addr = cidr_to_str(cidr_broadcast, CIDR_ONLYADDR);
              std::memcpy(info.broadcast.data(), broadcast_addr, std::strlen(broadcast_addr));
              std::free(broadcast_addr);
              cidr_free(cidr_broadcast);
              cidr_free(cidr_addr);

              info.scopeid = scopeid;
              ::freeifaddrs(interfaces);
              return std::move(info);
            }
          }
        } else
          goto next;

      next:
        temp_addr = temp_addr->ifa_next;
      }

      throw std::runtime_error(
          fmt::format("Interface with name {0} not found, ({1}), {2}:{3}", ifname, __func__, __FILE__, __LINE__));
    } else
      throw std::runtime_error("getifaddrs() Fuck!");
  }
};

#endif /* BASE_SOCKET_HPP */
