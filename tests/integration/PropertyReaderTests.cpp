#include "AtomCache.hpp"
#include "PropertyReader.hpp"
#include "X11Connection.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <xcb/xcb.h>

namespace prismdrake::x11 {
namespace {

using foundation::ErrorCode;

struct ConnectionDeleter final {
    void operator()(xcb_connection_t *connection) const noexcept {
        if (connection != nullptr) {
            xcb_disconnect(connection);
        }
    }
};

struct ReplyDeleter final {
    void operator()(void *reply) const noexcept { std::free(reply); }
};

using WriterConnection = std::unique_ptr<xcb_connection_t, ConnectionDeleter>;
using ProtocolError = std::unique_ptr<xcb_generic_error_t, ReplyDeleter>;

[[nodiscard]] xcb_screen_t *firstScreen(xcb_connection_t *connection) {
    const auto *setup = xcb_get_setup(connection);
    if (setup == nullptr) {
        return nullptr;
    }
    auto iterator = xcb_setup_roots_iterator(setup);
    return iterator.rem > 0 ? iterator.data : nullptr;
}

[[nodiscard]] bool checked(xcb_connection_t *connection, xcb_void_cookie_t cookie) {
    ProtocolError error{xcb_request_check(connection, cookie)};
    return error == nullptr;
}

TEST(PropertyReaderIntegrationTest, InternsOnlyTheCompleteFixedVocabulary) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    auto connection = X11Connection::connect(display);
    ASSERT_TRUE(connection);

    const auto atoms = AtomCache::create(connection.value());

    ASSERT_TRUE(atoms);
    for (std::size_t index = 0U; index < static_cast<std::size_t>(AtomName::count); ++index) {
        const auto atom = atoms.value().atom(static_cast<AtomName>(index));
        ASSERT_TRUE(atom);
        EXPECT_NE(atom->value(), 0U);
    }
    EXPECT_FALSE(atoms.value().atom(AtomName::count));
}

TEST(PropertyReaderIntegrationTest, PreservesCacheProvenanceAcrossConnectionMove) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    auto connection = X11Connection::connect(display);
    ASSERT_TRUE(connection);
    auto atoms = AtomCache::create(connection.value());
    ASSERT_TRUE(atoms);

    X11Connection movedConnection{std::move(connection.value())};
    constexpr PropertySpec absentPid{AtomName::cardinal, PropertyFormat::bits_32, 1U, 4U};
    const auto result =
        PropertyReader::read(movedConnection, atoms.value(), movedConnection.screen().rootWindow,
                             AtomName::net_wm_pid, absentPid);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::not_found);

    const auto movedFrom =
        PropertyReader::read(connection.value(), atoms.value(), movedConnection.screen().rootWindow,
                             AtomName::net_wm_pid, absentPid);
    ASSERT_FALSE(movedFrom);
    EXPECT_EQ(movedFrom.error().code, ErrorCode::invalid_argument);
}

TEST(PropertyReaderIntegrationTest, RejectsCacheFromAnotherOrDestroyedConnection) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    auto first = X11Connection::connect(display);
    auto second = X11Connection::connect(display);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto firstAtoms = AtomCache::create(first.value());
    ASSERT_TRUE(firstAtoms);

    constexpr PropertySpec absentPid{AtomName::cardinal, PropertyFormat::bits_32, 1U, 4U};
    const auto mismatched =
        PropertyReader::read(second.value(), firstAtoms.value(), second.value().screen().rootWindow,
                             AtomName::net_wm_pid, absentPid);
    ASSERT_FALSE(mismatched);
    EXPECT_EQ(mismatched.error().code, ErrorCode::invalid_argument);

    auto detachedAtoms = [display]() -> foundation::Result<AtomCache> {
        auto ephemeral = X11Connection::connect(display);
        if (!ephemeral) {
            return foundation::Result<AtomCache>::failure(ephemeral.error());
        }
        return AtomCache::create(ephemeral.value());
    }();
    ASSERT_TRUE(detachedAtoms);
    const auto detached =
        PropertyReader::read(second.value(), detachedAtoms.value(),
                             second.value().screen().rootWindow, AtomName::net_wm_pid, absentPid);
    ASSERT_FALSE(detached);
    EXPECT_EQ(detached.error().code, ErrorCode::invalid_argument);
}

