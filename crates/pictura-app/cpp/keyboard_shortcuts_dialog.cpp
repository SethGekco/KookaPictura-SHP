#include "keyboard_shortcuts_dialog.h"

#include "commands.h"
#include "tools.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItemIterator>
#include <QtWidgets/QVBoxLayout>

#include <functional>

namespace pictura {

// The Shortcut cell's editor: while it has focus every non-modifier chord is
// taken as the new binding (Return, Tab, and arrows included, as in CS6);
// Escape leaves the field.
class ShortcutCaptureField : public QWidget {
public:
    explicit ShortcutCaptureField(QWidget* parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("shortcutCaptureField"));
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_InputMethodEnabled, false);
    }

    std::function<void(const QKeySequence&)> onCaptured;

    void setText(const QString& text)
    {
        text_ = text;
        update();
    }

protected:
    bool event(QEvent* event) override
    {
        // Tab would otherwise move focus before keyPressEvent sees it.
        if (event->type() == QEvent::KeyPress) {
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        const int key = event->key();
        if (key == Qt::Key_Escape) {
            clearFocus();
            return;
        }
        if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift
            || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr) {
            return;
        }
        const Qt::KeyboardModifiers mods = event->modifiers()
            & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
        if (onCaptured) {
            onCaptured(QKeySequence(QKeyCombination(mods, Qt::Key(key))));
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const QPalette& pal = palette();
        painter.fillRect(rect(), pal.base());
        painter.setPen(hasFocus() ? pal.highlight().color() : pal.mid().color());
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        painter.setPen(pal.text().color());
        const QRect textRect = rect().adjusted(4, 0, -4, 0);
        painter.drawText(textRect, Qt::AlignVCenter, text_);
        if (hasFocus()) {
            const int x = textRect.left() + fontMetrics().horizontalAdvance(text_) + 1;
            painter.drawLine(x, 3, x, height() - 4);
        }
    }

    void focusInEvent(QFocusEvent* event) override
    {
        QWidget::focusInEvent(event);
        update();
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        QWidget::focusOutEvent(event);
        update();
    }

private:
    QString text_;
};

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kBindingRole = Qt::UserRole + 1;

// The tool letter a chord would trigger: the letter itself, or Shift+letter
// (the group's cycle key). Null for any other chord.
QChar toolLetterFor(const QKeySequence& chord)
{
    if (chord.count() != 1) {
        return QChar();
    }
    const QKeyCombination combo = chord[0];
    const int key = int(combo.key());
    const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
    if (key < Qt::Key_A || key > Qt::Key_Z || (mods != Qt::NoModifier && mods != Qt::ShiftModifier)) {
        return QChar();
    }
    return QChar(key);
}

const WorkspaceKey* workspaceKeyFor(const QKeySequence& chord)
{
    for (const WorkspaceKey& reserved : workspaceKeys()) {
        if (reserved.key == chord) {
            return &reserved;
        }
    }
    return nullptr;
}

bool toolForName(const QString& name, ToolId* out)
{
    for (ToolId id : allToolIds()) {
        if (toolIdName(id) == name) {
            *out = id;
            return true;
        }
    }
    return false;
}

QString commandPath(const CommandRegistry* registry, const QString& id)
{
    for (const CommandInfo& info : registry->describe()) {
        if (info.id == id) {
            return info.path.join(QStringLiteral(" > "));
        }
    }
    return id;
}

QString helpText()
{
    return QCoreApplication::translate(
        "KeyboardShortcutsDialog",
        "To edit keyboard shortcuts:\n"
        "1) Click in the \"Shortcut\" column for a command and press the keyboard "
        "shortcut to assign.\n"
        "2) Click Accept, or choose another command, to keep it; Add Shortcut gives a "
        "command another shortcut.\n"
        "3) Click OK when you are done editing to save all your changes.");
}

} // namespace

