#include <gtest/gtest.h>
#include <peersync/gui_logic.h>
#include <fstream>
#include <filesystem>

namespace {

TEST(GuiLogicTest, FormatElapsedTime) {
    EXPECT_EQ(peersync::formatElapsedTime(0), "< 1 min ago");
    EXPECT_EQ(peersync::formatElapsedTime(45), "< 1 min ago");
    EXPECT_EQ(peersync::formatElapsedTime(60), "1 min ago");
    EXPECT_EQ(peersync::formatElapsedTime(180), "3 mins ago");
    EXPECT_EQ(peersync::formatElapsedTime(3600), "1 hour ago");
    EXPECT_EQ(peersync::formatElapsedTime(7200), "2 hours ago");
    EXPECT_EQ(peersync::formatElapsedTime(86400), "1 day ago");
    EXPECT_EQ(peersync::formatElapsedTime(172800), "2 days ago");
}

TEST(GuiLogicTest, FormatTransferStatusLabel) {
    EXPECT_EQ(peersync::formatTransferStatusLabel(peersync::TransferStatus::Completed), "Completed");
    EXPECT_EQ(peersync::formatTransferStatusLabel(peersync::TransferStatus::Interrupted), "Interrupted");
    EXPECT_EQ(peersync::formatTransferStatusLabel(peersync::TransferStatus::Resumed), "Resumed");
    EXPECT_EQ(peersync::formatTransferStatusLabel(peersync::TransferStatus::Failed), "Failed");
    EXPECT_EQ(peersync::formatTransferStatusLabel(peersync::TransferStatus::InProgress), "In Progress");
}

TEST(GuiLogicTest, FindRelevantResumableSession) {
    std::vector<peersync::TransferHistoryEntry> history;
    EXPECT_EQ(peersync::findRelevantResumableSession(history, "peer1", ""), nullptr);

    // Add completed session (should be ignored)
    peersync::TransferHistoryEntry e1;
    e1.peerName = "peer1";
    e1.path = "/data/file1.txt";
    e1.status = peersync::TransferStatus::Completed;
    e1.timestampSec = 100;
    history.push_back(e1);
    EXPECT_EQ(peersync::findRelevantResumableSession(history, "peer1", ""), nullptr);

    // Add interrupted session for different peer (should be ignored when searching for peer1)
    peersync::TransferHistoryEntry e2;
    e2.peerName = "peer2";
    e2.path = "/data/file2.txt";
    e2.status = peersync::TransferStatus::Interrupted;
    e2.timestampSec = 200;
    history.push_back(e2);
    EXPECT_EQ(peersync::findRelevantResumableSession(history, "peer1", ""), nullptr);

    // Add interrupted session for peer1
    peersync::TransferHistoryEntry e3;
    e3.peerName = "peer1";
    e3.path = "/data/file3.txt";
    e3.status = peersync::TransferStatus::Interrupted;
    e3.timestampSec = 300;
    history.push_back(e3);
    const auto* res1 = peersync::findRelevantResumableSession(history, "peer1", "");
    ASSERT_NE(res1, nullptr);
    EXPECT_EQ(res1->path, "/data/file3.txt");

    // Add a more recent interrupted session for peer1 with different path
    peersync::TransferHistoryEntry e4;
    e4.peerName = "peer1";
    e4.path = "/data/file4.txt";
    e4.status = peersync::TransferStatus::Interrupted;
    e4.timestampSec = 400;
    history.push_back(e4);

    // Searching without specific path returns most recent (file4)
    const auto* res2 = peersync::findRelevantResumableSession(history, "peer1", "");
    ASSERT_NE(res2, nullptr);
    EXPECT_EQ(res2->path, "/data/file4.txt");

    // Searching with specific path returns file3
    const auto* res3 = peersync::findRelevantResumableSession(history, "peer1", "/data/file3.txt");
    ASSERT_NE(res3, nullptr);
    EXPECT_EQ(res3->path, "/data/file3.txt");
}

class GuiLogicDiskTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir = std::filesystem::temp_directory_path() / "gui_logic_test_dir";
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
        std::filesystem::create_directories(testDir, ec);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }
    std::filesystem::path testDir;
};

