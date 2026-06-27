#include "ui/video-marker-editor.hpp"

#include "ui/review/playback-icon-button.hpp"
#include "ui/review/timeline-widget.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QAudioOutput>
#include <QByteArray>
#include <QEvent>
#include <QCloseEvent>
#include <QFileInfo>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLayout>
#include <QScreen>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QApplication>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QSizePolicy>
#include <QUrl>
#include "utils/config.hpp"
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>
#include <cmath>
#include <limits>

static QString obsText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

VideoMarkerEditor::VideoMarkerEditor(const QString &videoPath, QWidget *parent) : QWidget(parent), videoPath(videoPath)
{
	rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(8);

	editorSurface = new QWidget(this);
	auto *surfaceLayout = new QVBoxLayout(editorSurface);
	surfaceLayout->setContentsMargins(0, 0, 0, 0);
	surfaceLayout->setSpacing(8);

	videoContainer = new QWidget(editorSurface);
	videoContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	videoContainer->setMinimumHeight(320);
	videoContainer->installEventFilter(this);
	videoContainer->setMouseTracking(true);
	editorSurface->installEventFilter(this);

	auto *videoOverlayLayout = new QGridLayout(videoContainer);
	videoOverlayLayout->setContentsMargins(0, 0, 0, 0);
	videoOverlayLayout->setSpacing(0);

	videoWidget = new QVideoWidget(videoContainer);
	videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
	videoWidget->setMouseTracking(true);
	videoWidget->installEventFilter(this);

	fullscreenControl = new QToolButton(videoContainer);
	fullscreenControl->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
	fullscreenControl->setToolTip(obsText("Tooltip.FullScreen"));
	fullscreenControl->setAutoRaise(true);
	fullscreenControl->setCursor(Qt::PointingHandCursor);
	fullscreenControl->setText(QString());
	fullscreenControl->setFocusPolicy(Qt::NoFocus);
	fullscreenControl->setFixedSize(38, 38);
	fullscreenControl->setIconSize(QSize(22, 22));
	fullscreenControl->setStyleSheet(
		"QToolButton { background: rgba(0, 0, 0, 170); border: 1px solid rgba(255, 255, 255, 90); border-radius: 19px; padding: 7px; }"
		"QToolButton:hover { background: rgba(0, 0, 0, 220); border: 1px solid rgba(255, 255, 255, 160); }");

	videoOverlayLayout->addWidget(videoWidget, 0, 0);
	videoOverlayLayout->addWidget(fullscreenControl, 0, 0, Qt::AlignRight | Qt::AlignBottom);
	fullscreenControl->hide();
	connect(fullscreenControl, &QToolButton::clicked, this, &VideoMarkerEditor::toggleFullScreen);

	player = new QMediaPlayer(this);
	audioOutput = new QAudioOutput(this);
	player->setAudioOutput(audioOutput);
	player->setVideoOutput(videoWidget);
	player->setSource(QUrl::fromLocalFile(videoPath));

	const QPointer<VideoMarkerEditor> self(this);
	playPauseControl = new PlaybackIconButton(editorSurface);
	playPauseControl->clicked = [self]() {
		if (self)
			self->togglePlayback();
	};

	timeline = new TimelineWidget(editorSurface);
	timeline->previewSeekRequested = [self](qint64 positionMs) {
		if (self)
			self->seekToMilliseconds(positionMs);
	};
	timeline->commitSeekRequested = [self](qint64 positionMs) {
		if (self)
			self->seekToMilliseconds(positionMs);
	};
	timeline->markerSelected = [self](int index) {
		if (self)
			self->selectMarker(index);
	};
	timeline->markerMovePreviewRequested = [self](int index, qint64 positionMs) {
		if (!self)
			return;
		self->updateMarkerAtIndex(index, positionMs / 1000.0, false);
		self->seekToMilliseconds(positionMs);
	};
	timeline->markerMoveCommitted = [self](int index, qint64 positionMs) {
		if (!self)
			return;
		self->updateMarkerAtIndex(index, positionMs / 1000.0, true);
		self->seekToMilliseconds(positionMs);
	};

	currentTimeLabel = new QLabel("00:00:00", editorSurface);
	durationTimeLabel = new QLabel("00:00:00", editorSurface);
	selectedClipLabel = new QLabel(obsText("Label.SelectedClipInitial"), editorSurface);
	seekStatusLabel = new QLabel(QStringLiteral("Seeking…"), editorSurface);
	seekStatusLabel->setVisible(false);
	seekStatusLabel->setStyleSheet(QStringLiteral("QLabel { color: palette(mid); font-size: 11px; }"));
	auto *shortcutHelpLabel = new QLabel(obsText("Shortcut.Help"), editorSurface);
	shortcutHelpLabel->setWordWrap(true);
	shortcutHelpLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	shortcutHelpLabel->setStyleSheet(
		"QLabel { background: rgba(127, 127, 127, 35); border: 1px solid rgba(127, 127, 127, 90); "
		"border-radius: 6px; padding: 6px 8px; color: palette(text); font-size: 11px; }");

	reviewActionButton = new QPushButton(obsText("Button.ReviewAndUpload"), editorSurface);
	reviewActionButton->setVisible(false);
	reviewActionButton->setToolTip(obsText("Tooltip.ReviewAndUpload"));

	connect(reviewActionButton, &QPushButton::clicked, this, [this]() {
		saveMarkers();
		emit reviewRequested();
	});

	connect(player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
		playPauseControl->setPlaying(state == QMediaPlayer::PlayingState);
	});
	connect(player, &QMediaPlayer::durationChanged, this, &VideoMarkerEditor::updateTimelineDuration);
	connect(player, &QMediaPlayer::positionChanged, this, &VideoMarkerEditor::updateTimelinePosition);
	connect(player, &QMediaPlayer::mediaStatusChanged, this, &VideoMarkerEditor::handleMediaStatusChanged);

	auto *timelineLayout = new QHBoxLayout();
	timelineLayout->setContentsMargins(0, 0, 0, 0);
	timelineLayout->setSpacing(8);
	timelineLayout->addWidget(playPauseControl, 0, Qt::AlignVCenter);
	timelineLayout->addWidget(currentTimeLabel, 0, Qt::AlignVCenter);
	timelineLayout->addWidget(timeline, 1, Qt::AlignVCenter);
	timelineLayout->addWidget(durationTimeLabel, 0, Qt::AlignVCenter);

	auto *controlsLayout = new QHBoxLayout();
	controlsLayout->setContentsMargins(0, 0, 0, 0);
	controlsLayout->setSpacing(8);
	controlsLayout->addWidget(reviewActionButton);
	controlsLayout->addStretch();

	surfaceLayout->addWidget(videoContainer, 1);
	surfaceLayout->addLayout(timelineLayout);
	surfaceLayout->addWidget(selectedClipLabel);
	surfaceLayout->addWidget(seekStatusLabel);
	surfaceLayout->addWidget(shortcutHelpLabel);
	surfaceLayout->addLayout(controlsLayout);

	rootLayout->addWidget(editorSurface, 1);

	loadSavedMarkers();
	setupShortcuts();

	QTimer::singleShot(0, this, [guard = QPointer<VideoMarkerEditor>(this)]() {
		if (guard)
			guard->positionOverlayControls();
	});
}