const QList<WorkspaceKey>& workspaceKeys()
{
    static const QList<WorkspaceKey> keys = {
        {QKeySequence(Qt::Key_BracketLeft), QT_TR_NOOP("Decrease Brush Size")},
        {QKeySequence(Qt::Key_BracketRight), QT_TR_NOOP("Increase Brush Size")},
        {QKeySequence(Qt::SHIFT | Qt::Key_BracketLeft), QT_TR_NOOP("Decrease Brush Hardness")},
        {QKeySequence(Qt::SHIFT | Qt::Key_BracketRight), QT_TR_NOOP("Increase Brush Hardness")},
        {QKeySequence(Qt::Key_D), QT_TR_NOOP("Default Foreground/Background Colors")},
        {QKeySequence(Qt::Key_X), QT_TR_NOOP("Switch Foreground/Background Colors")},
        {QKeySequence(Qt::Key_F), QT_TR_NOOP("Cycle Screen Modes")},
        // CS6's Quick Mask key, kept free for it.
        {QKeySequence(Qt::Key_Q), QT_TR_NOOP("Quick Mask Mode")},
        {QKeySequence(Qt::Key_Tab), QT_TR_NOOP("Show/Hide Panels")},
        {QKeySequence(Qt::SHIFT | Qt::Key_Tab), QT_TR_NOOP("Show/Hide Panels")},
        {QKeySequence(Qt::CTRL | Qt::Key_Tab), QT_TR_NOOP("Next Document Window")},
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QT_TR_NOOP("Previous Document Window")},
        {QKeySequence(Qt::Key_Delete), QT_TR_NOOP("Delete / Clear")},
        {QKeySequence(Qt::Key_Backspace), QT_TR_NOOP("Delete / Clear")},
        {QKeySequence(Qt::SHIFT | Qt::Key_F1), QT_TR_NOOP("Darker Interface")},
        {QKeySequence(Qt::SHIFT | Qt::Key_F2), QT_TR_NOOP("Lighter Interface")},
        // KookaPictura-SHP fork (shp_support.cpp): SHP frame stepping.
        {QKeySequence(Qt::Key_Comma), QT_TR_NOOP("Previous SHP Frame")},
        {QKeySequence(Qt::Key_Period), QT_TR_NOOP("Next SHP Frame")},
    };
    return keys;
}

