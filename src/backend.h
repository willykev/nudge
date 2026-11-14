#pragma once

#include "alerts.h"
#include "remindermodel.h"
#include "sidebarmodel.h"
#include "store.h"

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

// Everything the window does, as actions on the store and state for QML.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *model READ model CONSTANT)
    Q_PROPERTY(QAbstractItemModel *sidebar READ sidebar CONSTANT)
    Q_PROPERTY(QString viewKey READ viewKey WRITE setViewKey NOTIFY viewChanged)
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY viewChanged)
    Q_PROPERTY(QString viewTitle READ viewTitle NOTIFY viewChanged)
    Q_PROPERTY(QString viewColor READ viewColor NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewListId READ viewListId NOTIFY viewChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY viewChanged)
    Q_PROPERTY(int openCount READ openCount NOTIFY countsChanged)
    Q_PROPERTY(bool showCompleted READ showCompleted WRITE setShowCompleted NOTIFY showCompletedChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
    Q_PROPERTY(qint64 selectedId READ selectedId WRITE setSelectedId NOTIFY selectedIdChanged)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectedRowChanged)
    Q_PROPERTY(QVariantMap selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(qint64 draftId READ draftId NOTIFY draftChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QVariantList lists READ lists NOTIFY listsChanged)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY sectionsChanged)
    Q_PROPERTY(QStringList palette READ palette CONSTANT)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    Q_PROPERTY(bool backgroundAvailable READ backgroundAvailable NOTIFY backgroundChanged)
    Q_PROPERTY(bool backgroundOn READ backgroundOn NOTIFY backgroundChanged)
    Q_PROPERTY(bool backgroundAsk READ backgroundAsk NOTIFY backgroundChanged)

public:
    explicit Backend(Store *store, QObject *parent = nullptr);
    ~Backend() override;

    // Tests hold the clock still and stand in for systemd.
    void setClock(std::function<QDateTime()> clock);
    void setService(const QString &systemctl, const QString &unitFile);
    void attachAlerts(Alerts *alerts);
    Notifier *notifier() const { return m_notifier; }

    QAbstractItemModel *model() const { return m_model; }
    QAbstractItemModel *sidebar() const { return m_sidebar; }
    ReminderModel *reminders() const { return m_model; }
    SidebarModel *views() const { return m_sidebar; }

    QString viewKey() const { return m_view.key(); }
    void setViewKey(const QString &key);
    QString viewKind() const;
    QString viewTitle() const;
    QString viewColor() const;
    qint64 viewListId() const { return m_view.kind == View::List ? m_view.listId : 0; }
    QString sortMode() const;
    void setSortMode(const QString &mode);
    int openCount() const { return m_openCount; }
    bool showCompleted() const { return m_showCompleted; }
    void setShowCompleted(bool show);
    QString search() const { return m_search; }
    void setSearch(const QString &text);

    qint64 selectedId() const { return m_selectedId; }
    void setSelectedId(qint64 id);
    int selectedRow() const { return m_model->rowOf(m_selectedId); }
    QVariantMap selected() const;
    qint64 draftId() const { return m_draftId; }
    bool canUndo() const { return m_store->canUndo(); }
    bool canRedo() const { return m_store->canRedo(); }
    QVariantList lists() const;
    QVariantList sections() const;
    QStringList palette() const;
    QString problem() const;
    bool backgroundAvailable() const;
    bool backgroundOn() const { return m_backgroundOn; }
    bool backgroundAsk() const;

    // Moving around.
    Q_INVOKABLE void selectStep(int delta);
    Q_INVOKABLE void selectEdge(bool last);
    Q_INVOKABLE void stepView(int delta);
    Q_INVOKABLE void reveal(qint64 id);

    // Reminders.
    Q_INVOKABLE qint64 addReminder();
    Q_INVOKABLE void commitTitle(qint64 id, const QString &text);
    Q_INVOKABLE QString quickPreview(const QString &text) const;
    Q_INVOKABLE void toggleCompleted(qint64 id);
    Q_INVOKABLE void toggleFlag(qint64 id);
    Q_INVOKABLE void setPriority(qint64 id, int priority);
    Q_INVOKABLE void setDueIn(qint64 id, int days);
    Q_INVOKABLE void clearDue(qint64 id);
    Q_INVOKABLE void remove(qint64 id);
    Q_INVOKABLE void indent(qint64 id);
    Q_INVOKABLE void outdent(qint64 id);
    Q_INVOKABLE void moveReminder(qint64 id, int delta);
    Q_INVOKABLE void moveToList(qint64 id, qint64 listId);
    Q_INVOKABLE void moveToSection(qint64 id, qint64 sectionId);
    Q_INVOKABLE void toggleCollapsed(qint64 id);
    Q_INVOKABLE void openLink(qint64 id);

    // The details pane.
    Q_INVOKABLE void setTitle(qint64 id, const QString &text);
    Q_INVOKABLE void setNotes(qint64 id, const QString &text);
    Q_INVOKABLE void setUrl(qint64 id, const QString &text);
    Q_INVOKABLE bool setDate(qint64 id, const QString &text);
    Q_INVOKABLE void setDateIso(qint64 id, const QString &iso);
    Q_INVOKABLE bool setTime(qint64 id, const QString &text);
    Q_INVOKABLE void setRepeat(qint64 id, const QString &unit, int interval);
    Q_INVOKABLE bool setRepeatEnd(qint64 id, const QString &text);
    Q_INVOKABLE void setEarly(qint64 id, int minutes);
    Q_INVOKABLE void setFlagged(qint64 id, bool flagged);
    Q_INVOKABLE void setTags(qint64 id, const QString &text);

    // Lists and sections.
    Q_INVOKABLE qint64 addList(const QString &name, const QString &color);
    Q_INVOKABLE void updateList(qint64 id, const QString &name, const QString &color);
    Q_INVOKABLE void removeList(qint64 id);
    Q_INVOKABLE void moveList(qint64 id, int delta);
    Q_INVOKABLE QVariantMap listInfo(qint64 id) const;
    Q_INVOKABLE qint64 addSection(const QString &name);
    Q_INVOKABLE void renameSection(qint64 id, const QString &name);
    Q_INVOKABLE void removeSection(qint64 id);
    Q_INVOKABLE QString sectionName(qint64 id) const;
    Q_INVOKABLE int completedCount() const;
    Q_INVOKABLE void clearCompleted();

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void cycleSort();
    Q_INVOKABLE void setBackground(bool on);
    Q_INVOKABLE void dismissBackgroundAsk();
    void retranslate(); // the language changed: names made here need making again

