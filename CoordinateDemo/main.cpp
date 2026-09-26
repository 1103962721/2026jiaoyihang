#include <windows.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <psapi.h>

namespace
{
struct WindowItem
{
    HWND hwnd = nullptr;
    QString title;
    QString process;
};

QString processName(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]{};
    QString result;
    if (GetModuleFileNameExW(process, nullptr, path, MAX_PATH) > 0)
        result = QString::fromWCharArray(path).section(QChar('\\'), -1);
    CloseHandle(process);
    return result;
}

BOOL CALLBACK enumerateProc(HWND hwnd, LPARAM lParam)
{
    auto* items = reinterpret_cast<QVector<WindowItem>*>(lParam);
    if (!items || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    if (!title[0]) return TRUE;
    RECT client{};
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return TRUE;
    items->push_back({ hwnd, QString::fromWCharArray(title), processName(hwnd) });
    return TRUE;
}
}

class CoordinateWindow final : public QWidget
{
public:
    CoordinateWindow()
    {
        setWindowTitle(QStringLiteral("DF 客户区坐标取点 Demo"));
        resize(620, 360);
        auto* root = new QVBoxLayout(this);
        auto* row = new QHBoxLayout;
        windowBox_ = new QComboBox(this);
        auto* refresh = new QPushButton(QStringLiteral("刷新窗口"), this);
        auto* bind = new QPushButton(QStringLiteral("绑定选中窗口"), this);
        row->addWidget(windowBox_, 1); row->addWidget(refresh); row->addWidget(bind);
        root->addLayout(row);

        auto* form = new QFormLayout;
        screenLabel_ = addValue(form, QStringLiteral("屏幕坐标"));
        clientLabel_ = addValue(form, QStringLiteral("客户区坐标"));
        referenceLabel_ = addValue(form, QStringLiteral("2560×1440参考坐标"));
        sizeLabel_ = addValue(form, QStringLiteral("客户区尺寸"));
        hwndLabel_ = addValue(form, QStringLiteral("窗口句柄"));
        root->addLayout(form);

        auto* actionRow = new QHBoxLayout;
        auto* copyClient = new QPushButton(QStringLiteral("复制客户区坐标"), this);
        auto* copyReference = new QPushButton(QStringLiteral("复制参考坐标"), this);
        actionRow->addWidget(copyClient); actionRow->addWidget(copyReference);
        root->addLayout(actionRow);
        status_ = new QLabel(QStringLiteral("请刷新并绑定目标窗口"), this);
        status_->setWordWrap(true);
        root->addWidget(status_);

        connect(refresh, &QPushButton::clicked, this, &CoordinateWindow::refreshWindows);
        connect(bind, &QPushButton::clicked, this, &CoordinateWindow::bindSelected);
        connect(copyClient, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(clientText_); });
        connect(copyReference, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(referenceText_); });
        connect(&timer_, &QTimer::timeout, this, &CoordinateWindow::updateCoordinates);
        timer_.start(50);
        refreshWindows();
    }

private:
    QLabel* addValue(QFormLayout* form, const QString& name)
    {
        auto* value = new QLabel(QStringLiteral("-"), this);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(name, value);
        return value;
    }

    void refreshWindows()
    {
        items_.clear();
        EnumWindows(enumerateProc, reinterpret_cast<LPARAM>(&items_));
        windowBox_->clear();
        for (const auto& item : items_)
            windowBox_->addItem(QStringLiteral("%1 [%2]").arg(item.title, item.process));
        status_->setText(QStringLiteral("已发现 %1 个可选窗口").arg(items_.size()));
    }

    void bindSelected()
    {
        const int index = windowBox_->currentIndex();
        if (index < 0 || index >= items_.size()) return;
        hwnd_ = items_[index].hwnd;
        status_->setText(QStringLiteral("已绑定：%1，请将鼠标移动到目标位置").arg(items_[index].title));
        updateCoordinates();
    }

    void updateCoordinates()
    {
        if (!hwnd_ || !IsWindow(hwnd_)) return;
        POINT screen{};
        GetCursorPos(&screen);
        POINT client = screen;
        if (!ScreenToClient(hwnd_, &client)) return;
        RECT rect{};
        if (!GetClientRect(hwnd_, &rect)) return;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int refX = width > 0 ? client.x * 2560 / width : 0;
        const int refY = height > 0 ? client.y * 1440 / height : 0;
        screenLabel_->setText(QStringLiteral("(%1, %2)").arg(screen.x).arg(screen.y));
        clientText_ = QStringLiteral("(%1, %2)").arg(client.x).arg(client.y);
        referenceText_ = QStringLiteral("(%1, %2)").arg(refX).arg(refY);
        clientLabel_->setText(clientText_);
        referenceLabel_->setText(referenceText_);
        sizeLabel_->setText(QStringLiteral("%1 × %2").arg(width).arg(height));
        hwndLabel_->setText(QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(hwnd_), 0, 16));
    }

    QComboBox* windowBox_ = nullptr;
    QLabel* screenLabel_ = nullptr;
    QLabel* clientLabel_ = nullptr;
    QLabel* referenceLabel_ = nullptr;
    QLabel* sizeLabel_ = nullptr;
    QLabel* hwndLabel_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer timer_;
    QVector<WindowItem> items_;
    HWND hwnd_ = nullptr;
    QString clientText_;
    QString referenceText_;
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    CoordinateWindow window;
    window.show();
    return app.exec();
}
