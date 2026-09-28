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

#include "net/cTcpTransport.hpp"
#include "net/cWakeupPipe.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

namespace binlog_streamer {
namespace {

// No SA_RESTART: makes poll()/recv() return EINTR instead of silently
// resuming. sigaction(), not signal(), to make sa_flags explicit.
void NoopSignalHandler(int) {}

void InstallSignalHandlerWithoutRestart(int signalNumber) {
  struct sigaction action{};
  action.sa_handler = NoopSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(signalNumber, &action, nullptr);
}

// Loopback TCP listener on an OS-assigned port, for tests needing a real
// socket pair (EINTR/SIGPIPE are properties of the real syscalls).
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

// Sends RST instead of FIN: a plain close() lets the peer's writes succeed
// into its send buffer until it fills, not the fast failure the SIGPIPE
// test below needs. SO_LINGER{1,0} forces a reset.
void CloseWithReset(int fd) {
  struct linger resetOnClose{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &resetOnClose, sizeof(resetOnClose));
  close(fd);
}

TEST(TcpTransportTest,
     ReadSurvivesEintrWithoutStopRequestedAndReturnsDataOnceSent) {
  InstallSignalHandlerWithoutRestart(SIGUSR1);
  LoopbackListener listener;
  TcpTransport transport;
  std::string connectError;
  ASSERT_TRUE(transport.Connect("127.0.0.1", listener.Port(),
                                std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocket = listener.Accept();
  ASSERT_GE(acceptedSocket, 0);

  const pthread_t readingThread = pthread_self();
  std::thread interrupter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pthread_kill(readingThread, SIGUSR1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint8_t payload[] = {0x2A};
    send(acceptedSocket, payload, sizeof(payload), 0);
  });

  std::uint8_t buffer[8] = {};
  std::size_t bytesRead = 0;
  std::string error;
  const ReadOutcome outcome =
      transport.Read(buffer, bytesRead, std::chrono::milliseconds(5000), error);
  interrupter.join();
  close(acceptedSocket);

  EXPECT_EQ(outcome, ReadOutcome::Data) << error;
  ASSERT_EQ(bytesRead, 1u);
  EXPECT_EQ(buffer[0], 0x2A);
}

TEST(TcpTransportTest, ReadReturnsInterruptedWhenStopIsRequestedDuringEintr) {
  InstallSignalHandlerWithoutRestart(SIGUSR1);
  LoopbackListener listener;
  std::atomic<bool> stopRequested{false};
  TcpTransport transport(&stopRequested);
  std::string connectError;
  ASSERT_TRUE(transport.Connect("127.0.0.1", listener.Port(),
                                std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocket = listener.Accept();
  ASSERT_GE(acceptedSocket, 0);

  const pthread_t readingThread = pthread_self();
  std::thread interrupter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopRequested = true;
    pthread_kill(readingThread, SIGUSR1);
  });

  std::uint8_t buffer[8] = {};
  std::size_t bytesRead = 0;
  std::string error;
  // No one ever sends anything: the only way this Read() returns before
  // its 5s timeout is by noticing the stop request after EINTR.
  const ReadOutcome outcome =
      transport.Read(buffer, bytesRead, std::chrono::milliseconds(5000), error);
  interrupter.join();
  close(acceptedSocket);

  EXPECT_EQ(outcome, ReadOutcome::Interrupted);
  EXPECT_EQ(bytesRead, 0u);
}

// The next two tests call WakeupPipe::Wake() directly rather than raising a
// signal: they exercise the self-pipe mechanism itself, not signal delivery
// (already covered above).

TEST(
    TcpTransportTest,
    ReadReturnsInterruptedImmediatelyWhenTheWakeupPipeIsAlreadySignaledBeforeRead) {
  LoopbackListener listener;
  WakeupPipe wakeupPipe;
  std::string wakeupError;
  ASSERT_TRUE(wakeupPipe.Open(wakeupError)) << wakeupError;
  std::atomic<bool> stopRequested{false};
  TcpTransport transport(&stopRequested, &wakeupPipe);
  std::string connectError;
  ASSERT_TRUE(transport.Connect("127.0.0.1", listener.Port(),
                                std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocket = listener.Accept();
  ASSERT_GE(acceptedSocket, 0);

  wakeupPipe.Wake();

  std::uint8_t buffer[8] = {};
  std::size_t bytesRead = 0;
  std::string error;
  const auto start = std::chrono::steady_clock::now();
  // No one ever sends anything: the only way this returns before its 5s
  // timeout is poll() noticing the wakeup pipe's read end.
  const ReadOutcome outcome =
      transport.Read(buffer, bytesRead, std::chrono::milliseconds(5000), error);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  close(acceptedSocket);

  EXPECT_EQ(outcome, ReadOutcome::Interrupted);
  EXPECT_EQ(bytesRead, 0u);
  EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

TEST(
    TcpTransportTest,
    ReadReturnsInterruptedWhenWokenBetweenTwoCallsWithNoDataAndNoSignalInvolved) {
  // The scenario the self-pipe closes: a signal handled between two Read()
  // calls (not blocked in any syscall) sets nothing poll() could get EINTR
  // from; only the already-readable pipe makes the second Read() notice it.
  LoopbackListener listener;
  WakeupPipe wakeupPipe;
  std::string wakeupError;
  ASSERT_TRUE(wakeupPipe.Open(wakeupError)) << wakeupError;
  std::atomic<bool> stopRequested{false};
  TcpTransport transport(&stopRequested, &wakeupPipe);
  std::string connectError;
  ASSERT_TRUE(transport.Connect("127.0.0.1", listener.Port(),
                                std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocket = listener.Accept();
  ASSERT_GE(acceptedSocket, 0);

  const std::uint8_t payload[] = {0x2A};
  ASSERT_EQ(send(acceptedSocket, payload, sizeof(payload), 0),
            static_cast<ssize_t>(sizeof(payload)));

  std::uint8_t buffer[8] = {};
  std::size_t bytesRead = 0;
  std::string error;
  ASSERT_EQ(
      transport.Read(buffer, bytesRead, std::chrono::milliseconds(2000), error),
      ReadOutcome::Data);
  ASSERT_EQ(bytesRead, 1u);

  wakeupPipe.Wake();

  bytesRead = 0;
  const auto start = std::chrono::steady_clock::now();
  const ReadOutcome outcome =
      transport.Read(buffer, bytesRead, std::chrono::milliseconds(5000), error);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  close(acceptedSocket);

  EXPECT_EQ(outcome, ReadOutcome::Interrupted);
  EXPECT_EQ(bytesRead, 0u);
  EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// The race two threads sharing one WakeupPipe must survive: draining the
// pipe on one thread's Read() could leave another already-woken thread
// finding it empty and waiting out its timeout instead of returning
// Interrupted.
TEST(TcpTransportTest, WakeInterruptsTwoBlockedReadsOnOneSharedWakeupPipe) {
  LoopbackListener listener;
  WakeupPipe wakeupPipe;
  std::string wakeupError;
  ASSERT_TRUE(wakeupPipe.Open(wakeupError)) << wakeupError;
  std::atomic<bool> stopRequested{false};

  TcpTransport transportA(&stopRequested, &wakeupPipe);
  TcpTransport transportB(&stopRequested, &wakeupPipe);
  std::string connectError;
  ASSERT_TRUE(transportA.Connect("127.0.0.1", listener.Port(),
                                 std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocketA = listener.Accept();
  ASSERT_GE(acceptedSocketA, 0);
  ASSERT_TRUE(transportB.Connect("127.0.0.1", listener.Port(),
                                 std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocketB = listener.Accept();
  ASSERT_GE(acceptedSocketB, 0);

  ReadOutcome outcomeA = ReadOutcome::Failed;
  ReadOutcome outcomeB = ReadOutcome::Failed;
  std::chrono::steady_clock::duration elapsedA{};
  std::chrono::steady_clock::duration elapsedB{};
  // No one ever sends anything on either socket: the only way either
  // Read() below returns before its 5s timeout is poll() noticing the
  // shared wakeup pipe's read end.
  std::thread readerA([&] {
    std::uint8_t buffer[8] = {};
    std::size_t bytesRead = 0;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    outcomeA = transportA.Read(buffer, bytesRead,
                               std::chrono::milliseconds(5000), error);
    elapsedA = std::chrono::steady_clock::now() - start;
  });
  std::thread readerB([&] {
    std::uint8_t buffer[8] = {};
    std::size_t bytesRead = 0;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    outcomeB = transportB.Read(buffer, bytesRead,
                               std::chrono::milliseconds(5000), error);
    elapsedB = std::chrono::steady_clock::now() - start;
  });

  // Not load-bearing for correctness (a reader not yet in poll() when
  // Wake() runs is the other half of the race, covered via the queued
  // byte) - only makes this test exercise two simultaneously blocked readers.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  wakeupPipe.Wake();

  readerA.join();
  readerB.join();
  close(acceptedSocketA);
  close(acceptedSocketB);

  EXPECT_EQ(outcomeA, ReadOutcome::Interrupted);
  EXPECT_EQ(outcomeB, ReadOutcome::Interrupted);
  EXPECT_LT(elapsedA, std::chrono::milliseconds(2000));
  EXPECT_LT(elapsedB, std::chrono::milliseconds(2000));
}

TEST(TcpTransportTest, AcceptWrapsAnAlreadyOpenSocketForReadingAndWriting) {
  LoopbackListener listener;
  TcpTransport clientSide;
  std::string connectError;
  ASSERT_TRUE(clientSide.Connect("127.0.0.1", listener.Port(),
                                 std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocket = listener.Accept();
  ASSERT_GE(acceptedSocket, 0);

  TcpTransport serverSide;
  std::string acceptError;
  ASSERT_TRUE(serverSide.Accept(acceptedSocket, acceptError)) << acceptError;

  const std::uint8_t payload[] = {0x2A};
  std::string writeError;
  ASSERT_TRUE(
      serverSide.Write(payload, std::chrono::milliseconds(2000), writeError))
      << writeError;

  std::uint8_t buffer[8] = {};
  std::size_t bytesRead = 0;
  std::string readError;
  const ReadOutcome outcome = clientSide.Read(
      buffer, bytesRead, std::chrono::milliseconds(2000), readError);
  EXPECT_EQ(outcome, ReadOutcome::Data) << readError;
  ASSERT_EQ(bytesRead, 1u);
  EXPECT_EQ(buffer[0], 0x2A);
}

TEST(TcpTransportTest, WriteAfterPeerResetFailsWithoutKillingTheProcess) {
  LoopbackListener listener;
  TcpTransport transport;
  std::string connectError;
  ASSERT_TRUE(transport.Connect("127.0.0.1", listener.Port(),
                                std::chrono::milliseconds(2000), connectError))
      << connectError;
  const int acceptedSocket = listener.Accept();
  ASSERT_GE(acceptedSocket, 0);
  CloseWithReset(acceptedSocket);

  std::this_thread::sleep_for(
      std::chrono::milliseconds(50));  // let the RST arrive over loopback

  // One write may land in the kernel's send buffer before the RST is
  // noticed; a bounded number of large writes should observe it -
  // exhausting the budget without a failure is itself the test failure below.
  const std::vector<std::uint8_t> chunk(1024 * 1024, 0x55);
  bool sawFailure = false;
  std::string error;
  for (int attempt = 0; attempt < 20 && !sawFailure; ++attempt) {
    if (!transport.Write(chunk, std::chrono::milliseconds(2000), error))
      sawFailure = true;
  }

  // Reaching this line at all (rather than the process dying to an
  // unhandled SIGPIPE) is itself part of what this test proves.
  EXPECT_TRUE(sawFailure) << "expected Write() to eventually fail after the "
                             "peer reset the connection";
}

// poll() re-checks its descriptors after waking, so a reader that emptied
// the pipe could leave others asleep: the pending wakeup must survive an
// interrupted Read().
TEST(TcpTransportTest, AnInterruptedReadLeavesTheWakeupPendingForOtherReaders) {
  int sockets[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
  WakeupPipe wakeupPipe;
  std::string error;
  ASSERT_TRUE(wakeupPipe.Open(error)) << error;
  std::atomic<bool> stopRequested{
      false};  // left unset: only the pipe reports the stop here
  TcpTransport transport(&stopRequested, &wakeupPipe);
  ASSERT_TRUE(transport.Accept(sockets[0], error)) << error;

  wakeupPipe.Wake();
  std::uint8_t byte[1];
  std::size_t bytesRead = 0;
  EXPECT_EQ(
      transport.Read(byte, bytesRead, std::chrono::milliseconds(1000), error),
      ReadOutcome::Interrupted);

  struct pollfd pending{wakeupPipe.ReadFd(), POLLIN, 0};
  EXPECT_EQ(poll(&pending, 1, 0), 1);

  close(sockets[1]);
}

}  // namespace
}  // namespace binlog_streamer