QVector<ClipDuration> VideoMarkerEditor::clipRanges() const
{
	QVector<double> markers = clipMarkersSec;
	std::sort(markers.begin(), markers.end());

	QVector<ClipDuration> ranges;
	const double durationSec = durationMs > 0 ? durationMs / 1000.0 : 0.0;

	if (markers.isEmpty() && durationSec > 0.0) {
		ranges.append({0.0, std::min(DefaultClipDurationSec, durationSec)});
		return ranges;
	}

	for (int i = 0; i < markers.size(); i += 2) {
		double startSec = markers[i];
		double endSec = i + 1 < markers.size() ? markers[i + 1] : startSec + DefaultClipDurationSec;

		if (durationSec > 0.0) {
			startSec = std::clamp(startSec, 0.0, durationSec);
			endSec = std::clamp(endSec, startSec, durationSec);
		}

		if (endSec > startSec)
			ranges.append({startSec, endSec});
	}

	return ranges;
}

bool VideoMarkerEditor::hasExplicitClipMarkers() const
{
	return !clipMarkersSec.isEmpty();
}

qint64 VideoMarkerEditor::durationMilliseconds() const
{
	return durationMs;
}

qint64 VideoMarkerEditor::positionMilliseconds() const
{
	return player ? player->position() : 0;
}