KeyboardShortcutsDialog::KeyboardShortcutsDialog(CommandRegistry* registry, QWidget* parent)
    : QDialog(parent)
    , registry_(registry)
{
    setObjectName(QStringLiteral("keyboardShortcutsDialog"));
    setWindowTitle(tr("Keyboard Shortcuts and Menus"));
    resize(800, 600);

    for (const CommandInfo& info : registry_->describe()) {
        working_.commands.insert(info.id, registry_->shortcuts(info.id));
    }
    for (ToolId id : allToolIds()) {
        working_.tools.insert(int(id), toolShortcut(id));
    }

    auto* root = new QHBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    auto* page = new QWidget(tabs);
    tabs->addTab(page, tr("Keyboard Shortcuts"));
    tabs->addTab(new QWidget(tabs), tr("Menus"));
    tabs->setTabEnabled(1, false);
    root->addWidget(tabs, 1);

    auto* side = new QVBoxLayout;
    auto* ok = new QPushButton(tr("OK"), this);
    ok->setObjectName(QStringLiteral("shortcutsOk"));
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("shortcutsCancel"));
    for (QPushButton* button : {ok, cancel}) {
        button->setAutoDefault(false);
        side->addWidget(button);
    }
    side->addStretch(1);
    root->addLayout(side);

    auto* column = new QVBoxLayout(page);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Shortcuts For:"), page));
    scopeCombo_ = new QComboBox(page);
    scopeCombo_->setObjectName(QStringLiteral("shortcutsScope"));
    scopeCombo_->addItem(tr("Application Menus"));
    scopeCombo_->addItem(tr("Panel Menus"));
    scopeCombo_->addItem(tr("Tools"));
    if (auto* model = qobject_cast<QStandardItemModel*>(scopeCombo_->model())) {
        model->item(1)->setEnabled(false);
    }
    top->addWidget(scopeCombo_);
    top->addStretch(1);
    top->addWidget(new QLabel(tr("Set:"), page));
    auto* set = new QComboBox(page);
    set->addItem(tr("Photoshop Defaults"));
    set->setEnabled(false);
    top->addWidget(set);
    column->addLayout(top);
    auto* legacy = new QCheckBox(tr("Use Legacy Channel Shortcuts"), page);
    legacy->setEnabled(false);
    column->addWidget(legacy);

    auto* body = new QHBoxLayout;
    tree_ = new QTreeWidget(page);
    tree_->setObjectName(QStringLiteral("shortcutsTree"));
    tree_->setColumnCount(2);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree_->header()->resizeSection(1, 220);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setUniformRowHeights(true);
    body->addWidget(tree_, 1);

    auto* buttons = new QVBoxLayout;
    auto addButton = [page, buttons](const QString& text, const char* name) {
        auto* button = new QPushButton(text, page);
        button->setObjectName(QLatin1String(name));
        button->setAutoDefault(false);
        // Clicking a button leaves the key field focused for the next chord.
        button->setFocusPolicy(Qt::NoFocus);
        buttons->addWidget(button);
        return button;
    };
    acceptButton_ = addButton(tr("Accept"), "shortcutsAccept");
    undoButton_ = addButton(tr("Undo"), "shortcutsUndo");
    defaultButton_ = addButton(tr("Use Default"), "shortcutsUseDefault");
    buttons->addSpacing(16);
    addButton_ = addButton(tr("Add Shortcut"), "shortcutsAdd");
    deleteButton_ = addButton(tr("Delete Shortcut"), "shortcutsDelete");
    buttons->addSpacing(16);
    addButton(tr("Summarize..."), "shortcutsSummarize")->setEnabled(false);
    buttons->addStretch(1);
    body->addLayout(buttons);
    column->addLayout(body, 1);

    auto* info = new QFrame(page);
    info->setObjectName(QStringLiteral("shortcutsInfo"));
    info->setFrameShape(QFrame::StyledPanel);
    auto* infoRow = new QHBoxLayout(info);
    infoIcon_ = new QLabel(info);
    infoIcon_->setAlignment(Qt::AlignTop);
    infoRow->addWidget(infoIcon_);
    auto* infoColumn = new QVBoxLayout;
    infoText_ = new QLabel(info);
    infoText_->setObjectName(QStringLiteral("shortcutsInfoText"));
    // Help, refusals, and conflicts read dark-on-yellow (theme `notice`).
    infoText_->setProperty("notice", true);
    infoText_->setWordWrap(true);
    infoColumn->addWidget(infoText_);
    auto* conflictRow = new QHBoxLayout;
    goToConflictButton_ = new QPushButton(tr("Accept and Go To Conflict"), info);
    goToConflictButton_->setObjectName(QStringLiteral("shortcutsGoToConflict"));
    undoChangesButton_ = new QPushButton(tr("Undo Changes"), info);
    undoChangesButton_->setObjectName(QStringLiteral("shortcutsUndoChanges"));
    for (QPushButton* button : {goToConflictButton_, undoChangesButton_}) {
        button->setAutoDefault(false);
        button->setFocusPolicy(Qt::NoFocus);
        conflictRow->addWidget(button);
    }
    conflictRow->addStretch(1);
    infoColumn->addLayout(conflictRow);
    infoRow->addLayout(infoColumn, 1);
    column->addWidget(info);

    connect(scopeCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        commitPending();
        scope_ = index == 2 ? Scope::Tools : Scope::ApplicationMenus;
        addingKey_.clear();
        buildRows();
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current) { selectRow(current); });
    // A click on the already-current row puts the key field back in focus.
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (capture_ && item == tree_->currentItem()) {
            capture_->setFocus(Qt::MouseFocusReason);
        }
    });
    connect(acceptButton_, &QPushButton::clicked, this, [this]() { acceptPending(false); });
    connect(goToConflictButton_, &QPushButton::clicked, this, [this]() { acceptPending(true); });
    connect(undoChangesButton_, &QPushButton::clicked, this,
            &KeyboardShortcutsDialog::discardPending);
    connect(undoButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::undo);
    connect(defaultButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::useDefault);
    connect(addButton_, &QPushButton::clicked, this, &KeyboardShortcutsDialog::addShortcut);
    connect(deleteButton_, &QPushButton::clicked, this,
            &KeyboardShortcutsDialog::deleteShortcut);
    connect(ok, &QPushButton::clicked, this, &KeyboardShortcutsDialog::applyAndAccept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    buildRows();
}

