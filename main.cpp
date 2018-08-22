// Copyright Ashley Newson 2018

#include <iostream>
#include <functional>
#include <stdexcept>
#include <string>
#include <cstring>
#include <regex>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <poll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/netfilter_ipv4.h>
#include <termios.h>

#include "Util.hpp"
#include "Cleaner.hpp"
#include "ProxySettings.hpp"
#include "TcpServer.hpp"
#include "UdpServer.hpp"

void print_usage() {
    const char* usage = R"END_USAGE(Transproxify - Copyright Ashley Newson 2018

Usage:
    transproxify [OPTIONS...] PROXY_HOST PROXY_PORT LISTEN_PORT

Synopsis:
    Perform transparent TCP proxying through an HTTP or SOCKS4/5 proxy.

    Not all software supports configuring proxies. With transproxify, you can
    force communications to pass through a proxy from inside the router.

    Transproxify listens on a given port accepting redirected traffic. When a
    redirected client connects to transproxify, transproxify will connect to a
    given proxy server and establish a tunnel for forwarding data between the
    client and its intended server, all transparent to the client.

    Transproxify will not intercept traffic by itself. You may need to alter
    firewall settings. For example, to use transproxify to proxy HTTP and HTTPS
    traffic on ports 80 and 443 via proxyserver:8080 HTTP proxy, you might use:

      # echo 1 > /proc/sys/net/ipv4/ip_forward
      # iptables -t nat -A PREROUTING -p tcp \
            --match multiport --dports 80,443 \
            -j REDIRECT --to-port 10000
      # transproxify proxyserver 8080 10000

    If your transparent proxying machine isn't already set up to do so, it may
    also be necessary to forward other ports. A quick (and potentially
    dangerous) way to do this would be using:

      # iptables -A FORWARD -j ACCEPT

    Arp spoofing may also be convenient for certain purposes, such as security
    research from non-router hosts:

      # arpspoof -i NETWORK_INTERFACE -t CLIENT_ADDRESS ROUTER_ADDRESS

    Transproxify can also be run on the client itself if the client's iptables
    rules are set up to redirect traffic by using OUTPUT instead of PREROUTING:

      # iptables -t nat -A OUTPUT -p tcp \
            --match multiport --dports 80,443 \
            -j REDIRECT --to-port 10000

Options:
    -r PROXIED_PROTOCOL
        Specify the transport layer protocol to redirect via the given proxy.
        Not all options are supported by all proxy protocols. Default is tcp.
        Valid choices are: tcp, udp
    -t PROXY_PROTOCOL
        Specify the upstream proxy's protocol. Default is http.
        Valid choices for TCP are: direct, http, socks4, socks5
        Valid choices for UDP are: direct, socks5
    -u USERNAME
        Specify the username for proxy authentication.

        WARNING: all credentials are sent over the network in cleartext!
    -p
        Prompt for a password for proxy authentication at startup.

        WARNING: all credentials are sent over the network in cleartext!
    -P PASSWORD
        Specify the password for proxy authentication.

        WARNING: Users on the same system can often view passwords entered in
        this way by examining process tables.

        WARNING: all credentials are sent over the network in cleartext!

UDP Setup:
    Setting up UDP proxying is a little different. We must create a new lookup
    table for specially marked packets allowing us to treat any address as if
    it was local. We must then mark and redirect our desired packets using
    iptables. For example, to proxy all traffic using UDP port 53:

      # ip rule add fwmark 1 lookup 100
      # ip route add local 0.0.0.0/0 dev lo table 100
      # iptables -t mangle -A PREROUTING -p udp --dport 53 \
            -j TPROXY --tproxy-mark 0x1/0x1 --on-port 10000
      # transproxify -r udp -t socks5 proxyserver 1080 10000

Direct Connections:
    You can instruct Transproxify to communicate with destination servers
    directly, without using a proxy, for both TCP and UDP. This is primarily
    useful for debugging purposes, but may also be useful when combined with
    other types of transparent proxying software. Just specify the upstream
    proxy address as localhost and the port as 0:

      # transproxify -t direct localhost 0 10000

HTTP proxy authentication:
    If a username and password are supplied, transproxify will send a
    Proxy-Authorization header using the basic authorization scheme.

SOCKS4 proxy authentication:
    If a username or password is supplied, transproxify will use this as the
    UserId in requests to the socks server. Else, a blank UserID is sent.

SOCKS5 proxy authentication:
    If a username and password is supplied, transproxify will offer to
    authenticate using the username and password authentication method as well
    as the no authentication method. If no username or password is given, only
    the no authentication method is attempted.

Security and Disclaimer:
    Like many forms of networking software or proxies can pose a significant
    risk to network security. Transproxify provides no guarantees about
    communication confidentiality, integrity, authenticity, or availability.

    In particular, all tunnels and proxy credentials are transferred in
    cleartext across the network. Any user on the network can use transproxify
    without authentication, thus gaining access to the upstream proxy. Client
    applications should enforce their own security where possible (such as
    TLS).

    The author(s) of this software cannot be held responsible for any loss,
    damage, or otherwise bad thing which happens as a result of using this
    software. This includes, but is not limited to, data compromise, loss of
    service or integrity, and remote code execution.

    This tool is provided in good faith. Use at your own risk.
)END_USAGE";

    std::cerr << usage;
}