QVector<double> VideoMarkerEditor::markerPositions() const
{
	QVector<double> markers = clipMarkersSec;
	std::sort(markers.begin(), markers.end());
	return markers;
}

void VideoMarkerEditor::setMarkerPositions(const QVector<double> &markers)
{
	clipMarkersSec.clear();

	const double durationSec = durationMs > 0 ? durationMs / 1000.0 : 0.0;

	for (double marker : markers) {
		if (!std::isfinite(marker))
			continue;

		marker = std::max(0.0, marker);
		if (durationSec > 0.0)
			marker = std::min(marker, durationSec);

		clipMarkersSec.append(marker);
	}

	rebuildClipRanges();
}

void VideoMarkerEditor::setReviewActionVisible(bool visible)
{
	if (reviewActionButton)
		reviewActionButton->setVisible(visible);
}

void VideoMarkerEditor::setupShortcuts()
{
	auto addShortcut = [this](const QKeySequence &sequence, auto slot,
				  Qt::ShortcutContext context = Qt::WindowShortcut) {
		auto *shortcut = new QShortcut(sequence, this);
		shortcut->setContext(context);
		connect(shortcut, &QShortcut::activated, this, [this, sequence, slot]() {
			if (!shouldHandleShortcut(sequence))
				return;

			(this->*slot)();
		});
	};

	addShortcut(QKeySequence(Qt::Key_Space), &VideoMarkerEditor::togglePlayback);
	addShortcut(QKeySequence(Qt::Key_Right), &VideoMarkerEditor::seekForwardTenSeconds);
	addShortcut(QKeySequence(Qt::Key_Left), &VideoMarkerEditor::seekBackwardTenSeconds);
	addShortcut(QKeySequence(Qt::Key_Period), &VideoMarkerEditor::seekForwardFrame);
	addShortcut(QKeySequence(Qt::Key_Comma), &VideoMarkerEditor::seekBackwardFrame);
	addShortcut(QKeySequence(Qt::Key_M), &VideoMarkerEditor::addMarkerAtCurrentPosition);
	addShortcut(QKeySequence(Qt::Key_Delete), &VideoMarkerEditor::deleteSelectedMarkerOrRange,
		    Qt::WidgetWithChildrenShortcut);
	addShortcut(QKeySequence(Qt::Key_Backspace), &VideoMarkerEditor::deleteSelectedMarkerOrRange,
		    Qt::WidgetWithChildrenShortcut);
	addShortcut(QKeySequence(Qt::Key_F), &VideoMarkerEditor::toggleFullScreen);
	addShortcut(QKeySequence(Qt::Key_Escape), &VideoMarkerEditor::exitFullScreen);
}

