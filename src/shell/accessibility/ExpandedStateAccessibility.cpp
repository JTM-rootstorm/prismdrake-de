#include "ExpandedStateAccessibility.hpp"

#include <QAccessible>
#include <QAccessibleStateChangeEvent>
#include <QLocale>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QObject>
#include <QQuickItem>
#include <QVariant>

#include <QtQuick/private/qaccessiblequickitem_p.h>
#include <QtQuickTemplates2/private/qquickcontrol_p.h>

#include <atomic>

namespace prismdrake::shell::accessibility {
namespace {

constexpr auto expandedPropertyName = "prismdrakeAccessibleExpanded";
constexpr auto expandablePropertyName = "prismdrakeAccessibleExpandable";

class ExpandedStateNotifier final : public QObject {
    Q_OBJECT

  public:
    explicit ExpandedStateNotifier(QQuickItem *item) : QObject(item) {
        const auto slotIndex = metaObject()->indexOfSlot("notifyExpandedStateChanged()");
        Q_ASSERT(slotIndex >= 0);
        const auto slot = metaObject()->method(slotIndex);
        for (const auto propertyName : {expandedPropertyName, expandablePropertyName}) {
            const auto propertyIndex = item->metaObject()->indexOfProperty(propertyName);
            if (propertyIndex < 0) {
                continue;
            }
            const auto property = item->metaObject()->property(propertyIndex);
            if (property.hasNotifySignal()) {
                QObject::connect(item, property.notifySignal(), this, slot);
            }
        }
    }

  private slots:
    void notifyExpandedStateChanged() {
        QAccessible::State changed{};
        changed.expanded = true;
        changed.collapsed = true;
        changed.expandable = true;
        QAccessibleStateChangeEvent event(parent(), changed);
        QAccessible::updateAccessibility(&event);
    }
};

class ExpandedStateAccessible final : public QAccessibleQuickItem {
  public:
    explicit ExpandedStateAccessible(QQuickControl *control)
        : QAccessibleQuickItem(control), control_(control) {
        const auto propertyIndex = control->metaObject()->indexOfProperty(expandedPropertyName);
        if (propertyIndex < 0 || control->findChild<ExpandedStateNotifier *>(
                                     {}, Qt::FindDirectChildrenOnly) != nullptr) {
            return;
        }
        new ExpandedStateNotifier(control);
    }

    [[nodiscard]] QAccessible::State state() const override {
        auto accessibleState = QAccessibleQuickItem::state();
        const auto expandableProperty =
            item()->metaObject()->indexOfProperty(expandablePropertyName);
        const auto expandable =
            expandableProperty < 0 || item()->property(expandablePropertyName).toBool();
        const auto expanded = item()->property(expandedPropertyName).toBool();
        accessibleState.expandable = expandable;
        accessibleState.expanded = expandable && expanded;
        accessibleState.collapsed = expandable && !expanded;
        return accessibleState;
    }

    [[nodiscard]] QList<QAccessible::Attribute> attributeKeys() const override {
        auto keys = QAccessibleQuickItem::attributeKeys();
        if (!keys.contains(QAccessible::Attribute::Locale)) {
            keys.push_back(QAccessible::Attribute::Locale);
        }
        return keys;
    }

    [[nodiscard]] QVariant attributeValue(QAccessible::Attribute key) const override {
        if (key == QAccessible::Attribute::Locale) {
            return QVariant::fromValue(control_->locale());
        }
        return QAccessibleQuickItem::attributeValue(key);
    }

  private:
    QQuickControl *const control_;
};

[[nodiscard]] QAccessibleInterface *expandedStateFactory(const QString &, QObject *object) {
    if (object == nullptr || object->metaObject()->indexOfProperty(expandedPropertyName) < 0) {
        return nullptr;
    }
    auto *control = qobject_cast<QQuickControl *>(object);
    return control != nullptr ? new ExpandedStateAccessible(control) : nullptr;
}

} // namespace

void installExpandedStateAccessibility() {
    static std::atomic_bool installed{false};
    if (!installed.exchange(true)) {
        QAccessible::installFactory(expandedStateFactory);
    }
}

std::optional<ExpandedState> queryExpandedState(QObject *object) {
    if (object == nullptr || object->metaObject()->indexOfProperty(expandedPropertyName) < 0) {
        return std::nullopt;
    }
    auto *accessible = QAccessible::queryAccessibleInterface(object);
    if (accessible == nullptr) {
        return std::nullopt;
    }
    const auto state = accessible->state();
    return ExpandedState{state.expandable, state.expanded, state.collapsed};
}

} // namespace prismdrake::shell::accessibility

#include "ExpandedStateAccessibility.moc"