void KeyboardShortcutsDialog::buildRows()
{
    pending_ = Pending{};
    capture_ = nullptr;
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    if (scope_ == Scope::Tools) {
        tree_->setHeaderLabels({tr("Tool"), tr("Shortcut")});
        tree_->setRootIsDecorated(false);
        for (ToolId id : allToolIds()) {
            const QChar letter = working_.tools.value(int(id));
            auto* item = new QTreeWidgetItem(tree_, {QString::fromLatin1(toolInfo(id).label),
                                                     letter.isNull() ? QString() : QString(letter)});
            item->setData(0, kKeyRole, toolIdName(id));
            item->setData(0, kBindingRole, 0);
        }
    } else {
        tree_->setHeaderLabels({tr("Application Menu Command"), tr("Shortcut")});
        tree_->setRootIsDecorated(true);
        QHash<QString, QTreeWidgetItem*> menus;
        for (const CommandInfo& info : registry_->describe()) {
            QTreeWidgetItem* parent = nullptr;
            QString path;
            for (int i = 0; i + 1 < info.path.size(); ++i) {
                path += info.path.at(i) + QLatin1Char('/');
                QTreeWidgetItem* menu = menus.value(path);
                if (!menu) {
                    menu = parent ? new QTreeWidgetItem(parent, {info.path.at(i)})
                                  : new QTreeWidgetItem(tree_, {info.path.at(i)});
                    menu->setFlags(Qt::ItemIsEnabled);
                    menus.insert(path, menu);
                }
                parent = menu;
            }
            // One row per binding, the label on the first only (CS6's layout);
            // a command without one, or one being given another, shows a blank row.
            const QStringList texts = bindingTexts(info.id);
            const int rows = qMax(1, int(texts.size()) + (addingKey_ == info.id ? 1 : 0));
            for (int binding = 0; binding < rows; ++binding) {
                const QString label = binding == 0 ? info.path.last() : QString();
                const QString text = binding < texts.size() ? texts.at(binding) : QString();
                auto* item = parent ? new QTreeWidgetItem(parent, {label, text})
                                    : new QTreeWidgetItem(tree_, {label, text});
                item->setData(0, kKeyRole, info.id);
                item->setData(0, kBindingRole, binding);
            }
        }
        // Top-level menus open, submenus folded, as CS6 lists them.
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            tree_->topLevelItem(i)->setExpanded(true);
        }
    }
    showInfo();
    refreshButtons();
}

QString KeyboardShortcutsDialog::rowKey(const QTreeWidgetItem* item) const
{
    return item ? item->data(0, kKeyRole).toString() : QString();
}

int KeyboardShortcutsDialog::rowBinding(const QTreeWidgetItem* item) const
{
    return item ? item->data(0, kBindingRole).toInt() : 0;
}

QTreeWidgetItem* KeyboardShortcutsDialog::rowFor(const QString& key, int binding) const
{
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        if (rowKey(*it) == key && rowBinding(*it) == binding) {
            return *it;
        }
    }
    return nullptr;
}

QStringList KeyboardShortcutsDialog::bindingTexts(const QString& key) const
{
    QStringList texts;
    if (scope_ == Scope::Tools) {
        ToolId id;
        if (toolForName(key, &id) && !working_.tools.value(int(id)).isNull()) {
            texts << QString(working_.tools.value(int(id)));
        }
        return texts;
    }
    for (const QKeySequence& binding : working_.commands.value(key)) {
        texts << binding.toString(QKeySequence::NativeText);
    }
    return texts;
}

void KeyboardShortcutsDialog::refreshButtons()
{
    QTreeWidgetItem* item = tree_->currentItem();
    const QString key = rowKey(item);
    const bool row = !key.isEmpty();
    const bool bound = row && rowBinding(item) < bindingTexts(key).size();
    acceptButton_->setEnabled(pending_.dirty);
    undoButton_->setEnabled(pending_.dirty || !undoStack_.isEmpty());
    defaultButton_->setEnabled(row);
    // A tool takes one letter; a command may gain more bindings.
    addButton_->setEnabled(row && (scope_ == Scope::ApplicationMenus || !bound));
    deleteButton_->setEnabled(bound && !pending_.dirty);
}

void KeyboardShortcutsDialog::showInfo()
{
    const bool conflict = pending_.dirty && !pending_.conflictKey.isEmpty();
    goToConflictButton_->setVisible(conflict);
    undoChangesButton_->setVisible(conflict);
    if (!conflict) {
        setNotice(helpText(), false);
        return;
    }
    QString owner = pending_.conflictKey;
    ToolId tool;
    if (pending_.conflictScope == Scope::Tools && toolForName(pending_.conflictKey, &tool)) {
        owner = QString::fromLatin1(toolInfo(tool).label);
    } else if (pending_.conflictScope == Scope::ApplicationMenus) {
        owner = commandPath(registry_, pending_.conflictKey);
    }
    setNotice(tr("%1 is already in use and will be removed from %2 if accepted.")
                  .arg(pending_.value.toString(QKeySequence::NativeText), owner),
              true);
}

