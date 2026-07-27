#include <cstddef>
#include <cstdint>
#include <vector>
#include <exception>
#include <peersync/protocol.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) {
        return 0;
    }

    std::vector<uint8_t> payload(data, data + size);

    try {
        peersync::MessageType type = peersync::getMessageType(payload);
        (void)type;
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeHelloMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializePairChallengeMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializePairResponseMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializePairResultMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeManifestRequestMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeManifestResponseMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeDeltaInstructionsMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeBlockDataMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeTransferAckMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeResumeRequestMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeResumeResponseMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeTransferCompleteMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeErrorMessageMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeDirectoryManifestRequestMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    try {
        auto msg = peersync::deserializeDirectoryManifestResponseMessage(payload);
        (void)peersync::serializeMessage(msg);
    } catch (const std::exception&) {}

    return 0;
}
