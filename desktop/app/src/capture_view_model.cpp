#include "usn/app/viewmodels/capture_view_model.h"

namespace usn::app {

CaptureViewModel::CaptureViewModel(QObject* parent) : QObject(parent) {}

void CaptureViewModel::setSampleRateHz(quint64 rate) {
    if (m_sampleRateHz != rate) {
        m_sampleRateHz = rate;
        emit sampleRateHzChanged();
    }
}

void CaptureViewModel::setChannelCount(int count) {
    if (m_channelCount != count) {
        m_channelCount = count;
        emit channelCountChanged();
    }
}

void CaptureViewModel::startCapture() {
    if (m_isCapturing) {
        return;
    }
    m_isCapturing = true;
    m_stateString = QStringLiteral("Capturing");
    m_samplesCaptured = 0;
    emit isCapturingChanged();
    emit stateStringChanged();
    emit samplesCapturedChanged();
}

void CaptureViewModel::stopCapture() {
    if (!m_isCapturing) {
        return;
    }
    m_isCapturing = false;
    m_stateString = QStringLiteral("Stopped");
    emit isCapturingChanged();
    emit stateStringChanged();
}

}  // namespace usn::app
