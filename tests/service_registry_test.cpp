// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>
#include <vitreo/service_registry.hpp>

using namespace vitreo;

// ---------------------------------------------------------------------------
// Dummy service types with distinct kServiceId values
// ---------------------------------------------------------------------------

struct ServiceA {
    static constexpr u32 kServiceId = 0;
    int value = 42;
};

struct ServiceB {
    static constexpr u32 kServiceId = 1;
    float data = 3.14f;
};

struct ServiceC {
    static constexpr u32 kServiceId = 2;
};

// ===========================================================================
// Tests
// ===========================================================================

TEST(ServiceRegistryTest, GetUnregisteredReturnsNullptr) {
    ServiceRegistry reg;
    EXPECT_EQ(reg.get<ServiceA>(), nullptr);
    EXPECT_EQ(reg.get<ServiceB>(), nullptr);
}

TEST(ServiceRegistryTest, RegisterAndRetrieve) {
    ServiceRegistry reg;
    ServiceA a;
    reg.register_service(&a);
    EXPECT_EQ(reg.get<ServiceA>(), &a);
}

TEST(ServiceRegistryTest, RegisterPreservesPointer) {
    ServiceRegistry reg;
    ServiceA a;
    a.value = 99;
    reg.register_service(&a);
    EXPECT_EQ(reg.get<ServiceA>()->value, 99);
}

TEST(ServiceRegistryTest, MultipleDifferentServices) {
    ServiceRegistry reg;
    ServiceA a;
    ServiceB b;
    ServiceC c;

    reg.register_service(&a);
    reg.register_service(&b);
    reg.register_service(&c);

    EXPECT_EQ(reg.get<ServiceA>(), &a);
    EXPECT_EQ(reg.get<ServiceB>(), &b);
    EXPECT_EQ(reg.get<ServiceC>(), &c);
}

TEST(ServiceRegistryTest, OverwriteReplaces) {
    ServiceRegistry reg;
    ServiceA a1;
    a1.value = 1;
    ServiceA a2;
    a2.value = 2;

    reg.register_service(&a1);
    EXPECT_EQ(reg.get<ServiceA>(), &a1);

    reg.register_service(&a2);
    EXPECT_EQ(reg.get<ServiceA>(), &a2);
    EXPECT_EQ(reg.get<ServiceA>()->value, 2);
}

TEST(ServiceRegistryTest, RegisterDoesNotAffectOtherSlots) {
    ServiceRegistry reg;
    ServiceA a;
    reg.register_service(&a);

    // ServiceB should still be null
    EXPECT_EQ(reg.get<ServiceB>(), nullptr);
}