bool VideoMarkerEditor::shouldHandleShortcut(const QKeySequence &sequence) const
{
	const QWidget *focus = QApplication::focusWidget();
	if (!focus)
		return isVisible();

	const QWidget *hostWindow = window();
	if (!hostWindow || focus->window() != hostWindow)
		return false;

	if (qobject_cast<const QLineEdit *>(focus) || qobject_cast<const QTextEdit *>(focus) ||
	    qobject_cast<const QPlainTextEdit *>(focus) || qobject_cast<const QComboBox *>(focus) ||
	    qobject_cast<const QAbstractSpinBox *>(focus)) {
		return false;
	}

	bool focusInsideItemView = false;
	for (const QWidget *widget = focus; widget; widget = widget->parentWidget()) {
		if (qobject_cast<const QAbstractItemView *>(widget)) {
			focusInsideItemView = true;
			break;
		}
	}

	if (focusInsideItemView) {
		/*
		 * The review marker table owns Delete/Backspace and row editing. Still allow
		 * fullscreen shortcuts from the table, because the user expects F/Escape to
		 * work regardless of which review widget currently has focus.
		 */
		return sequence == QKeySequence(Qt::Key_F) || sequence == QKeySequence(Qt::Key_Escape);
	}

	return isVisible();
}
bool VideoMarkerEditor::eventFilter(QObject *watched, QEvent *event)
{
	if ((watched == videoContainer || watched == videoWidget || watched == editorSurface) &&
	    (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
	     event->type() == QEvent::LayoutRequest || event->type() == QEvent::Move)) {
		QTimer::singleShot(0, this, [guard = QPointer<VideoMarkerEditor>(this)]() {
			if (guard)
				guard->positionOverlayControls();
		});
	}

	if ((watched == videoContainer || watched == videoWidget || watched == editorSurface) &&
	    event->type() == QEvent::Hide && fullscreenControl) {
		fullscreenControl->hide();
	}

	if (watched == fullscreenWindow && event->type() == QEvent::Close) {
		exitFullScreen();
		event->ignore();
		return true;
	}

	return QWidget::eventFilter(watched, event);
}

void VideoMarkerEditor::positionOverlayControls()
{
	if (!fullscreenControl || !videoContainer || !isVisible())
		return;

	/*
	 * Keep the fullscreen control as a child of the video container.  The previous
	 * implementation used a top-level Tool window with global coordinates, which
	 * made the icon stay fixed on the screen when the review/editor window moved.
	 */
	const int margin = 14;
	const int x = std::max(margin, videoContainer->width() - fullscreenControl->width() - margin);
	const int y = std::max(margin, videoContainer->height() - fullscreenControl->height() - margin);

	fullscreenControl->move(x, y);
	fullscreenControl->show();
	fullscreenControl->raise();
}

void VideoMarkerEditor::togglePlayback()
{
	if (!player)
		return;

	if (player->playbackState() == QMediaPlayer::PlayingState)
		player->pause();
	else
		player->play();
}

void VideoMarkerEditor::toggleFullScreen()
{
	if (fullscreenHostWindow) {
		exitFullScreen();
		return;
	}

	if (!editorSurface)
		return;

	fullscreenHostWindow = window();
	if (!fullscreenHostWindow)
		return;

	windowStateBeforeFullScreen = fullscreenHostWindow->windowState();
	widgetsHiddenForFullScreen.clear();

	/*
	 * Do NOT reparent editorSurface/videoWidget to another top-level window.
	 * QMediaPlayer/QVideoWidget on Windows/WMF can crash after the native video
	 * surface is moved between windows.  Fullscreen is implemented by making the
	 * current review window fullscreen and hiding every sibling around the editor.
	 * Visually this gives fullscreen video + timeline, while keeping the same
	 * QVideoWidget/native surface alive.
	 */
	QWidget *visibleBranch = this;
	QWidget *parentWidget = visibleBranch->parentWidget();

	while (parentWidget) {
		const auto children = parentWidget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (QWidget *child : children) {
			if (!child || child == visibleBranch || child == fullscreenControl)
				continue;

			if (child->isVisible()) {
				widgetsHiddenForFullScreen.append(child);
				child->hide();
			}
		}

		if (parentWidget == fullscreenHostWindow)
			break;

		visibleBranch = parentWidget;
		parentWidget = parentWidget->parentWidget();
	}

	fullscreenControl->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
	fullscreenControl->setToolTip(obsText("Tooltip.ExitFullScreen"));

	fullscreenHostWindow->showFullScreen();
	fullscreenHostWindow->raise();
	fullscreenHostWindow->activateWindow();
	setFocus(Qt::OtherFocusReason);
	editorSurface->setFocus(Qt::OtherFocusReason);

	QTimer::singleShot(0, this, [guard = QPointer<VideoMarkerEditor>(this)]() {
		if (!guard)
			return;
		guard->positionOverlayControls();
		if (guard->fullscreenControl)
			guard->fullscreenControl->raise();
	});
}