void KeyboardShortcutsDialog::setNotice(const QString& text, bool warning)
{
    infoText_->setText(text);
    // Capped at its text's width the label sits left and the yellow hugs the
    // words; an alignment flag instead would break the wrapped height.
    infoText_->ensurePolished();
    int textWidth = 0;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        textWidth = qMax(textWidth, infoText_->fontMetrics().horizontalAdvance(line));
    }
    const QMargins pad = infoText_->contentsMargins();
    infoText_->setMaximumWidth(textWidth + pad.left() + pad.right() + 2 * infoText_->margin() + 16);
    infoIcon_->setPixmap(style()
                             ->standardIcon(warning ? QStyle::SP_MessageBoxWarning
                                                    : QStyle::SP_MessageBoxInformation)
                             .pixmap(24, 24));
}

void KeyboardShortcutsDialog::selectRow(QTreeWidgetItem* item)
{
    // Leaving a row keeps its new shortcut, unless that would take another
    // command's: a steal needs an explicit Accept.
    if (pending_.dirty) {
        if (pending_.conflictKey.isEmpty()) {
            commitPending();
        } else {
            discardPending();
        }
    }
    openEditor(item);
}

void KeyboardShortcutsDialog::openEditor(QTreeWidgetItem* item)
{
    if (capture_) {
        if (QTreeWidgetItem* host = rowFor(pending_.key, pending_.binding)) {
            tree_->removeItemWidget(host, 1);
        }
        capture_ = nullptr;
    }
    pending_ = Pending{};
    const QString key = rowKey(item);
    if (!key.isEmpty()) {
        pending_.key = key;
        pending_.binding = rowBinding(item);
        capture_ = new ShortcutCaptureField;
        capture_->onCaptured = [this](const QKeySequence& shortcut) { capture(shortcut); };
        capture_->setText(item->text(1));
        tree_->setItemWidget(item, 1, capture_);
        capture_->setFocus(Qt::OtherFocusReason);
    }
    showInfo();
    refreshButtons();
}

void KeyboardShortcutsDialog::capture(const QKeySequence& shortcut)
{
    if (pending_.key.isEmpty()) {
        return;
    }
    if (const WorkspaceKey* reserved = workspaceKeyFor(shortcut)) {
        setNotice(tr("%1 is used by the workspace (%2) and cannot be assigned.")
                      .arg(shortcut.toString(QKeySequence::NativeText), tr(reserved->use)),
                  true);
        return;
    }
    // CS6 tool shortcuts are a single unmodified letter.
    if (scope_ == Scope::Tools
        && (toolLetterFor(shortcut).isNull()
            || shortcut[0].keyboardModifiers() != Qt::NoModifier)) {
        setNotice(tr("Tool shortcuts must be a single letter, A through Z."), true);
        return;
    }
    pending_.value = shortcut;
    pending_.dirty = true;
    findConflict();
    capture_->setText(shortcut.toString(QKeySequence::NativeText));
    showInfo();
    refreshButtons();
}

void KeyboardShortcutsDialog::findConflict()
{
    pending_.conflictKey.clear();
    pending_.losingCommands.clear();
    pending_.losingTools.clear();
    const QChar letter = toolLetterFor(pending_.value);
    int ownGroup = -1;
    if (scope_ == Scope::Tools) {
        ToolId tool;
        if (!toolForName(pending_.key, &tool)) {
            return;
        }
        ownGroup = toolInfo(tool).group;
    }
    // A command holding the chord, or (from the Tools list) the letter or its
    // Shift+letter cycle key.
    const QKeySequence shifted =
        letter.isNull() ? QKeySequence() : QKeySequence(Qt::SHIFT | Qt::Key(letter.unicode()));
    for (const CommandInfo& info : registry_->describe()) {
        if (scope_ == Scope::ApplicationMenus && info.id == pending_.key) {
            continue;
        }
        const QList<QKeySequence>& bindings = working_.commands.value(info.id);
        if (bindings.contains(pending_.value)
            || (scope_ == Scope::Tools && bindings.contains(shifted))) {
            pending_.losingCommands << info.id;
        }
    }
    // A letter selects one group, so another group holding it loses it; the
    // edited tool's own group may share it.
    if (!letter.isNull()) {
        for (ToolId id : allToolIds()) {
            if (working_.tools.value(int(id)) == letter && toolInfo(id).group != ownGroup) {
                pending_.losingTools << int(id);
            }
        }
    }
    if (!pending_.losingCommands.isEmpty()) {
        pending_.conflictKey = pending_.losingCommands.first();
        pending_.conflictScope = Scope::ApplicationMenus;
    } else if (!pending_.losingTools.isEmpty()) {
        pending_.conflictKey = toolIdName(ToolId(pending_.losingTools.first()));
        pending_.conflictScope = Scope::Tools;
    }
}

