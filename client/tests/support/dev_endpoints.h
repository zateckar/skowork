// Tests that need a SKOWORK_DEV_ENDPOINTS build: loopback http fakes (identity provider, Graph mock, speech engine)
// and the worker test hooks. Release builds leave those out, so such a test skips there with the reason in the output.
#pragma once

#include <gtest/gtest.h>

#ifdef SKOWORK_DEV_ENDPOINTS
#define WB_REQUIRES_DEV_ENDPOINTS() static_cast<void>(0)
#else
#define WB_REQUIRES_DEV_ENDPOINTS() GTEST_SKIP() << "needs a SKOWORK_DEV_ENDPOINTS build (loopback fakes and test hooks)"
#endif