void VideoMarkerEditor::exitFullScreen()
{
	if (!fullscreenHostWindow)
		return;

	for (QWidget *widget : widgetsHiddenForFullScreen) {
		if (widget)
			widget->show();
	}
	widgetsHiddenForFullScreen.clear();

	QWidget *windowToRestore = fullscreenHostWindow;
	fullscreenHostWindow = nullptr;

	if (fullscreenControl) {
		fullscreenControl->setIcon(style()->standardIcon(QStyle::SP_TitleBarMaxButton));
		fullscreenControl->setToolTip(obsText("Tooltip.FullScreen"));
	}

	if (windowToRestore) {
		windowToRestore->setWindowState(windowStateBeforeFullScreen);
		if (windowStateBeforeFullScreen.testFlag(Qt::WindowFullScreen) == false &&
		    windowStateBeforeFullScreen == Qt::WindowNoState)
			windowToRestore->showNormal();
		windowToRestore->raise();
		windowToRestore->activateWindow();
	}

	QTimer::singleShot(0, this, [guard = QPointer<VideoMarkerEditor>(this)]() {
		if (!guard)
			return;
		guard->positionOverlayControls();
		if (guard->fullscreenControl)
			guard->fullscreenControl->raise();
	});
}

void VideoMarkerEditor::seekForwardTenSeconds()
{
	seekToMilliseconds(positionMilliseconds() + 10000);
}

void VideoMarkerEditor::seekBackwardTenSeconds()
{
	seekToMilliseconds(positionMilliseconds() - 10000);
}

void VideoMarkerEditor::seekForwardFrame()
{
	seekToMilliseconds(positionMilliseconds() + FrameStepMs);
}

void VideoMarkerEditor::seekBackwardFrame()
{
	seekToMilliseconds(positionMilliseconds() - FrameStepMs);
}

void VideoMarkerEditor::addClipDuration(double startSec, double endSec)
{
	if (durationMs > 0) {
		const double durationSec = durationMs / 1000.0;
		startSec = std::clamp(startSec, 0.0, durationSec);
		endSec = std::clamp(endSec, startSec, durationSec);
	}

	clipMarkersSec.append(startSec);
	if (endSec > startSec && endSec < startSec + DefaultClipDurationSec)
		clipMarkersSec.append(endSec);

	rebuildClipRanges();
}

void VideoMarkerEditor::addMarkerAtCurrentPosition()
{
	const double startSec = positionMilliseconds() / 1000.0;
	clipMarkersSec.append(startSec);
	rebuildClipRanges();

	QVector<double> markers = markerPositions();
	for (int i = 0; i < markers.size(); ++i) {
		if (std::abs(markers[i] - startSec) < 0.01) {
			selectMarker(i);
			break;
		}
	}
}

void VideoMarkerEditor::selectRange(int index)
{
	clearSelectedMarker();
	selectedRangeIndex = index;
	updateSelectedClipPreview(positionMilliseconds() / 1000.0);
}

void VideoMarkerEditor::goToSelectedRange()
{
	const QVector<ClipDuration> ranges = clipRanges();
	if (selectedRangeIndex < 0 || selectedRangeIndex >= ranges.size())
		return;

	seekToSeconds(ranges[selectedRangeIndex].startSec);
}

void VideoMarkerEditor::removeSelectedRange()
{
	clearSelectedMarker();
	removeClipRangeAtIndex(selectedRangeIndex);
}

void VideoMarkerEditor::removeSelectedMarker()
{
	if (selectedMarkerIndex < 0)
		return;

	QVector<double> markers = clipMarkersSec;
	std::sort(markers.begin(), markers.end());
	if (selectedMarkerIndex >= markers.size()) {
		clearSelectedMarker();
		return;
	}

	markers.removeAt(selectedMarkerIndex);
	clipMarkersSec = markers;
	selectedMarkerIndex = -1;
	if (timeline)
		timeline->setSelectedMarkerIndex(-1);
	rebuildClipRanges();
}

