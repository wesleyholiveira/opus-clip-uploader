#pragma once

#include "models/curation-settings.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QTableWidget>
#include <QVideoWidget>
#include <QString>
#include <QVector>

class QAudioOutput;
class TimelineWidget;
class PlaybackIconButton;

class UploadReviewDialog : public QDialog {
	Q_OBJECT

public:
	explicit UploadReviewDialog(const QString &videoPath, QWidget *parent = nullptr);

	CurationSettings curationSettings() const;

private:
	static constexpr double DefaultClipDurationSec = 90.0;

	QString videoPath;
	qint64 durationMs = 0;
	bool updatingTimelineFromPlayer = false;

	QMediaPlayer *player = nullptr;
	QAudioOutput *audioOutput = nullptr;
	QVideoWidget *videoWidget = nullptr;

	TimelineWidget *timeline = nullptr;
	PlaybackIconButton *playPauseControl = nullptr;
	QLabel *currentTimeLabel = nullptr;
	QLabel *durationTimeLabel = nullptr;
	QLabel *selectedClipLabel = nullptr;
	QTableWidget *clipTable = nullptr;
	QLineEdit *topicKeywordsInput = nullptr;
	QComboBox *genreInput = nullptr;
	QComboBox *modelInput = nullptr;
	QCheckBox *skipCurateInput = nullptr;

	QVector<double> clipMarkersSec;

	void addClipDuration(double startSec, double endSec);
	void addMarkerAtCurrentPosition();
	void updateTimelinePosition(qint64 positionMs);
	void updateTimelineDuration(qint64 newDurationMs);
	void updateSelectedClipPreview(double startSec);
	void seekToSeconds(double seconds);
	void seekToMilliseconds(qint64 positionMs);
	void refreshTimelineMarkers();
	void rebuildClipRanges();
	QVector<ClipDuration> calculatedClipRanges() const;
	void removeClipRangeAtRow(int row);
	void loadSavedCurationOptions();
	void saveCurationOptions() const;
	QString formatTimecode(double seconds) const;
};