TEST_F(GuiLogicDiskTest, DetectJournalSingleFile) {
    std::filesystem::path file = testDir / "test.dat";
    std::filesystem::path jPath = file.string() + ".peersync-journal";
    std::filesystem::path tPath = file.string() + ".peersync-tmp";

    uint64_t ba = 0, es = 0;
    EXPECT_FALSE(peersync::detectJournalForPath(file.string(), false, ba, es));

    // Create journal and tmp file
    {
        std::ofstream ofs(jPath);
        ofs << "path=test.dat\nexpected_size=1000\nbytes_applied=450\n";
    }
    {
        std::ofstream ofs(tPath);
        ofs << "dummy tmp content";
    }

    EXPECT_TRUE(peersync::detectJournalForPath(file.string(), false, ba, es));
    EXPECT_EQ(ba, 450);
    EXPECT_EQ(es, 1000);
}

TEST_F(GuiLogicDiskTest, DetectJournalDirectory) {
    std::filesystem::path subDir = testDir / "sub";
    std::error_code ec;
    std::filesystem::create_directories(subDir, ec);

    std::filesystem::path file1 = subDir / "part1.bin";
    std::filesystem::path jPath1 = file1.string() + ".peersync-journal";
    std::filesystem::path tPath1 = file1.string() + ".peersync-tmp";

    {
        std::ofstream ofs(jPath1);
        ofs << "path=sub/part1.bin\nexpected_size=2000\nbytes_applied=800\n";
    }
    {
        std::ofstream ofs(tPath1);
        ofs << "dummy tmp";
    }

    uint64_t ba = 0, es = 0;
    EXPECT_TRUE(peersync::detectJournalForPath(testDir.string(), true, ba, es));
    EXPECT_EQ(ba, 800);
    EXPECT_EQ(es, 2000);
}

TEST(GuiLogicTest, TransitionScreen) {
    using peersync::AppScreen;
    using peersync::GuiEvent;
    
    // Discovery transitions
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Discovery, GuiEvent::StartSetupInitiator), AppScreen::Setup);
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Discovery, GuiEvent::StartSetupResponder), AppScreen::Setup);
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Discovery, GuiEvent::ResumeFromHistory), AppScreen::Setup);
    // Invalid event from Discovery should do nothing
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Discovery, GuiEvent::CancelSetup), AppScreen::Discovery);
    
    // Setup transitions
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Setup, GuiEvent::CancelSetup), AppScreen::Discovery);
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Setup, GuiEvent::StartTransfer), AppScreen::Transferring);
    
    // Transferring transitions
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Transferring, GuiEvent::CancelTransfer), AppScreen::Discovery);
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Transferring, GuiEvent::TransferFinished), AppScreen::Complete);
    
    // Complete transitions
    EXPECT_EQ(peersync::transitionScreen(AppScreen::Complete, GuiEvent::ReturnToHome), AppScreen::Discovery);
}

TEST(GuiLogicTest, GetSetupFromHistory) {
    // Initiator history entry (has peerIp)
    peersync::TransferHistoryEntry initEntry;
    initEntry.peerIp = "192.168.1.100";
    initEntry.peerPort = 5566;
    initEntry.path = "/path/to/send";
    initEntry.isFolder = true;
    
    auto initSetup = peersync::getSetupFromHistory(initEntry);
    EXPECT_TRUE(initSetup.isInitiator);
    EXPECT_EQ(initSetup.targetIp, "192.168.1.100");
    EXPECT_EQ(initSetup.targetPort, 5566);
    EXPECT_EQ(initSetup.path, "/path/to/send");
    EXPECT_TRUE(initSetup.isFolder);
    
    // Responder history entry (empty peerIp)
    peersync::TransferHistoryEntry respEntry;
    respEntry.peerIp = "";
    respEntry.peerPort = 5566;
    respEntry.path = "/path/to/receive";
    respEntry.isFolder = false;
    
    auto respSetup = peersync::getSetupFromHistory(respEntry);
    EXPECT_FALSE(respSetup.isInitiator);
    EXPECT_EQ(respSetup.targetIp, "");
    EXPECT_EQ(respSetup.targetPort, 5566);
    EXPECT_EQ(respSetup.path, "/path/to/receive");
    EXPECT_FALSE(respSetup.isFolder);
}

} // anonymous namespace
