#pragma once

#include "api/twitch.hpp"

#include <borealis.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifdef BOREALIS_USE_STD_THREAD
#include <thread>
#else
#include <pthread.h>
#endif

namespace twitch {

enum class ChatMode {
    Off = 0,
    RightPanel = 1,
    Overlay = 2,
};

enum class ChatParticipation {
    ReadOnly = 0,
    Interactive = 1,
};

enum class ChatComposerMode {
    NintendoKeyboard = 0,
    TwiNXComposer = 1,
};

enum class ChatEmoteMode {
    Static = 0,
    Animated = 1,
};

enum class ChatOverlaySize {
    FullHeight = 0,
    Compact = 1,
};

enum class ChatOverlayPosition {
    TopRight = 0,
    TopLeft = 1,
    BottomRight = 2,
    BottomLeft = 3,
};

enum class ChatFragmentType {
    Text = 0,
    Emote = 1,
    Mention = 2,
    Cheermote = 3,
};

struct ChatPreferences {
    ChatMode mode = ChatMode::RightPanel;
    ChatParticipation participation = ChatParticipation::ReadOnly;
    ChatComposerMode composerMode = ChatComposerMode::TwiNXComposer;
    ChatEmoteMode emoteMode = ChatEmoteMode::Static;
    ChatOverlaySize overlaySize = ChatOverlaySize::FullHeight;
    ChatOverlayPosition overlayPosition = ChatOverlayPosition::TopRight;
    int fontSize = 16;
    int opacity = 82;
    int dockedWidth = 330;
    int overlayWidth = 360;
    bool timestamps = false;
};

struct ChatFragment {
    ChatFragmentType type = ChatFragmentType::Text;
    std::string text;
    std::string emoteId;
    std::string emoteUrl;
    std::string animatedEmoteUrl;
};

struct ChatBadge {
    std::string setId;
    std::string id;
    std::string info;
    std::string imageUrl;
};

struct ChatMessage {
    std::string id;
    std::string userName;
    std::string text;
    std::string color;
    std::string timestamp;
    std::vector<ChatBadge> badges;
    std::vector<ChatFragment> fragments;
};

using ChatMessageCallback = std::function<void(ChatMessage)>;
using ChatStatusCallback = std::function<void(const std::string&)>;
using ChatSendCallback = std::function<void(const std::string&)>;

struct UserEmote {
    std::string id;
    std::string name;
    std::string ownerId;
    std::string emoteType;
    std::string imageUrl;
    bool channelEmote = false;
};

struct UserEmoteCatalogue {
    std::vector<UserEmote> emotes;
    bool channelSubscribed = false;
    bool subscriptionPermissionGranted = false;
};

using UserEmotesCallback =
    std::function<void(UserEmoteCatalogue)>;

ChatPreferences loadChatPreferences();
bool saveChatPreferences(const ChatPreferences& preferences);
brls::Event<ChatPreferences>* chatPreferencesEvent();

bool hasChatWriteScope();
bool hasUserEmotesScope();

void loadUserEmotesAsync(
    const std::string& channel,
    UserEmotesCallback success,
    ErrorCallback failure);

std::vector<UserEmote> loadRecentEmotes();
void rememberRecentEmote(const UserEmote& emote);

void sendChatMessageAsync(
    const std::string& channel,
    const std::string& message,
    ChatSendCallback success,
    ErrorCallback failure);

class ChatClient {
public:
    ChatClient(
        std::string channel,
        ChatMessageCallback message,
        ChatStatusCallback status);
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

    void stop();

private:
    static void* threadEntry(void* context);
    void run();

    std::string channel;
    ChatMessageCallback messageCallback;
    ChatStatusCallback statusCallback;
    std::atomic_bool stopRequested{false};

#ifdef BOREALIS_USE_STD_THREAD
    std::shared_ptr<std::thread> thread;
#else
    pthread_t thread{};
    bool threadStarted = false;
#endif
};

}  // namespace twitch
