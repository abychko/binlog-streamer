/* Copyright (c) 2026, Alexey Bychko.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of binlog-streamer hereby grant you an
   additional permission to link the program and your derivative works
   with the separately licensed software that they have either included
   with the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "net/cTlsTransport.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <future>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include "net/cTcpTransport.hpp"
#include "net/cTlsCertificateGenerator.hpp"
#include "net/cTlsContext.hpp"

namespace binlog_streamer {
namespace {

constexpr std::chrono::milliseconds TIMEOUT{5000};

// Loopback TCP listener on an OS-assigned port (as in cTcpTransportTest).
class LoopbackListener {
 public:
  LoopbackListener() {
    m_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(m_listenSocket, reinterpret_cast<struct sockaddr *>(&address),
         sizeof(address));
    listen(m_listenSocket, 1);
    socklen_t addressLength = sizeof(address);
    getsockname(m_listenSocket, reinterpret_cast<struct sockaddr *>(&address),
                &addressLength);
    m_port = ntohs(address.sin_port);
  }
  ~LoopbackListener() {
    if (m_listenSocket >= 0) close(m_listenSocket);
  }
  LoopbackListener(const LoopbackListener &) = delete;
  LoopbackListener &operator=(const LoopbackListener &) = delete;

  std::uint16_t Port() const { return m_port; }
  int Accept() const { return accept(m_listenSocket, nullptr, nullptr); }

 private:
  int m_listenSocket = -1;
  std::uint16_t m_port = 0;
};

const GeneratedCertificates &Certificates() {
  static const GeneratedCertificates certificates = [] {
    GeneratedCertificates generated;
    std::string error;
    if (!TlsCertificateGenerator::Generate("test", generated, error))
      ADD_FAILURE() << error;
    return generated;
  }();
  return certificates;
}

TlsMaterial ServerMaterial() {
  return TlsMaterial{Certificates().caCertPem, Certificates().serverCertPem,
                     Certificates().serverKeyPem};
}

// What one side of a loopback TLS connection did: echo, or fail the
// handshake, and what it saw.
struct ServerResult {
  bool accepted = false;
  std::string error;
  std::vector<std::uint8_t> received;
};

// Accepts one connection, runs the TLS handshake with serverContext, reads
// expectedBytes and writes them back, then closes.
std::future<ServerResult> RunEchoServer(const LoopbackListener &listener,
                                        const TlsContext &serverContext,
                                        std::size_t expectedBytes) {
  return std::async(std::launch::async, [&listener, &serverContext,
                                         expectedBytes] {
    ServerResult result;
    TcpTransport tcp;
    if (!tcp.Accept(listener.Accept(), result.error)) return result;
    TlsTransport tls(tcp);
    result.accepted = tls.Accept(serverContext, TIMEOUT, result.error);
    if (!result.accepted) return result;
    while (result.received.size() < expectedBytes) {
      std::uint8_t buffer[4096];
      std::size_t bytesRead = 0;
      if (tls.Read(buffer, bytesRead, TIMEOUT, result.error) !=
          ReadOutcome::Data)
        return result;
      result.received.insert(result.received.end(), buffer, buffer + bytesRead);
    }
    tls.Write(result.received, TIMEOUT, result.error);
    tls.Close();
    return result;
  });
}

std::vector<std::uint8_t> ReadAll(Transport &transport, std::size_t count,
                                  std::string &error) {
  std::vector<std::uint8_t> out;
  while (out.size() < count) {
    std::uint8_t buffer[4096];
    std::size_t bytesRead = 0;
    if (transport.Read(buffer, bytesRead, TIMEOUT, error) != ReadOutcome::Data)
      break;
    out.insert(out.end(), buffer, buffer + bytesRead);
  }
  return out;
}

void Echoes(SslMode clientMode, const TlsMaterial &clientMaterial,
            std::size_t payloadSize) {
  TlsContext serverContext;
  std::string error;
  ASSERT_TRUE(serverContext.LoadServer(ServerMaterial(), error)) << error;
  TlsContext clientContext;
  ASSERT_TRUE(clientContext.LoadClient(clientMode, clientMaterial, error))
      << error;

  LoopbackListener listener;
  auto server = RunEchoServer(listener, serverContext, payloadSize);

  TcpTransport tcp;
  TlsTransport tls(tcp);
  ASSERT_TRUE(tls.Connect("127.0.0.1", listener.Port(), TIMEOUT, error))
      << error;
  EXPECT_FALSE(tls.Enabled());
  ASSERT_TRUE(tls.Handshake(clientContext, "127.0.0.1", TIMEOUT, error))
      << error;
  EXPECT_TRUE(tls.Enabled());
  EXPECT_FALSE(tls.Cipher().empty());
  EXPECT_EQ(tls.Version().substr(0, 4), "TLSv");

  std::vector<std::uint8_t> payload(payloadSize);
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<std::uint8_t>(i * 7 + 3);
  ASSERT_TRUE(tls.Write(payload, TIMEOUT, error)) << error;
  EXPECT_EQ(ReadAll(tls, payloadSize, error), payload) << error;

  // The server closed after echoing: the client sees the end of stream,
  // not an error.
  std::uint8_t buffer[16];
  std::size_t bytesRead = 0;
  EXPECT_EQ(tls.Read(buffer, bytesRead, TIMEOUT, error), ReadOutcome::Closed)
      << error;

  const ServerResult result = server.get();
  EXPECT_TRUE(result.accepted) << result.error;
  EXPECT_EQ(result.received, payload);
}

TEST(TlsCertificateGeneratorTest, GeneratesAPairTheContextLoads) {
  const GeneratedCertificates &certificates = Certificates();
  EXPECT_EQ(certificates.caCertPem.substr(0, 27),
            "-----BEGIN CERTIFICATE-----");
  EXPECT_EQ(certificates.serverCertPem.substr(0, 27),
            "-----BEGIN CERTIFICATE-----");
  EXPECT_EQ(certificates.caKeyPem.substr(0, 27), "-----BEGIN PRIVATE KEY-----");
  EXPECT_EQ(certificates.serverKeyPem.substr(0, 27),
            "-----BEGIN PRIVATE KEY-----");
  std::string error;
  EXPECT_TRUE(TlsContext::Check(ServerMaterial(), error)) << error;
  EXPECT_TRUE(TlsContext::Check(
      TlsMaterial{"", certificates.caCertPem, certificates.caKeyPem}, error))
      << error;
}

TEST(TlsContextTest, RejectsAKeyThatDoesNotMatchTheCertificate) {
  std::string error;
  EXPECT_FALSE(TlsContext::Check(
      TlsMaterial{"", Certificates().serverCertPem, Certificates().caKeyPem},
      error));
  EXPECT_EQ(error, "ssl_key does not match ssl_cert");
}

TEST(TlsContextTest, RejectsTextThatIsNotPem) {
  std::string error;
  EXPECT_FALSE(TlsContext::Check(
      TlsMaterial{"", "not a certificate", Certificates().serverKeyPem},
      error));
  EXPECT_EQ(error.substr(0, 9), "ssl_cert:") << error;
  EXPECT_FALSE(
      TlsContext::Check(TlsMaterial{"not a ca", Certificates().serverCertPem,
                                    Certificates().serverKeyPem},
                        error));
  EXPECT_EQ(error.substr(0, 7), "ssl_ca:") << error;
}

TEST(TlsContextTest, AServerNeedsBothCertificateAndKey) {
  TlsContext context;
  std::string error;
  EXPECT_FALSE(context.LoadServer(
      TlsMaterial{"", Certificates().serverCertPem, ""}, error));
  EXPECT_EQ(error, "a server needs both ssl_cert and ssl_key");
  EXPECT_FALSE(context.Loaded());
}

TEST(TlsContextTest, AClientVerifyingTheChainNeedsACa) {
  TlsContext context;
  std::string error;
  EXPECT_FALSE(context.LoadClient(SslMode::VerifyCa, TlsMaterial{}, error));
  EXPECT_NE(error.find("ssl_ca"), std::string::npos) << error;
  EXPECT_FALSE(context.LoadClient(SslMode::Disabled, TlsMaterial{}, error));
  EXPECT_TRUE(context.LoadClient(SslMode::Required, TlsMaterial{}, error))
      << error;
  EXPECT_EQ(context.mode(), SslMode::Required);
}

// Forwards to another transport, counting the writes.
class CountingTransport final : public Transport {
 public:
  explicit CountingTransport(Transport &inner) : m_inner(inner) {}
  bool Connect(const std::string &host, std::uint16_t port,
               std::chrono::milliseconds timeout, std::string &error) override {
    return m_inner.Connect(host, port, timeout, error);
  }
  ReadOutcome Read(std::span<std::uint8_t> buffer, std::size_t &bytesRead,
                   std::chrono::milliseconds timeout,
                   std::string &error) override {
    return m_inner.Read(buffer, bytesRead, timeout, error);
  }
  bool Write(std::span<const std::uint8_t> data,
             std::chrono::milliseconds timeout, std::string &error) override {
    ++writes;
    return m_inner.Write(data, timeout, error);
  }
  void Close() override { m_inner.Close(); }

  int writes = 0;

 private:
  Transport &m_inner;
};

TEST(TlsTransportTest, EachFullRecordGoesOutInOneWrite) {
  constexpr std::size_t RECORDS = 4;
  constexpr std::size_t PAYLOAD_SIZE = RECORDS * 16 * 1024;
  TlsContext serverContext;
  std::string error;
  ASSERT_TRUE(serverContext.LoadServer(ServerMaterial(), error)) << error;
  TlsContext clientContext;
  ASSERT_TRUE(clientContext.LoadClient(SslMode::Required, TlsMaterial{}, error))
      << error;
  LoopbackListener listener;
  auto server = RunEchoServer(listener, serverContext, PAYLOAD_SIZE);

  TcpTransport tcp;
  CountingTransport counting(tcp);
  TlsTransport tls(counting);
  ASSERT_TRUE(tls.Connect("127.0.0.1", listener.Port(), TIMEOUT, error))
      << error;
  ASSERT_TRUE(tls.Handshake(clientContext, "127.0.0.1", TIMEOUT, error))
      << error;

  const std::vector<std::uint8_t> payload(PAYLOAD_SIZE, 0x5A);
  counting.writes = 0;
  ASSERT_TRUE(tls.Write(payload, TIMEOUT, error)) << error;
  EXPECT_EQ(counting.writes, static_cast<int>(RECORDS));
  EXPECT_EQ(ReadAll(tls, PAYLOAD_SIZE, error), payload) << error;
  EXPECT_TRUE(server.get().accepted);
}

TEST(TlsTransportTest, EchoesASmallPayloadUnderRequired) {
  Echoes(SslMode::Required, TlsMaterial{}, 100);
}

TEST(TlsTransportTest, EchoesAPayloadLargerThanOneRecordUnderRequired) {
  Echoes(SslMode::Required, TlsMaterial{}, 100 * 1024);
}

TEST(TlsTransportTest, VerifyCaAcceptsTheCaThatSignedTheServer) {
  Echoes(SslMode::VerifyCa, TlsMaterial{Certificates().caCertPem, "", ""}, 100);
}

TEST(TlsTransportTest, VerifyCaRejectsAServerSignedByAnotherCa) {
  GeneratedCertificates other;
  std::string error;
  ASSERT_TRUE(TlsCertificateGenerator::Generate("other", other, error))
      << error;

  TlsContext serverContext;
  ASSERT_TRUE(serverContext.LoadServer(ServerMaterial(), error)) << error;
  TlsContext clientContext;
  ASSERT_TRUE(clientContext.LoadClient(
      SslMode::VerifyCa, TlsMaterial{other.caCertPem, "", ""}, error))
      << error;

  LoopbackListener listener;
  auto server = RunEchoServer(listener, serverContext, 0);
  TcpTransport tcp;
  TlsTransport tls(tcp);
  ASSERT_TRUE(tls.Connect("127.0.0.1", listener.Port(), TIMEOUT, error))
      << error;
  EXPECT_FALSE(tls.Handshake(clientContext, "127.0.0.1", TIMEOUT, error));
  EXPECT_NE(error.find("certificate verification failed"), std::string::npos)
      << error;
  EXPECT_FALSE(tls.Enabled());
  tls.Close();
  const ServerResult result = server.get();
  EXPECT_FALSE(result.accepted);
}

TEST(TlsTransportTest, VerifyIdentityRejectsACertificateWithoutTheHost) {
  std::string error;
  TlsContext serverContext;
  ASSERT_TRUE(serverContext.LoadServer(ServerMaterial(), error)) << error;
  TlsContext clientContext;
  ASSERT_TRUE(clientContext.LoadClient(
      SslMode::VerifyIdentity, TlsMaterial{Certificates().caCertPem, "", ""},
      error))
      << error;

  LoopbackListener listener;
  auto server = RunEchoServer(listener, serverContext, 0);
  TcpTransport tcp;
  TlsTransport tls(tcp);
  ASSERT_TRUE(tls.Connect("127.0.0.1", listener.Port(), TIMEOUT, error))
      << error;
  EXPECT_FALSE(tls.Handshake(clientContext, "127.0.0.1", TIMEOUT, error));
  EXPECT_NE(error.find("certificate verification failed"), std::string::npos)
      << error;
  tls.Close();
  (void)server.get();
}

TEST(TlsTransportTest, AcceptFeedsBytesTheCallerAlreadyReadOffTheSocket) {
  std::string error;
  TlsContext serverContext;
  ASSERT_TRUE(serverContext.LoadServer(ServerMaterial(), error)) << error;
  TlsContext clientContext;
  ASSERT_TRUE(clientContext.LoadClient(SslMode::Required, TlsMaterial{}, error))
      << error;

  LoopbackListener listener;
  auto server = std::async(std::launch::async, [&] {
    ServerResult result;
    TcpTransport tcp;
    if (!tcp.Accept(listener.Accept(), result.error)) return result;
    // Take the first bytes of the client hello off the socket the way a
    // read-ahead PacketChannel would, and hand them to Accept().
    std::uint8_t first[5];
    std::size_t bytesRead = 0;
    while (bytesRead < sizeof(first)) {
      std::size_t got = 0;
      if (tcp.Read(std::span<std::uint8_t>(first + bytesRead,
                                           sizeof(first) - bytesRead),
                   got, TIMEOUT, result.error) != ReadOutcome::Data)
        return result;
      bytesRead += got;
    }
    TlsTransport tls(tcp);
    result.accepted = tls.Accept(serverContext, TIMEOUT, result.error,
                                 std::span<const std::uint8_t>(first));
    if (!result.accepted) return result;
    std::uint8_t buffer[16];
    if (tls.Read(buffer, bytesRead, TIMEOUT, result.error) != ReadOutcome::Data)
      return result;
    result.received.assign(buffer, buffer + bytesRead);
    tls.Close();
    return result;
  });

  TcpTransport tcp;
  TlsTransport tls(tcp);
  ASSERT_TRUE(tls.Connect("127.0.0.1", listener.Port(), TIMEOUT, error))
      << error;
  ASSERT_TRUE(tls.Handshake(clientContext, "127.0.0.1", TIMEOUT, error))
      << error;
  const std::vector<std::uint8_t> payload{'h', 'i'};
  ASSERT_TRUE(tls.Write(payload, TIMEOUT, error)) << error;
  const ServerResult result = server.get();
  EXPECT_TRUE(result.accepted) << result.error;
  EXPECT_EQ(result.received, payload);
}

TEST(TlsTransportTest, HandshakeFailsWhenThePeerClosesInsteadOfAnswering) {
  std::string error;
  TlsContext clientContext;
  ASSERT_TRUE(clientContext.LoadClient(SslMode::Required, TlsMaterial{}, error))
      << error;
  LoopbackListener listener;
  auto server = std::async(std::launch::async, [&] {
    const int fd = listener.Accept();
    std::uint8_t buffer[64];
    recv(fd, buffer, sizeof(buffer), 0);
    close(fd);
  });
  TcpTransport tcp;
  TlsTransport tls(tcp);
  ASSERT_TRUE(tls.Connect("127.0.0.1", listener.Port(), TIMEOUT, error))
      << error;
  EXPECT_FALSE(tls.Handshake(clientContext, "127.0.0.1", TIMEOUT, error));
  EXPECT_EQ(error.substr(0, 14), "TLS handshake:") << error;
  EXPECT_FALSE(tls.Enabled());
  server.get();
}

}  // namespace
}  // namespace binlog_streamer