bool KeyboardShortcutsDialog::commitPending()
{
    if (!pending_.dirty) {
        return false;
    }
    pushUndo();
    const QChar letter = toolLetterFor(pending_.value);
    for (int loser : std::as_const(pending_.losingTools)) {
        working_.tools.insert(loser, QChar());
    }
    for (const QString& loser : std::as_const(pending_.losingCommands)) {
        QList<QKeySequence>& bindings = working_.commands[loser];
        bindings.removeAll(pending_.value);
        if (scope_ == Scope::Tools) {
            bindings.removeAll(QKeySequence(Qt::SHIFT | Qt::Key(letter.unicode())));
        }
    }
    if (scope_ == Scope::Tools) {
        ToolId tool;
        if (toolForName(pending_.key, &tool)) {
            working_.tools.insert(int(tool), letter);
        }
    } else {
        QList<QKeySequence>& bindings = working_.commands[pending_.key];
        // The same chord on another of this command's rows is a duplicate.
        const qsizetype existing = bindings.indexOf(pending_.value);
        if (pending_.binding < bindings.size()) {
            bindings[pending_.binding] = pending_.value;
        } else {
            bindings.append(pending_.value);
        }
        if (existing >= 0 && existing != pending_.binding) {
            bindings.removeAt(existing);
        }
    }
    if (addingKey_ == pending_.key) {
        addingKey_.clear();
    }
    pending_.dirty = false;
    if (QTreeWidgetItem* row = rowFor(pending_.key, pending_.binding)) {
        row->setText(1, pending_.value.toString(QKeySequence::NativeText));
    }
    return true;
}

void KeyboardShortcutsDialog::discardPending()
{
    if (!pending_.dirty) {
        return;
    }
    pending_.dirty = false;
    pending_.conflictKey.clear();
    pending_.losingCommands.clear();
    pending_.losingTools.clear();
    if (capture_) {
        if (QTreeWidgetItem* row = rowFor(pending_.key, pending_.binding)) {
            capture_->setText(row->text(1));
        }
    }
    showInfo();
    refreshButtons();
}

void KeyboardShortcutsDialog::acceptPending(bool goToConflict)
{
    const QString key = pending_.key;
    const int binding = pending_.binding;
    const QString conflict = pending_.conflictKey;
    const Scope conflictScope = pending_.conflictScope;
    if (!commitPending()) {
        return;
    }
    // A steal removes a row elsewhere, so the list is rebuilt; a conflict in
    // the other list switches to it.
    if (goToConflict && !conflict.isEmpty()) {
        if (conflictScope != scope_) {
            setScopeForTest(conflictScope);
        }
        reselect(conflict, 0);
    } else {
        reselect(key, binding);
    }
}

void KeyboardShortcutsDialog::reselect(const QString& key, int binding)
{
    buildRows();
    QTreeWidgetItem* item = rowFor(key, binding);
    if (!item) {
        item = rowFor(key, 0);
    }
    if (item) {
        // The cleared tree has no current row, so this opens the editor.
        tree_->setCurrentItem(item);
        tree_->scrollToItem(item);
    }
}

void KeyboardShortcutsDialog::pushUndo() { undoStack_.append(working_); }

void KeyboardShortcutsDialog::undo()
{
    if (pending_.dirty) {
        discardPending();
        return;
    }
    if (undoStack_.isEmpty()) {
        return;
    }
    const QString key = pending_.key;
    const int binding = pending_.binding;
    working_ = undoStack_.takeLast();
    reselect(key, binding);
}