TEST(PropertyReaderIntegrationTest, ReadsStrictBoundsAndRejectsStaleOrMalformedState) {
    const char *display = std::getenv("DISPLAY");
    ASSERT_NE(display, nullptr);
    auto connection = X11Connection::connect(display);
    ASSERT_TRUE(connection);
    auto atoms = AtomCache::create(connection.value());
    ASSERT_TRUE(atoms);

    int writerScreenIndex = 0;
    WriterConnection writer{xcb_connect(display, &writerScreenIndex)};
    ASSERT_TRUE(writer);
    ASSERT_EQ(xcb_connection_has_error(writer.get()), 0);
    auto *screen = firstScreen(writer.get());
    ASSERT_NE(screen, nullptr);

    const xcb_window_t rawWindow = xcb_generate_id(writer.get());
    ASSERT_TRUE(
        checked(writer.get(), xcb_create_window_checked(writer.get(), XCB_COPY_FROM_PARENT,
                                                        rawWindow, screen->root, 0, 0, 32U, 32U, 0U,
                                                        XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                        screen->root_visual, 0U, nullptr)));
    auto window = WindowId::fromProtocol(rawWindow);
    ASSERT_TRUE(window);
    const auto pidAtom = atoms.value().atom(AtomName::net_wm_pid);
    const auto cardinalAtom = atoms.value().atom(AtomName::cardinal);
    ASSERT_TRUE(pidAtom);
    ASSERT_TRUE(cardinalAtom);

    const std::vector<std::uint32_t> published{7U, 11U, 13U, 17U};
    ASSERT_TRUE(checked(writer.get(),
                        xcb_change_property_checked(writer.get(), XCB_PROP_MODE_REPLACE, rawWindow,
                                                    pidAtom->value(), cardinalAtom->value(), 32U,
                                                    published.size(), published.data())));
    ASSERT_GT(xcb_flush(writer.get()), 0);

    constexpr PropertySpec complete{AtomName::cardinal, PropertyFormat::bits_32, 4U, 16U};
    const auto value = PropertyReader::read(connection.value(), atoms.value(), window.value(),
                                            AtomName::net_wm_pid, complete);
    ASSERT_TRUE(value);
    const auto items = value.value().uint32Items();
    ASSERT_TRUE(items);
    EXPECT_EQ(items.value(), published);

    constexpr PropertySpec oneItem{AtomName::cardinal, PropertyFormat::bits_32, 1U, 4U};
    const auto truncated = PropertyReader::read(connection.value(), atoms.value(), window.value(),
                                                AtomName::net_wm_pid, oneItem);
    ASSERT_FALSE(truncated);
    EXPECT_EQ(truncated.error().code, ErrorCode::validation_error);

    constexpr PropertySpec wrongType{AtomName::window, PropertyFormat::bits_32, 4U, 16U};
    const auto mismatched = PropertyReader::read(connection.value(), atoms.value(), window.value(),
                                                 AtomName::net_wm_pid, wrongType);
    ASSERT_FALSE(mismatched);
    EXPECT_EQ(mismatched.error().code, ErrorCode::validation_error);

    constexpr PropertySpec absentName{AtomName::utf8_string, PropertyFormat::bits_8, 64U, 64U};
    const auto absent = PropertyReader::read(connection.value(), atoms.value(), window.value(),
                                             AtomName::net_wm_name, absentName);
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, ErrorCode::not_found);

    ASSERT_TRUE(checked(writer.get(), xcb_destroy_window_checked(writer.get(), rawWindow)));
    ASSERT_GT(xcb_flush(writer.get()), 0);
    const auto stale = PropertyReader::read(connection.value(), atoms.value(), window.value(),
                                            AtomName::net_wm_pid, complete);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::io_error);
}

} // namespace
} // namespace prismdrake::x11
