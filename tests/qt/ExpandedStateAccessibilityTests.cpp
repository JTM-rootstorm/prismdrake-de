#include "ExpandedStateAccessibility.hpp"

#include <QAccessible>
#include <QAccessibleStateChangeEvent>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>

#include <gtest/gtest.h>

#include <vector>

namespace {

struct CapturedStateChange final {
    QObject *object;
    QAccessible::State changed;
};

std::vector<CapturedStateChange> capturedStateChanges;

void captureAccessibilityUpdate(QAccessibleEvent *event) {
    if (event != nullptr && event->type() == QAccessible::StateChanged) {
        const auto *stateEvent = static_cast<QAccessibleStateChangeEvent *>(event);
        capturedStateChanges.push_back({event->object(), stateEvent->changedStates()});
    }
}

class UpdateHandlerGuard final {
  public:
    UpdateHandlerGuard()
        : previous_(QAccessible::installUpdateHandler(captureAccessibilityUpdate)) {
        capturedStateChanges.clear();
    }

    ~UpdateHandlerGuard() { QAccessible::installUpdateHandler(previous_); }

    UpdateHandlerGuard(const UpdateHandlerGuard &) = delete;
    UpdateHandlerGuard &operator=(const UpdateHandlerGuard &) = delete;

  private:
    QAccessible::UpdateHandler previous_;
};

TEST(ExpandedStateAccessibilityTest, PreservesControlInterfaceAndPublishesStateTransitions) {
    prismdrake::shell::accessibility::installExpandedStateAccessibility();
    QQmlApplicationEngine engine;
    engine.loadData(R"(
import QtQuick
import QtQuick.Controls
Control {
    width: 160
    height: 48
    property bool prismdrakeAccessibleExpandable: true
    property bool prismdrakeAccessibleExpanded: false
    property int pressCount: 0
    locale: Qt.locale("ar_EG")
    Accessible.role: Accessible.Button
    Accessible.name: "Expandable probe"
    Accessible.focusable: true
    Accessible.onPressAction: pressCount += 1
}
)");
    ASSERT_EQ(engine.rootObjects().size(), 1);
    auto *control = engine.rootObjects().front();
    auto *accessible = QAccessible::queryAccessibleInterface(control);
    ASSERT_NE(accessible, nullptr);

    EXPECT_EQ(accessible->role(), QAccessible::Button);
    EXPECT_EQ(accessible->text(QAccessible::Name), QStringLiteral("Expandable probe"));
    ASSERT_NE(accessible->actionInterface(), nullptr);
    EXPECT_TRUE(accessible->actionInterface()->actionNames().contains(
        QAccessibleActionInterface::pressAction()));
    accessible->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    EXPECT_EQ(control->property("pressCount").toInt(), 1);

    auto *attributes = accessible->attributesInterface();
    ASSERT_NE(attributes, nullptr);
    EXPECT_TRUE(attributes->attributeKeys().contains(QAccessible::Attribute::Locale));
    const QLocale expectedLocale{QLocale::Arabic, QLocale::Egypt};
    EXPECT_EQ(attributes->attributeValue(QAccessible::Attribute::Locale).value<QLocale>(),
              expectedLocale);

    auto state = accessible->state();
    EXPECT_TRUE(state.expandable);
    EXPECT_FALSE(state.expanded);
    EXPECT_TRUE(state.collapsed);

    UpdateHandlerGuard updates;
    ASSERT_TRUE(control->setProperty("prismdrakeAccessibleExpanded", true));
    QCoreApplication::processEvents();
    state = accessible->state();
    EXPECT_TRUE(state.expandable);
    EXPECT_TRUE(state.expanded);
    EXPECT_FALSE(state.collapsed);
    ASSERT_EQ(capturedStateChanges.size(), 1U);
    EXPECT_EQ(capturedStateChanges.front().object, control);
    EXPECT_TRUE(capturedStateChanges.front().changed.expandable);
    EXPECT_TRUE(capturedStateChanges.front().changed.expanded);
    EXPECT_TRUE(capturedStateChanges.front().changed.collapsed);
}

TEST(ExpandedStateAccessibilityTest, NonExpandableControlExportsNoExpansionState) {
    prismdrake::shell::accessibility::installExpandedStateAccessibility();
    QQmlApplicationEngine engine;
    engine.loadData(R"(
import QtQuick
import QtQuick.Controls
Control {
    property bool prismdrakeAccessibleExpandable: false
    property bool prismdrakeAccessibleExpanded: false
    Accessible.role: Accessible.Button
    Accessible.name: "Non-expandable probe"
}
)");
    ASSERT_EQ(engine.rootObjects().size(), 1);
    const auto state =
        prismdrake::shell::accessibility::queryExpandedState(engine.rootObjects().front());
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->expandable);
    EXPECT_FALSE(state->expanded);
    EXPECT_FALSE(state->collapsed);
}

} // namespace

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
