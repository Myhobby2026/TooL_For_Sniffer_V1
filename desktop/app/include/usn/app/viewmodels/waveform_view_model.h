// -----------------------------------------------------------------------------
// waveform_view_model.h -- QML ViewModel managing the waveform viewport.
// -----------------------------------------------------------------------------
#pragma once

#include <QObject>

namespace usn::app {

class WaveformViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(double visibleStartSample READ visibleStartSample WRITE setVisibleStartSample NOTIFY visibleRangeChanged)
    Q_PROPERTY(double visibleSpanSamples READ visibleSpanSamples WRITE setVisibleSpanSamples NOTIFY visibleRangeChanged)

public:
    explicit WaveformViewModel(QObject* parent = nullptr);
    ~WaveformViewModel() override = default;

    [[nodiscard]] double visibleStartSample() const noexcept { return m_visibleStartSample; }
    [[nodiscard]] double visibleSpanSamples() const noexcept { return m_visibleSpanSamples; }

    void setVisibleStartSample(double start);
    void setVisibleSpanSamples(double span);

public slots:
    void zoomAt(double anchorFraction, double factor);
    void panBySamples(double deltaSamples);
    void resetView();

signals:
    void visibleRangeChanged();

private:
    double m_visibleStartSample{0.0};
    double m_visibleSpanSamples{1000.0};
};

}  // namespace usn::app
