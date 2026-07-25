#include <gtest/gtest.h>
#include <peersync/pairing.h>
#include <peersync/socket.h>
#include <peersync/message_framing.h>
#include <peersync/exceptions.h>
#include <thread>
#include <string>
#include <vector>

namespace {

void executeHandshake(peersync::TcpSocket& sock,
                      peersync::PairingRole role,
                      const std::string& pin,
                      bool& outSuccess,
                      std::vector<uint8_t>& outSessionKey,
                      std::string& outErrorMessage) {
    peersync::PairingSession session(role, pin);
    if (role == peersync::PairingRole::Initiator) {
        session.start();
    }

    while (!session.isFinished()) {
        while (session.hasOutgoingMessage()) {
            auto msg = session.popOutgoingMessage();
            peersync::sendFramedMessage(sock, msg);
        }
        if (!session.isFinished()) {
            try {
                auto incoming = peersync::recvFramedMessage(sock);
                session.processMessage(incoming);
            } catch (const std::exception& e) {
                outSuccess = false;
                outErrorMessage = e.what();
                return;
            }
        }
    }

    // Flush any terminal outgoing messages (e.g. final PairResult from Initiator to Responder)
    while (session.hasOutgoingMessage()) {
        auto msg = session.popOutgoingMessage();
        peersync::sendFramedMessage(sock, msg);
    }

    outSuccess = session.isAuthenticated();
    outSessionKey = session.getSessionKey();
    outErrorMessage = session.getErrorMessage();
}

} // anonymous namespace

TEST(PairingIntegrationTest, HandshakeSuccessOverLoopbackSockets) {
    peersync::PairingSession::clearSeenNonces();

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread serverAcceptThread([&]() {
        acceptedSocket = server.accept();
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (serverAcceptThread.joinable()) {
        serverAcceptThread.join();
    }

    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    bool clientSuccess = false;
    std::vector<uint8_t> clientKey;
    std::string clientError;

    bool serverSuccess = false;
    std::vector<uint8_t> serverKey;
    std::string serverError;

    std::string pin = "849201";

    std::thread serverHandshakeThread([&]() {
        executeHandshake(acceptedSocket, peersync::PairingRole::Responder, pin, serverSuccess, serverKey, serverError);
    });

    executeHandshake(client, peersync::PairingRole::Initiator, pin, clientSuccess, clientKey, clientError);

    if (serverHandshakeThread.joinable()) {
        serverHandshakeThread.join();
    }

    EXPECT_TRUE(clientSuccess) << "Client handshake failed: " << clientError;
    EXPECT_TRUE(serverSuccess) << "Server handshake failed: " << serverError;
    EXPECT_FALSE(clientKey.empty());
    EXPECT_FALSE(serverKey.empty());
    EXPECT_EQ(clientKey, serverKey) << "Derived session keys must match over real socket handshake";
}

TEST(PairingIntegrationTest, HandshakeFailureWrongPinOverLoopbackSockets) {
    peersync::PairingSession::clearSeenNonces();

    peersync::TcpSocket server = peersync::TcpSocket::listen(0, "127.0.0.1");
    uint16_t port = server.getBoundPort();

    peersync::TcpSocket acceptedSocket;
    std::thread serverAcceptThread([&]() {
        acceptedSocket = server.accept();
    });

    peersync::TcpSocket client = peersync::TcpSocket::connect("127.0.0.1", port);
    if (serverAcceptThread.joinable()) {
        serverAcceptThread.join();
    }

    ASSERT_TRUE(client.isValid());
    ASSERT_TRUE(acceptedSocket.isValid());

    bool clientSuccess = true;
    std::vector<uint8_t> clientKey;
    std::string clientError;

    bool serverSuccess = true;
    std::vector<uint8_t> serverKey;
    std::string serverError;

    std::string initPin = "111111";
    std::string respPin = "222222"; // Mismatched PIN

    std::thread serverHandshakeThread([&]() {
        executeHandshake(acceptedSocket, peersync::PairingRole::Responder, respPin, serverSuccess, serverKey, serverError);
    });

    executeHandshake(client, peersync::PairingRole::Initiator, initPin, clientSuccess, clientKey, clientError);

    if (serverHandshakeThread.joinable()) {
        serverHandshakeThread.join();
    }

    EXPECT_FALSE(clientSuccess) << "Client must fail handshake with mismatched PIN";
    EXPECT_FALSE(serverSuccess) << "Server must fail handshake with mismatched PIN";
    EXPECT_TRUE(clientKey.empty()) << "No session key must be exposed on failed handshake";
    EXPECT_TRUE(serverKey.empty()) << "No session key must be exposed on failed handshake";
    EXPECT_NE(clientError.find("PIN verification failed"), std::string::npos);
}
