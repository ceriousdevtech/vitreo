#include <gtest/gtest.h>
#include "ipc/startup_message.hpp"
using namespace vitreo;

TEST(WinExecStartup, NotificationIsPersistentAndLateNotificationCannotRecreateEvent) {
    std::string name;
    {
        ipc::StartupMessageEvent event;
        ASSERT_TRUE(event.valid());
        name = event.name();
        EXPECT_TRUE(ipc::StartupMessageEvent::notify(name));
        EXPECT_EQ(event.wait(0), 0u);
        EXPECT_EQ(event.wait(0), 0u);
    }
    EXPECT_FALSE(ipc::StartupMessageEvent::notify(name));
}

