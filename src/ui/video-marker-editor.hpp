#pragma once

#include "models/curation-settings.hpp"

#include <QMediaPlayer>
#include <QString>
#include <QVector>
#include <QWidget>

class QAudioOutput;
class QEvent;
class QLabel;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;
class QVideoWidget;
class TimelineWidget;
class PlaybackIconButton;

class VideoMarkerEditor : public QWidget {
	Q_OBJECT

public:
	explicit VideoMarkerEditor(const QString &videoPath, QWidget *parent = nullptr);

	QVector<ClipDuration> clipRanges() const;
	qint64 durationMilliseconds() const;
	qint64 positionMilliseconds() const;
	QString formatTimecode(double seconds) const;

public slots:
	void seekToSeconds(double seconds);
	void seekToMilliseconds(qint64 positionMs);
	void addMarkerAtCurrentPosition();
	void removeSelectedRange();
	void goToSelectedRange();
	void selectRange(int index);
	void togglePlayback();
	void toggleFullScreen();
	void exitFullScreen();
	void seekForwardTenSeconds();
	void seekBackwardTenSeconds();
	void seekForwardFrame();
	void seekBackwardFrame();
	QVector<double> markerPositions() const;
	void setMarkerPositions(const QVector<double> &markers);
	void setReviewActionVisible(bool visible);

signals:
	void rangesChanged(const QVector<ClipDuration> &ranges);
	void positionChanged(qint64 positionMs);
	void durationChanged(qint64 durationMs);
	void reviewRequested();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	static constexpr double DefaultClipDurationSec = 90.0;
	static constexpr qint64 FrameStepMs = 33;

	QString videoPath;
	qint64 durationMs = 0;
	bool updatingTimelineFromPlayer = false;
	bool pauseAfterSeekWarmup = false;
	int selectedRangeIndex = -1;

	QMediaPlayer *player = nullptr;
	QAudioOutput *audioOutput = nullptr;
	QWidget *editorSurface = nullptr;
	QWidget *videoContainer = nullptr;
	QWidget *fullscreenWindow = nullptr;
	QWidget *reviewWindowBeforeFullScreen = nullptr;
	bool reviewWindowWasVisibleBeforeFullScreen = false;
	QWidget *fullscreenHostWindow = nullptr;
	Qt::WindowStates windowStateBeforeFullScreen = Qt::WindowNoState;
	QVector<QWidget *> widgetsHiddenForFullScreen;
	QVBoxLayout *rootLayout = nullptr;
	QVideoWidget *videoWidget = nullptr;
	TimelineWidget *timeline = nullptr;
	PlaybackIconButton *playPauseControl = nullptr;
	QToolButton *fullscreenControl = nullptr;
	QPushButton *reviewActionButton = nullptr;
	QLabel *currentTimeLabel = nullptr;
	QLabel *durationTimeLabel = nullptr;
	QLabel *selectedClipLabel = nullptr;

	QVector<double> clipMarkersSec;

	void setupShortcuts();
	bool shouldHandleShortcut() const;
	void addClipDuration(double startSec, double endSec);
	void updateTimelinePosition(qint64 positionMs);
	void updateTimelineDuration(qint64 newDurationMs);
	void handleMediaStatusChanged(QMediaPlayer::MediaStatus status);
	void warmUpVideoFrameAfterSeek(bool wasPlaying);
	void updateSelectedClipPreview(double startSec);
	void refreshTimelineMarkers();
	void positionOverlayControls();
	void rebuildClipRanges();
	void removeClipRangeAtIndex(int index);
	void loadSavedMarkers();
	void saveMarkers() const;
	QString markerConfigKey() const;
};
