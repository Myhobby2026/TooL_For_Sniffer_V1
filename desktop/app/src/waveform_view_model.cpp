#include "usn/app/viewmodels/waveform_view_model.h"

#include <algorithm>

namespace usn::app {

WaveformViewModel::WaveformViewModel(QObject* parent) : QObject(parent) {}

void WaveformViewModel::setVisibleStartSample(double start) {
    start = std::max(0.0, start);
    if (m_visibleStartSample != start) {
        m_visibleStartSample = start;
        emit visibleRangeChanged();
    }
}

void WaveformViewModel::setVisibleSpanSamples(double span) {
    span = std::max(10.0, span);
    if (m_visibleSpanSamples != span) {
        m_visibleSpanSamples = span;
        emit visibleRangeChanged();
    }
}

void WaveformViewModel::zoomAt(double anchorFraction, double factor) {
    anchorFraction = std::clamp(anchorFraction, 0.0, 1.0);
    if (factor <= 0.0) {
        return;
    }

    double oldSpan = m_visibleSpanSamples;
    double newSpan = std::max(10.0, oldSpan / factor);
    double anchorSample = m_visibleStartSample + anchorFraction * oldSpan;
    double newStart = std::max(0.0, anchorSample - anchorFraction * newSpan);

    m_visibleStartSample = newStart;
    m_visibleSpanSamples = newSpan;
    emit visibleRangeChanged();
}

void WaveformViewModel::panBySamples(double deltaSamples) {
    double newStart = std::max(0.0, m_visibleStartSample + deltaSamples);
    if (newStart != m_visibleStartSample) {
        m_visibleStartSample = newStart;
        emit visibleRangeChanged();
    }
}

void WaveformViewModel::resetView() {
    m_visibleStartSample = 0.0;
    m_visibleSpanSamples = 1000.0;
    emit visibleRangeChanged();
}

}  // namespace usn::app