void VideoMarkerEditor::deleteSelectedMarkerOrRange()
{
	if (selectedMarkerIndex >= 0) {
		removeSelectedMarker();
		return;
	}

	removeSelectedRange();
}

void VideoMarkerEditor::updateTimelinePosition(qint64 positionMs)
{
	if (!updatingTimelineFromPlayer)
		timeline->setPosition(positionMs);

	const double startSec = positionMs / 1000.0;
	currentTimeLabel->setText(formatTimecode(startSec));
	if (seekStatusLabel && seekStatusLabel->isVisible())
		seekStatusLabel->hide();
	updateSelectedClipPreview(startSec);
	emit positionChanged(positionMs);
}

void VideoMarkerEditor::updateTimelineDuration(qint64 newDurationMs)
{
	durationMs = newDurationMs;
	timeline->setDuration(durationMs);
	durationTimeLabel->setText(formatTimecode(durationMs / 1000.0));
	rebuildClipRanges();
	emit durationChanged(durationMs);
}

void VideoMarkerEditor::updateSelectedClipPreview(double startSec)
{
	if (!selectedClipLabel)
		return;

	const QVector<ClipDuration> ranges = clipRanges();

	if (selectedRangeIndex >= 0 && selectedRangeIndex < ranges.size()) {
		const auto &range = ranges[selectedRangeIndex];
		const double selectedSec = std::max(0.0, range.endSec - range.startSec);
		selectedClipLabel->setText(obsText("Label.SelectedClipRange")
						   .arg(selectedRangeIndex + 1)
						   .arg(formatTimecode(range.startSec), formatTimecode(range.endSec))
						   .arg(selectedSec, 0, 'f', 0));
		return;
	}

	for (int i = 0; i < ranges.size(); ++i) {
		const auto &range = ranges[i];
		if (startSec >= range.startSec && startSec <= range.endSec) {
			const double selectedSec = std::max(0.0, range.endSec - range.startSec);
			selectedClipLabel->setText(
				obsText("Label.SelectedClipRange")
					.arg(i + 1)
					.arg(formatTimecode(range.startSec), formatTimecode(range.endSec))
					.arg(selectedSec, 0, 'f', 0));
			return;
		}
	}

	double endSec = startSec + DefaultClipDurationSec;
	if (durationMs > 0)
		endSec = std::min(endSec, durationMs / 1000.0);

	selectedClipLabel->setText(obsText("Label.NextMarkerRange")
					   .arg(formatTimecode(startSec), formatTimecode(endSec))
					   .arg(std::max(0.0, endSec - startSec), 0, 'f', 0));
}

void VideoMarkerEditor::seekToSeconds(double seconds)
{
	if (durationMs > 0)
		seconds = std::clamp(seconds, 0.0, durationMs / 1000.0);
	else
		seconds = std::max(0.0, seconds);

	seekToMilliseconds(static_cast<qint64>(seconds * 1000.0));
}

void VideoMarkerEditor::seekToMilliseconds(qint64 positionMs)
{
	if (!player || !timeline)
		return;

	if (durationMs > 0)
		positionMs = std::clamp<qint64>(positionMs, 0, durationMs);
	else
		positionMs = std::max<qint64>(0, positionMs);

	const bool wasPlaying = player->playbackState() == QMediaPlayer::PlayingState;
	if (seekStatusLabel) {
		seekStatusLabel->setText(QStringLiteral("Seeking…"));
		seekStatusLabel->show();
	}

	updatingTimelineFromPlayer = true;
	player->setPosition(positionMs);
	timeline->setPosition(positionMs);
	updatingTimelineFromPlayer = false;

	warmUpVideoFrameAfterSeek(wasPlaying);

	const double seconds = positionMs / 1000.0;
	currentTimeLabel->setText(formatTimecode(seconds));
	updateSelectedClipPreview(seconds);
	QPointer<QLabel> seekLabel(seekStatusLabel);
	QTimer::singleShot(450, this, [seekLabel]() {
		if (seekLabel)
			seekLabel->hide();
	});
}

