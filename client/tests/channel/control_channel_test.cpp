#include "channel/control_channel.h"

#include <gtest/gtest.h>

// client/control-channel Remote Session Revocation: only the control plane's own refusal locks the installation.
TEST(ControlChannelLock, ControlPlaneRevocationLocks) {
    EXPECT_TRUE(wb::locks_installation("DENIED"));
    EXPECT_TRUE(wb::locks_installation("denied"));
    EXPECT_TRUE(wb::locks_installation("revoked"));
}

TEST(ControlChannelLock, NetworkRefusalDoesNotLock) {
    // A 403 with an empty or foreign body (proxy, firewall, API gateway) arrives as "http_403".
    EXPECT_FALSE(wb::locks_installation("http_403"));
    EXPECT_FALSE(wb::locks_installation("http_502"));
    EXPECT_FALSE(wb::locks_installation("network"));
    // Other control-plane 403 codes are not revocations either.
    EXPECT_FALSE(wb::locks_installation("FORBIDDEN"));
    EXPECT_FALSE(wb::locks_installation("INSTALLATION_CONFLICT"));
    EXPECT_FALSE(wb::locks_installation(""));
}