void KeyboardShortcutsDialog::useDefault()
{
    const QString key = pending_.key;
    if (key.isEmpty()) {
        return;
    }
    ToolId tool = ToolId::Move;
    if (scope_ == Scope::Tools && !toolForName(key, &tool)) {
        return;
    }
    pushUndo();
    // The defaults come back even where another command or tool took them
    // since; each one goes through the same steal as an accepted chord.
    const QChar letter = scope_ == Scope::Tools ? toolInfo(tool).shortcut : QChar();
    const QList<QKeySequence> defaults =
        scope_ == Scope::Tools
            ? (letter.isNull() ? QList<QKeySequence>() : QList<QKeySequence>{QKeySequence(QString(letter))})
            : registry_->defaultShortcuts(key);
    for (const QKeySequence& chord : defaults) {
        pending_.value = chord;
        findConflict();
        for (int loser : std::as_const(pending_.losingTools)) {
            working_.tools.insert(loser, QChar());
        }
        const QChar chordLetter = toolLetterFor(chord);
        for (const QString& loser : std::as_const(pending_.losingCommands)) {
            working_.commands[loser].removeAll(chord);
            if (scope_ == Scope::Tools) {
                working_.commands[loser].removeAll(
                    QKeySequence(Qt::SHIFT | Qt::Key(chordLetter.unicode())));
            }
        }
    }
    if (scope_ == Scope::Tools) {
        working_.tools.insert(int(tool), letter);
    } else {
        working_.commands.insert(key, defaults);
    }
    addingKey_.clear();
    reselect(key, 0);
}

void KeyboardShortcutsDialog::addShortcut()
{
    const QString key = pending_.key;
    if (key.isEmpty()) {
        return;
    }
    if (scope_ == Scope::Tools) {
        if (capture_) {
            capture_->setFocus(Qt::OtherFocusReason);
        }
        return;
    }
    commitPending();
    addingKey_ = key;
    reselect(key, int(working_.commands.value(key).size()));
}

void KeyboardShortcutsDialog::deleteShortcut()
{
    const QString key = pending_.key;
    const int binding = pending_.binding;
    if (key.isEmpty() || pending_.dirty) {
        return;
    }
    pushUndo();
    if (scope_ == Scope::Tools) {
        ToolId tool;
        if (toolForName(key, &tool)) {
            working_.tools.insert(int(tool), QChar());
        }
    } else {
        QList<QKeySequence>& bindings = working_.commands[key];
        if (binding < bindings.size()) {
            bindings.removeAt(binding);
        }
    }
    reselect(key, qMax(0, binding - 1));
}

void KeyboardShortcutsDialog::applyAndAccept()
{
    // OK keeps a chord still pending in the field, as Accept would.
    commitPending();
    for (auto it = working_.commands.cbegin(); it != working_.commands.cend(); ++it) {
        if (registry_->shortcuts(it.key()) != it.value()) {
            registry_->setShortcuts(it.key(), it.value());
        }
    }
    for (auto it = working_.tools.cbegin(); it != working_.tools.cend(); ++it) {
        setToolShortcut(ToolId(it.key()), it.value());
    }
    accept();
}

void KeyboardShortcutsDialog::setScopeForTest(Scope scope)
{
    scopeCombo_->setCurrentIndex(scope == Scope::Tools ? 2 : 0);
}

bool KeyboardShortcutsDialog::selectRowForTest(const QString& key, int binding)
{
    QTreeWidgetItem* item = rowFor(key, binding);
    if (item) {
        tree_->setCurrentItem(item);
    }
    return item != nullptr;
}

QString KeyboardShortcutsDialog::selectedRowForTest() const
{
    return rowKey(tree_->currentItem());
}

QStringList KeyboardShortcutsDialog::rowShortcutsForTest(const QString& key) const
{
    QStringList texts;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        if (rowKey(*it) == key) {
            texts << QKeySequence((*it)->text(1), QKeySequence::NativeText)
                         .toString(QKeySequence::PortableText);
        }
    }
    return texts;
}

QString KeyboardShortcutsDialog::rowShortcutForTest(const QString& key) const
{
    const QStringList texts = rowShortcutsForTest(key);
    return texts.isEmpty() ? QString() : texts.first();
}

QWidget* KeyboardShortcutsDialog::captureFieldForTest() const { return capture_; }

QPushButton* KeyboardShortcutsDialog::buttonForTest(const QString& objectName) const
{
    return findChild<QPushButton*>(objectName);
}

QString KeyboardShortcutsDialog::infoTextForTest() const { return infoText_->text(); }

} // namespace pictura

#include "moc_keyboard_shortcuts_dialog.cpp"