void VideoMarkerEditor::handleMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
	if (!player || !timeline)
		return;

	if (status != QMediaPlayer::EndOfMedia)
		return;

	pauseAfterSeekWarmup = false;
	updatingTimelineFromPlayer = true;
	player->setPosition(0);
	timeline->setPosition(0);
	updatingTimelineFromPlayer = false;
	currentTimeLabel->setText(formatTimecode(0.0));
	updateSelectedClipPreview(0.0);
	player->play();
}

void VideoMarkerEditor::warmUpVideoFrameAfterSeek(bool wasPlaying)
{
	if (!player || wasPlaying)
		return;

	const auto state = player->playbackState();
	const auto status = player->mediaStatus();
	const bool needsWarmup = state == QMediaPlayer::StoppedState || status == QMediaPlayer::NoMedia ||
				 status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia ||
				 status == QMediaPlayer::EndOfMedia;

	if (!needsWarmup)
		return;

	pauseAfterSeekWarmup = true;
	player->play();

	QPointer<VideoMarkerEditor> guard(this);
	QTimer::singleShot(90, this, [guard]() {
		if (!guard || !guard->player || !guard->pauseAfterSeekWarmup)
			return;

		guard->pauseAfterSeekWarmup = false;
		if (guard->player->playbackState() == QMediaPlayer::PlayingState)
			guard->player->pause();
	});
}

void VideoMarkerEditor::refreshTimelineMarkers()
{
	QList<qint64> markers;
	markers.reserve(clipMarkersSec.size());

	for (double markerSec : clipMarkersSec)
		markers.append(static_cast<qint64>(markerSec * 1000.0));

	timeline->setMarkers(markers);
	timeline->setSelectedMarkerIndex(selectedMarkerIndex);
	timeline->setClipRanges(clipRanges());
}

void VideoMarkerEditor::rebuildClipRanges()
{
	std::sort(clipMarkersSec.begin(), clipMarkersSec.end());
	const QVector<ClipDuration> ranges = clipRanges();
	if (selectedRangeIndex >= ranges.size())
		selectedRangeIndex = ranges.isEmpty() ? -1 : ranges.size() - 1;
	if (selectedMarkerIndex >= clipMarkersSec.size())
		selectedMarkerIndex = -1;

	refreshTimelineMarkers();
	updateSelectedClipPreview(positionMilliseconds() / 1000.0);
	saveMarkers();
	emit rangesChanged(ranges);
}

void VideoMarkerEditor::selectMarker(int index)
{
	QVector<double> markers = markerPositions();
	if (index < 0 || index >= markers.size()) {
		clearSelectedMarker();
		return;
	}

	selectedMarkerIndex = index;
	selectedRangeIndex = index / 2;
	if (timeline)
		timeline->setSelectedMarkerIndex(selectedMarkerIndex);
	updateSelectedClipPreview(markers[index]);
}

void VideoMarkerEditor::clearSelectedMarker()
{
	if (selectedMarkerIndex < 0)
		return;

	selectedMarkerIndex = -1;
	if (timeline)
		timeline->setSelectedMarkerIndex(-1);
}

void VideoMarkerEditor::updateMarkerAtIndex(int index, double markerSec, bool commit)
{
	QVector<double> markers = clipMarkersSec;
	std::sort(markers.begin(), markers.end());
	if (index < 0 || index >= markers.size())
		return;

	const double durationSec = durationMs > 0 ? durationMs / 1000.0 : 0.0;
	markerSec = std::max(0.0, markerSec);
	if (durationSec > 0.0)
		markerSec = std::min(markerSec, durationSec);

	markers[index] = markerSec;
	std::sort(markers.begin(), markers.end());
	clipMarkersSec = markers;

	int selectedIndex = 0;
	double bestDistance = std::numeric_limits<double>::max();
	for (int i = 0; i < clipMarkersSec.size(); ++i) {
		const double distance = std::abs(clipMarkersSec[i] - markerSec);
		if (distance < bestDistance) {
			bestDistance = distance;
			selectedIndex = i;
		}
	}
	selectedMarkerIndex = clipMarkersSec.isEmpty() ? -1 : selectedIndex;
	selectedRangeIndex = selectedMarkerIndex >= 0 ? selectedMarkerIndex / 2 : -1;

	if (commit) {
		rebuildClipRanges();
		return;
	}

	refreshTimelineMarkers();
	updateSelectedClipPreview(markerSec);
}

