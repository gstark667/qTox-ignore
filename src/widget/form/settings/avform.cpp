/*
    Copyright © 2014-2016 by The qTox Project Contributors

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "avform.h"

#include <cassert>
#include <map>

#include <QDebug>
#include <QDesktopWidget>
#include <QScreen>
#include <QShowEvent>

#include "src/audio/audio.h"
#include "src/core/core.h"
#include "src/core/coreav.h"
#include "src/core/recursivesignalblocker.h"
#include "src/persistence/settings.h"
#include "src/video/cameradevice.h"
#include "src/video/camerasource.h"
#include "src/video/videosurface.h"
#include "src/widget/tool/screenshotgrabber.h"
#include "src/widget/translator.h"

#ifndef ALC_ALL_DEVICES_SPECIFIER
#define ALC_ALL_DEVICES_SPECIFIER ALC_DEVICE_SPECIFIER
#endif

AVForm::AVForm()
    : GenericForm(QPixmap(":/img/settings/av.png"))
    , subscribedToAudioIn(false)
    , camVideoSurface(nullptr)
    , camera(CameraSource::getInstance())
{
    setupUi(this);

    // block all child signals during initialization
    const RecursiveSignalBlocker signalBlocker(this);

    const Audio& audio = Audio::getInstance();
    const Settings& s = Settings::getInstance();

    cbEnableTestSound->setChecked(s.getEnableTestSound());

    cbEnableTestSound->setToolTip(tr("Play a test sound while changing the output volume."));

#ifndef USE_FILTERAUDIO
    cbEnableBackend2->setVisible(false);
#endif
    cbEnableBackend2->setChecked(s.getEnableBackend2());

    connect(rescanButton, &QPushButton::clicked, this, &AVForm::rescanDevices);

    playbackSlider->setTracking(false);
    playbackSlider->setValue(s.getOutVolume());
    playbackSlider->installEventFilter(this);

    microphoneSlider->setToolTip(tr("Use slider to set the gain of your input device ranging"
                                    " from %1dB to %2dB.")
                                     .arg(audio.minInputGain())
                                     .arg(audio.maxInputGain()));
    microphoneSlider->setMinimum(qRound(audio.minInputGain()) * 10);
    microphoneSlider->setMaximum(qRound(audio.maxInputGain()) * 10);
    microphoneSlider->setTickPosition(QSlider::TicksBothSides);
    microphoneSlider->setTickInterval(
        (qAbs(microphoneSlider->minimum()) + microphoneSlider->maximum()) / 4);
    microphoneSlider->setTracking(false);
    microphoneSlider->installEventFilter(this);

    voiceHoldSlider->setToolTip(tr("Use slider to set how long audio will be active for after"
                                    " audio drops below the threshold. Ranges from %1 to %2 milliseconds.")
                                      .arg(audio.getMinVoiceHold())
                                      .arg(audio.getMaxVoiceHold()));
    voiceHoldSlider->setMinimum(audio.getMinVoiceHold());
    voiceHoldSlider->setMaximum(audio.getMaxVoiceHold());
    voiceHoldSlider->setValue(s.getVoiceHold());
    voiceHoldSlider->setTickPosition(QSlider::TicksBothSides);
    voiceHoldSlider->setTickInterval(
        (qAbs(voiceHoldSlider->minimum()) + voiceHoldSlider->maximum()) / 4);
    voiceHoldSlider->setTracking(false);
    voiceHoldSlider->installEventFilter(this);

    audioThresholdSlider->setToolTip(tr("Use slider to set the activation volume for your"
                                    " input device."));
    audioThresholdSlider->setMinimum(audio.minInputThreshold() * 1000);
    audioThresholdSlider->setMaximum(audio.maxInputThreshold() * 1000);
    audioThresholdSlider->setValue(s.getAudioThreshold() * 1000);
    audioThresholdSlider->setTracking(false);
    audioThresholdSlider->installEventFilter(this);

    connect(&audio, &Audio::volumeAvailable, this, &AVForm::setVolume);
    volumeDisplay->setMinimum(audio.minInputThreshold() * 1000);
    volumeDisplay->setMaximum(audio.maxInputThreshold() * 1000);

    fillAudioQualityComboBox();

    eventsInit();

    QDesktopWidget* desktop = QApplication::desktop();
    connect(desktop, &QDesktopWidget::resized, this, &AVForm::rescanDevices);
    connect(desktop, &QDesktopWidget::screenCountChanged, this, &AVForm::rescanDevices);

    Translator::registerHandler(std::bind(&AVForm::retranslateUi, this), this);
}

AVForm::~AVForm()
{
    killVideoSurface();
    Translator::unregister(this);
}

void AVForm::hideEvent(QHideEvent* event)
{
    if (subscribedToAudioIn) {
        // TODO: This should not be done in show/hide events
        Audio::getInstance().unsubscribeInput();
        subscribedToAudioIn = false;
    }

    if (camVideoSurface) {
        camVideoSurface->setSource(nullptr);
        killVideoSurface();
    }
    videoDeviceList.clear();

    GenericForm::hideEvent(event);
}

void AVForm::showEvent(QShowEvent* event)
{
    getAudioOutDevices();
    getAudioInDevices();
    createVideoSurface();
    getVideoDevices();

    if (!subscribedToAudioIn) {
        // TODO: This should not be done in show/hide events
        Audio::getInstance().subscribeInput();
        subscribedToAudioIn = true;
    }

    GenericForm::showEvent(event);
}

void AVForm::open(const QString& devName, const VideoMode& mode)
{
    QRect rect = mode.toRect();
    Settings::getInstance().setCamVideoRes(rect);
    Settings::getInstance().setCamVideoFPS(static_cast<quint16>(mode.FPS));
    camera.setupDevice(devName, mode);
}

void AVForm::rescanDevices()
{
    getAudioInDevices();
    getAudioOutDevices();
    getVideoDevices();
}

void AVForm::setVolume(float value)
{
    volumeDisplay->setValue(value * 1000);
}

void AVForm::on_cbEnableBackend2_stateChanged()
{
    Settings::getInstance().setEnableBackend2(cbEnableBackend2->isChecked());
}

void AVForm::on_videoModescomboBox_currentIndexChanged(int index)
{
    assert(0 <= index && index < videoModes.size());
    int devIndex = videoDevCombobox->currentIndex();
    assert(0 <= devIndex && devIndex < videoDeviceList.size());

    QString devName = videoDeviceList[devIndex].first;
    VideoMode mode = videoModes[index];

    if (CameraDevice::isScreen(devName) && mode == VideoMode()) {
        if (Settings::getInstance().getScreenGrabbed()) {
            VideoMode mode(Settings::getInstance().getScreenRegion());
            open(devName, mode);
            return;
        }

        // note: grabber is self-managed and will destroy itself when done
        ScreenshotGrabber* screenshotGrabber = new ScreenshotGrabber;

        auto onGrabbed = [screenshotGrabber, devName, this](QRect region) {
            VideoMode mode(region);
            mode.width = mode.width / 2 * 2;
            mode.height = mode.height / 2 * 2;

            // Needed, if the virtual screen origin is the top left corner of the primary screen
            QRect screen = QApplication::primaryScreen()->virtualGeometry();
            mode.x += screen.x();
            mode.y += screen.y();

            Settings::getInstance().setScreenRegion(mode.toRect());
            Settings::getInstance().setScreenGrabbed(true);

            open(devName, mode);
        };

        connect(screenshotGrabber, &ScreenshotGrabber::regionChosen, this, onGrabbed,
                Qt::QueuedConnection);
        screenshotGrabber->showGrabber();
        return;
    }

    Settings::getInstance().setScreenGrabbed(false);
    open(devName, mode);
}

void AVForm::selectBestModes(QVector<VideoMode>& allVideoModes)
{
    // Identify the best resolutions available for the supposed XXXXp resolutions.
    std::map<int, VideoMode> idealModes;
    idealModes[120] = VideoMode(160, 120);
    idealModes[240] = VideoMode(430, 240);
    idealModes[360] = VideoMode(640, 360);
    idealModes[480] = VideoMode(854, 480);
    idealModes[720] = VideoMode(1280, 720);
    idealModes[1080] = VideoMode(1920, 1080);

    std::map<int, int> bestModeInds;
    for (int i = 0; i < allVideoModes.size(); ++i) {
        VideoMode mode = allVideoModes[i];

        // PS3-Cam protection, everything above 60fps makes no sense
        if (mode.FPS > 60)
            continue;

        for (auto iter = idealModes.begin(); iter != idealModes.end(); ++iter) {
            int res = iter->first;
            VideoMode idealMode = iter->second;
            // don't take approximately correct resolutions unless they really
            // are close
            if (mode.norm(idealMode) > 300)
                continue;

            if (bestModeInds.find(res) == bestModeInds.end()) {
                bestModeInds[res] = i;
                continue;
            }

            int index = bestModeInds[res];
            VideoMode best = allVideoModes[index];
            if (mode.norm(idealMode) < best.norm(idealMode)) {
                bestModeInds[res] = i;
                continue;
            }

            if (mode.norm(idealMode) == best.norm(idealMode)) {
                // prefer higher FPS and "better" pixel formats
                if (mode.FPS > best.FPS) {
                    bestModeInds[res] = i;
                    continue;
                }

                bool better = CameraDevice::betterPixelFormat(mode.pixel_format, best.pixel_format);
                if (mode.FPS >= best.FPS && better)
                    bestModeInds[res] = i;
            }
        }
    }

    QVector<VideoMode> newVideoModes;
    for (auto it = bestModeInds.rbegin(); it != bestModeInds.rend(); ++it) {
        VideoMode mode = allVideoModes[it->second];

        if (newVideoModes.empty()) {
            newVideoModes.push_back(mode);
        } else {
            int size = getModeSize(mode);
            auto result = std::find_if(newVideoModes.cbegin(), newVideoModes.cend(),
                                       [size](VideoMode mode) { return getModeSize(mode) == size; });

            if (result == newVideoModes.end())
                newVideoModes.push_back(mode);
        }
    }
    allVideoModes = newVideoModes;
}

void AVForm::fillCameraModesComboBox()
{
    qDebug() << "selected Modes:";
    bool previouslyBlocked = videoModescomboBox->blockSignals(true);
    videoModescomboBox->clear();

    for (int i = 0; i < videoModes.size(); ++i) {
        VideoMode mode = videoModes[i];

        QString str;
        QString pixelFormat = CameraDevice::getPixelFormatString(mode.pixel_format);
        qDebug("width: %d, height: %d, FPS: %f, pixel format: %s\n", mode.width, mode.height,
               mode.FPS, pixelFormat.toStdString().c_str());

        if (mode.height && mode.width)
            str += QString("%1p").arg(mode.height);
        else
            str += tr("Default resolution");

        videoModescomboBox->addItem(str);
    }

    if (videoModes.isEmpty())
        videoModescomboBox->addItem(tr("Default resolution"));

    videoModescomboBox->blockSignals(previouslyBlocked);
}

int AVForm::searchPreferredIndex()
{
    QRect prefRes = Settings::getInstance().getCamVideoRes();
    quint16 prefFPS = Settings::getInstance().getCamVideoFPS();

    for (int i = 0; i < videoModes.size(); ++i) {
        VideoMode mode = videoModes[i];
        if (mode.width == prefRes.width() && mode.height == prefRes.height()
            && static_cast<quint16>(mode.FPS) == prefFPS)
            return i;
    }

    return -1;
}

void AVForm::fillScreenModesComboBox()
{
    bool previouslyBlocked = videoModescomboBox->blockSignals(true);
    videoModescomboBox->clear();

    for (int i = 0; i < videoModes.size(); ++i) {
        VideoMode mode = videoModes[i];
        QString pixelFormat = CameraDevice::getPixelFormatString(mode.pixel_format);
        qDebug("%dx%d+%d,%d FPS: %f, pixel format: %s\n", mode.width, mode.height, mode.x, mode.y,
               mode.FPS, pixelFormat.toStdString().c_str());

        QString name;
        if (mode.width && mode.height)
            name = tr("Screen %1").arg(i + 1);
        else
            name = tr("Select region");

        videoModescomboBox->addItem(name);
    }

    videoModescomboBox->blockSignals(previouslyBlocked);
}

void AVForm::fillAudioQualityComboBox()
{
    const bool previouslyBlocked = audioQualityComboBox->blockSignals(true);

    audioQualityComboBox->addItem(tr("High (64 kbps)"), 64);
    audioQualityComboBox->addItem(tr("Medium (32 kbps)"), 32);
    audioQualityComboBox->addItem(tr("Low (16 kbps)"), 16);
    audioQualityComboBox->addItem(tr("Very low (8 kbps)"), 8);

    const int currentBitrate = Settings::getInstance().getAudioBitrate();
    const int index = audioQualityComboBox->findData(currentBitrate);

    audioQualityComboBox->setCurrentIndex(index);
    audioQualityComboBox->blockSignals(previouslyBlocked);
}

void AVForm::updateVideoModes(int curIndex)
{
    if (curIndex < 0 || curIndex >= videoDeviceList.size()) {
        qWarning() << "Invalid index:" << curIndex;
        return;
    }
    QString devName = videoDeviceList[curIndex].first;
    QVector<VideoMode> allVideoModes = CameraDevice::getVideoModes(devName);

    qDebug("available Modes:");
    bool isScreen = CameraDevice::isScreen(devName);
    if (isScreen) {
        // Add extra video mode to region selection
        allVideoModes.push_back(VideoMode());
        videoModes = allVideoModes;
        fillScreenModesComboBox();
    } else {
        selectBestModes(allVideoModes);
        videoModes = allVideoModes;
        fillCameraModesComboBox();
    }

    int preferedIndex = searchPreferredIndex();
    if (preferedIndex != -1) {
        Settings::getInstance().setScreenGrabbed(false);
        videoModescomboBox->blockSignals(true);
        videoModescomboBox->setCurrentIndex(preferedIndex);
        videoModescomboBox->blockSignals(false);
        open(devName, videoModes[preferedIndex]);
        return;
    }

    if (isScreen) {
        QRect rect = Settings::getInstance().getScreenRegion();
        VideoMode mode(rect);

        Settings::getInstance().setScreenGrabbed(true);
        videoModescomboBox->setCurrentIndex(videoModes.size() - 1);
        open(devName, mode);
        return;
    }

    // If the user hasn't set a preferred resolution yet,
    // we'll pick the resolution in the middle of the list,
    // and the best FPS for that resolution.
    // If we picked the lowest resolution, the quality would be awful
    // but if we picked the largest, FPS would be bad and thus quality bad too.
    int mid = (videoModes.size() - 1) / 2;
    videoModescomboBox->setCurrentIndex(mid);
}

void AVForm::on_videoDevCombobox_currentIndexChanged(int index)
{
    assert(0 <= index && index < videoDeviceList.size());

    Settings::getInstance().setScreenGrabbed(false);
    QString dev = videoDeviceList[index].first;
    Settings::getInstance().setVideoDev(dev);
    bool previouslyBlocked = videoModescomboBox->blockSignals(true);
    updateVideoModes(index);
    videoModescomboBox->blockSignals(previouslyBlocked);

    if (Settings::getInstance().getScreenGrabbed())
        return;

    int modeIndex = videoModescomboBox->currentIndex();
    VideoMode mode = VideoMode();
    if (0 < modeIndex && modeIndex < videoModes.size())
        mode = videoModes[modeIndex];

    camera.setupDevice(dev, mode);
    if (dev == "none")
        Core::getInstance()->getAv()->sendNoVideo();
}

void AVForm::on_audioQualityComboBox_currentIndexChanged(int index)
{
    Settings::getInstance().setAudioBitrate(audioQualityComboBox->currentData().toInt());
}

void AVForm::getVideoDevices()
{
    QString settingsInDev = Settings::getInstance().getVideoDev();
    int videoDevIndex = 0;
    videoDeviceList = CameraDevice::getDeviceList();
    // prevent currentIndexChanged to be fired while adding items
    videoDevCombobox->blockSignals(true);
    videoDevCombobox->clear();
    for (QPair<QString, QString> device : videoDeviceList) {
        videoDevCombobox->addItem(device.second);
        if (device.first == settingsInDev)
            videoDevIndex = videoDevCombobox->count() - 1;
    }
    videoDevCombobox->setCurrentIndex(videoDevIndex);
    videoDevCombobox->blockSignals(false);
    updateVideoModes(videoDevIndex);
}

int AVForm::getModeSize(VideoMode mode)
{
    return qRound(mode.height / 120.0) * 120;
}

void AVForm::getAudioInDevices()
{
    QStringList deviceNames;
    deviceNames << tr("Disabled") << Audio::getInstance().inDeviceNames();

    inDevCombobox->blockSignals(true);
    inDevCombobox->clear();
    inDevCombobox->addItems(deviceNames);
    inDevCombobox->blockSignals(false);

    int idx = 0;
    bool enabled = Settings::getInstance().getAudioInDevEnabled();
    if (enabled && deviceNames.size() > 1) {
        QString dev = Settings::getInstance().getInDev();
        idx = qMax(deviceNames.indexOf(dev), 1);
    }
    inDevCombobox->setCurrentIndex(idx);
}

void AVForm::getAudioOutDevices()
{
    QStringList deviceNames;
    deviceNames << tr("Disabled") << Audio::getInstance().outDeviceNames();

    outDevCombobox->blockSignals(true);
    outDevCombobox->clear();
    outDevCombobox->addItems(deviceNames);
    outDevCombobox->blockSignals(false);

    int idx = 0;
    bool enabled = Settings::getInstance().getAudioOutDevEnabled();
    if (enabled && deviceNames.size() > 1) {
        QString dev = Settings::getInstance().getOutDev();
        idx = qMax(deviceNames.indexOf(dev), 1);
    }
    outDevCombobox->setCurrentIndex(idx);
}

void AVForm::on_inDevCombobox_currentIndexChanged(int deviceIndex)
{
    Settings::getInstance().setAudioInDevEnabled(deviceIndex != 0);

    QString deviceName;
    if (deviceIndex > 0)
        deviceName = inDevCombobox->itemText(deviceIndex);

    Settings::getInstance().setInDev(deviceName);

    Audio& audio = Audio::getInstance();
    audio.reinitInput(deviceName);
    microphoneSlider->setEnabled(deviceIndex > 0);
    microphoneSlider->setSliderPosition(qRound(audio.inputGain() * 10.0));
}

void AVForm::on_outDevCombobox_currentIndexChanged(int deviceIndex)
{
    Settings::getInstance().setAudioOutDevEnabled(deviceIndex != 0);

    QString deviceName;
    if (deviceIndex > 0)
        deviceName = outDevCombobox->itemText(deviceIndex);

    Settings::getInstance().setOutDev(deviceName);

    Audio& audio = Audio::getInstance();
    audio.reinitOutput(deviceName);
    playbackSlider->setEnabled(deviceIndex > 0);
    playbackSlider->setSliderPosition(qRound(audio.outputVolume() * 100.0));
}

void AVForm::on_playbackSlider_valueChanged(int value)
{
    Settings::getInstance().setOutVolume(value);

    Audio& audio = Audio::getInstance();
    if (audio.isOutputReady()) {
        const qreal percentage = value / 100.0;
        audio.setOutputVolume(percentage);

        if (cbEnableTestSound->isChecked())
            audio.playMono16Sound(Audio::getSound(Audio::Sound::Test));
    }
}

void AVForm::on_cbEnableTestSound_stateChanged()
{
    Settings::getInstance().setEnableTestSound(cbEnableTestSound->isChecked());

    Audio& audio = Audio::getInstance();
    if (cbEnableTestSound->isChecked() && audio.isOutputReady())
        audio.playMono16Sound(Audio::getSound(Audio::Sound::Test));
}

void AVForm::on_microphoneSlider_valueChanged(int value)
{
    const qreal dB = value / 10.0;

    Settings::getInstance().setAudioInGainDecibel(dB);
    Audio::getInstance().setInputGain(dB);
}

void AVForm::on_audioThresholdSlider_valueChanged(int value)
{
    const qreal percent = value / 1000.0;

    Settings::getInstance().setAudioThreshold(percent);
    Audio::getInstance().setInputThreshold(percent);
}

void AVForm::on_voiceHoldSlider_valueChanged(int value)
{
    Settings::getInstance().setVoiceHold(value);
    Audio::getInstance().setVoiceHold(value);
}

void AVForm::createVideoSurface()
{
    if (camVideoSurface)
        return;

    camVideoSurface = new VideoSurface(QPixmap(), CamFrame);
    camVideoSurface->setObjectName(QStringLiteral("CamVideoSurface"));
    camVideoSurface->setMinimumSize(QSize(160, 120));
    camVideoSurface->setSource(&camera);
    gridLayout->addWidget(camVideoSurface, 0, 0, 1, 1);
}

void AVForm::killVideoSurface()
{
    if (!camVideoSurface)
        return;

    QLayoutItem* child;
    while ((child = gridLayout->takeAt(0)) != 0)
        delete child;

    camVideoSurface->close();
    delete camVideoSurface;
    camVideoSurface = nullptr;
}

void AVForm::retranslateUi()
{
    Ui::AVForm::retranslateUi(this);
}