signals:
    void viewChanged();
    void countsChanged();
    void showCompletedChanged();
    void searchChanged();
    void selectedIdChanged();
    void selectedRowChanged();
    void selectedChanged();
    void draftChanged();
    void undoChanged();
    void listsChanged();
    void sectionsChanged();
    void problemChanged();
    void backgroundChanged();
    void notice(const QString &text); // something worth a passing word
    void raise();                     // an alert was clicked: come to the front

private slots:
    void onPrepareForSleep(bool sleeping);

private:
    void tick();
    void refresh(bool reset = false);
    void updateSidebar();
    void selectNear(int row);
    void rememberSelection();
    bool edit(qint64 id, const QString &what, const std::function<void(Reminder &)> &change);
    QString stepKey(qint64 id, const QString &key = {}) const;
    QVector<qint64> completedInView() const;
    double positionAfter(const Reminder &reminder) const;
    double positionBefore(const Reminder &reminder) const;
    void spread(qint64 listId);
    void checkBackground();
    QString sortName(const QString &mode) const;
    QDateTime now() const { return m_clock(); }

    Store *m_store;
    ReminderModel *m_model;
    SidebarModel *m_sidebar;
    Notifier *m_notifier = nullptr;
    Alerts *m_alerts = nullptr;
    std::function<QDateTime()> m_clock;

    View m_view;
    View m_viewBeforeSearch;
    QString m_search;
    QString m_title, m_color, m_sort;
    QVariantList m_sections;
    bool m_showCompleted = false;
    int m_openCount = 0;
    qint64 m_selectedId = 0;
    int m_selectedRow = -1;
    qint64 m_draftId = 0;
    QString m_draftKey;
    int m_drafts = 0;
    QSet<qint64> m_lingering, m_collapsed;
    QTimer m_linger, m_tick;
    QDate m_day;
    QVector<TaskList> m_listsSeen;
    QVector<Section> m_sectionsSeen;
    qint64 m_beforeDraft = 0;
    bool m_backgroundAsked = false;

    QString m_systemctl = "systemctl";
    QString m_unitFile = "/usr/lib/systemd/user/nudge.service";
    bool m_backgroundOn = false;
};
