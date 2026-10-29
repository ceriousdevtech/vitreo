/// @file test_window_class.cpp
/// @brief Tests for WindowClassManager (WIN32-050)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/user32/classes/window_class.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

class WindowClassManagerTest : public ::testing::Test {
protected:
    WindowClassManager mgr;

    WindowClassData make_class(const std::wstring& name, u64 wndproc = 0x1000) {
        WindowClassData data;
        data.class_name = name;
        data.wndproc = wndproc;
        data.style = CS::HREDRAW | CS::VREDRAW;
        return data;
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

TEST_F(WindowClassManagerTest, RegisterClassReturnsNonZeroAtom) {
    ATOM atom = mgr.register_class(make_class(L"TestClass"));
    EXPECT_NE(atom, 0);
}

TEST_F(WindowClassManagerTest, RegisterClassReturnsAtomAboveC000) {
    ATOM atom = mgr.register_class(make_class(L"TestClass"));
    EXPECT_GE(atom, 0xC000);
}

TEST_F(WindowClassManagerTest, DuplicateClassNameFails) {
    mgr.register_class(make_class(L"MyClass"));
    ATOM dup = mgr.register_class(make_class(L"MyClass"));
    EXPECT_EQ(dup, 0);
}

TEST_F(WindowClassManagerTest, DuplicateIsCaseInsensitive) {
    mgr.register_class(make_class(L"MyClass"));
    ATOM dup = mgr.register_class(make_class(L"MYCLASS"));
    EXPECT_EQ(dup, 0);
}

TEST_F(WindowClassManagerTest, RegisterWithNullWndprocSucceeds) {
    // Windows does not validate lpfnWndProc (measured,
    // tools/oracle_probe/s1win_api_probe.expected registerclassexw_null_wndproc).
    ATOM atom = mgr.register_class(make_class(L"BadClass", 0));
    EXPECT_NE(atom, 0);
}

TEST_F(WindowClassManagerTest, MultipleClassesGetSequentialAtoms) {
    ATOM a1 = mgr.register_class(make_class(L"ClassA"));
    ATOM a2 = mgr.register_class(make_class(L"ClassB"));
    ATOM a3 = mgr.register_class(make_class(L"ClassC"));
    EXPECT_EQ(a2, a1 + 1);
    EXPECT_EQ(a3, a2 + 1);
}

// ---------------------------------------------------------------------------
// Unregistration
// ---------------------------------------------------------------------------

TEST_F(WindowClassManagerTest, UnregisterClassSucceeds) {
    mgr.register_class(make_class(L"ToRemove"));
    EXPECT_TRUE(mgr.unregister_class(L"ToRemove"));
}

TEST_F(WindowClassManagerTest, UnregisterUnregisteredClassFails) {
    EXPECT_FALSE(mgr.unregister_class(L"DoesNotExist"));
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

TEST_F(WindowClassManagerTest, FindByAtomReturnsCorrectData) {
    auto data = make_class(L"LookupClass");
    data.style = CS::OWNDC;
    ATOM atom = mgr.register_class(data);

    const WindowClassData* found = mgr.find_by_atom(atom);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->class_name, L"LookupClass");
    EXPECT_EQ(found->style, CS::OWNDC);
    EXPECT_EQ(found->atom, atom);
}

TEST_F(WindowClassManagerTest, FindByNameCaseInsensitive) {
    mgr.register_class(make_class(L"CaseTest"));

    const WindowClassData* found = mgr.find_by_name(L"casetest");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->class_name, L"CaseTest");
}

TEST_F(WindowClassManagerTest, FindByAtomAfterUnregisterReturnsNull) {
    ATOM atom = mgr.register_class(make_class(L"Temp"));
    mgr.unregister_class(L"Temp");

    EXPECT_EQ(mgr.find_by_atom(atom), nullptr);
}

TEST_F(WindowClassManagerTest, FindAtomReturnsCorrectAtom) {
    ATOM atom = mgr.register_class(make_class(L"AtomLookup"));
    ATOM found = mgr.find_atom(L"ATOMLOOKUP");
    EXPECT_EQ(found, atom);
}

TEST_F(WindowClassManagerTest, FindAtomReturnsZeroForUnknown) {
    EXPECT_EQ(mgr.find_atom(L"Unknown"), 0);
}

// ---------------------------------------------------------------------------
// Count
// ---------------------------------------------------------------------------

TEST_F(WindowClassManagerTest, CountTracksRegistrations) {
    EXPECT_EQ(mgr.count(), 0u);
    mgr.register_class(make_class(L"A"));
    EXPECT_EQ(mgr.count(), 1u);
    mgr.register_class(make_class(L"B"));
    EXPECT_EQ(mgr.count(), 2u);
    mgr.unregister_class(L"A");
    EXPECT_EQ(mgr.count(), 1u);
}

// ---------------------------------------------------------------------------
// Re-registration after unregister
// ---------------------------------------------------------------------------

TEST_F(WindowClassManagerTest, ReRegisterAfterUnregisterSucceeds) {
    mgr.register_class(make_class(L"Reuse"));
    mgr.unregister_class(L"Reuse");
    ATOM atom = mgr.register_class(make_class(L"Reuse"));
    EXPECT_NE(atom, 0);
}