void VideoMarkerEditor::removeClipRangeAtIndex(int index)
{
	if (index < 0)
		return;

	QVector<double> markers = clipMarkersSec;
	std::sort(markers.begin(), markers.end());

	const int markerIndex = index * 2;
	if (markerIndex >= markers.size())
		return;

	const double startMarker = markers[markerIndex];
	const bool hasExplicitEnd = markerIndex + 1 < markers.size();
	const double endMarker = hasExplicitEnd ? markers[markerIndex + 1] : -1.0;

	auto removeFirstNear = [this](double target) {
		for (int i = 0; i < clipMarkersSec.size(); ++i) {
			if (std::abs(clipMarkersSec[i] - target) < 0.01) {
				clipMarkersSec.removeAt(i);
				return;
			}
		}
	};

	removeFirstNear(startMarker);
	if (hasExplicitEnd)
		removeFirstNear(endMarker);

	rebuildClipRanges();

	const int rangeCount = static_cast<int>(clipRanges().size());
	selectedRangeIndex = rangeCount > 0 ? std::min(index, rangeCount - 1) : -1;
	updateSelectedClipPreview(positionMilliseconds() / 1000.0);
}

QString VideoMarkerEditor::markerConfigKey() const
{
	const QString fileName = QFileInfo(videoPath).fileName().trimmed();
	if (fileName.isEmpty())
		return {};

	const QString safeFileName = QString::fromLatin1(
		fileName.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
	return QStringLiteral("video_markers.%1").arg(safeFileName);
}

void VideoMarkerEditor::loadSavedMarkers()
{
	const QString key = markerConfigKey();
	if (key.isEmpty())
		return;

	const QString rawMarkers = PluginConfig::getValue(key).trimmed();
	if (rawMarkers.isEmpty())
		return;

	QVector<double> loadedMarkers;
	const QStringList values = rawMarkers.split(';', Qt::SkipEmptyParts);
	loadedMarkers.reserve(values.size());

	for (const QString &value : values) {
		bool ok = false;
		const double marker = value.trimmed().toDouble(&ok);
		if (ok && std::isfinite(marker) && marker >= 0.0)
			loadedMarkers.append(marker);
	}

	if (!loadedMarkers.isEmpty())
		setMarkerPositions(loadedMarkers);
}

void VideoMarkerEditor::saveMarkers() const
{
	const QString key = markerConfigKey();
	if (key.isEmpty())
		return;

	QVector<double> markers = clipMarkersSec;
	std::sort(markers.begin(), markers.end());

	if (markers.isEmpty()) {
		PluginConfig::removeValue(key);
		return;
	}

	QStringList values;
	values.reserve(markers.size());

	for (double marker : markers)
		values.append(QString::number(marker, 'f', 3));

	PluginConfig::setValue(key, values.join(';'));
}

QString VideoMarkerEditor::formatTimecode(double seconds) const
{
	const auto totalMs = static_cast<qint64>(seconds * 1000.0);
	const qint64 totalSeconds = totalMs / 1000;
	const qint64 hours = totalSeconds / 3600;
	const qint64 minutes = (totalSeconds % 3600) / 60;
	const qint64 secs = totalSeconds % 60;

	return QString("%1:%2:%3")
		.arg(hours, 2, 10, QChar('0'))
		.arg(minutes, 2, 10, QChar('0'))
		.arg(secs, 2, 10, QChar('0'));
}
