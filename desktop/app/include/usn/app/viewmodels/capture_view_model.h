// -----------------------------------------------------------------------------
// capture_view_model.h -- QML ViewModel exposing capture session controls.
// -----------------------------------------------------------------------------
#pragma once

#include <QObject>
#include <QString>

namespace usn::core {
class CaptureSession;
}

namespace usn::app {

class CaptureViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isCapturing READ isCapturing NOTIFY isCapturingChanged)
    Q_PROPERTY(quint64 sampleRateHz READ sampleRateHz WRITE setSampleRateHz NOTIFY sampleRateHzChanged)
    Q_PROPERTY(int channelCount READ channelCount WRITE setChannelCount NOTIFY channelCountChanged)
    Q_PROPERTY(quint64 samplesCaptured READ samplesCaptured NOTIFY samplesCapturedChanged)
    Q_PROPERTY(QString stateString READ stateString NOTIFY stateStringChanged)

public:
    explicit CaptureViewModel(QObject* parent = nullptr);
    ~CaptureViewModel() override = default;

    [[nodiscard]] bool isCapturing() const noexcept { return m_isCapturing; }
    [[nodiscard]] quint64 sampleRateHz() const noexcept { return m_sampleRateHz; }
    [[nodiscard]] int channelCount() const noexcept { return m_channelCount; }
    [[nodiscard]] quint64 samplesCaptured() const noexcept { return m_samplesCaptured; }
    [[nodiscard]] QString stateString() const { return m_stateString; }

    void setSampleRateHz(quint64 rate);
    void setChannelCount(int count);

public slots:
    void startCapture();
    void stopCapture();

signals:
    void isCapturingChanged();
    void sampleRateHzChanged();
    void channelCountChanged();
    void samplesCapturedChanged();
    void stateStringChanged();
    void captureError(const QString& message);

private:
    bool m_isCapturing{false};
    quint64 m_sampleRateHz{1'000'000};
    int m_channelCount{8};
    quint64 m_samplesCaptured{0};
    QString m_stateString{QStringLiteral("Idle")};
};

}  // namespace usn::app