int main(int argc, char **argv) {
    ProxySettings::ProxyProtocol proxyProtocol = ProxySettings::ProxyProtocol::HTTP;
    ProxySettings::ProxiedProtocol proxiedProtocol = ProxySettings::ProxiedProtocol::TCP;
    std::string proxyHost;
    int proxyPort = 0;
    int listenPort = 0;
    std::string username;
    std::string password;
    bool promptPassword = false;

    int c;
    while ((c = getopt(argc, argv, "t:r:u:pP:")) != -1) {
        switch (c) {
        case 't':
            if (strcmp(optarg, "direct") == 0) {
                proxyProtocol = ProxySettings::ProxyProtocol::DIRECT;
            }
            else if (strcmp(optarg, "http") == 0) {
                proxyProtocol = ProxySettings::ProxyProtocol::HTTP;
            }
            else if (strcmp(optarg, "socks4") == 0) {
                proxyProtocol = ProxySettings::ProxyProtocol::SOCKS4;
            }
            else if (strcmp(optarg, "socks5") == 0) {
                proxyProtocol = ProxySettings::ProxyProtocol::SOCKS5;
            }
            else {
                std::cerr << "Unknown proxy protocol" << std::endl;
                print_usage();
                exit(1);
            }
            break;
        case 'r':
            if (strcmp(optarg, "tcp") == 0) {
                proxiedProtocol = ProxySettings::ProxiedProtocol::TCP;
            }
            else if (strcmp(optarg, "udp") == 0) {
                proxiedProtocol = ProxySettings::ProxiedProtocol::UDP;
            }
            else {
                std::cerr << "Unknown proxied protocol" << std::endl;
                print_usage();
                exit(1);
            }
            break;
        case 'u':
            username = optarg;
            break;
        case 'p':
            promptPassword = true;
            break;
        case 'P':
            password = optarg;
            break;
        default:
            std::cerr << "Bad option" << std::endl;
            print_usage();
            exit(1);
        }
    }
    if (argc - optind != 3) {
        print_usage();
        exit(1);
    }
    try {
        proxyHost = argv[optind];
        proxyPort = std::stoi(argv[optind+1]);
        listenPort = std::stoi(argv[optind+2]);
    } catch (const std::invalid_argument&) {
        print_usage();
        exit(1);
    }

    if (promptPassword) {
        struct termios tty;
        tcgetattr(STDIN_FILENO, &tty);
        tty.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
        std::cerr << "Please enter your proxy's password:" << std::endl;
        char passwordCstr[256] = {};
        std::cin.getline(passwordCstr, 256);
        password = passwordCstr;
        tty.c_lflag |= ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
        if (std::cin.fail()) {
            std::cerr << "Failed to get password from stdin" << std::endl;
            exit(1);
        }
    }

    ProxySettings proxySettings(proxyProtocol, proxiedProtocol, proxyHost, proxyPort, username, password);

    switch (proxiedProtocol) {
    case ProxySettings::ProxiedProtocol::TCP:
        TcpServer(proxySettings, listenPort).run();
        break;
    case ProxySettings::ProxiedProtocol::UDP:
        UdpServer(proxySettings, listenPort).run();
        break;
    }

    // Unreachable?
    throw std::runtime_error("Unreachable code");
}
