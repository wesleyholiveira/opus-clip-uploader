#include "ui/upload-review-dialog.hpp"
#include "ui/advanced-settings-tree.hpp"
#include "utils/config.hpp"

#include <QAbstractItemView>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>
#include <functional>

static constexpr const char *CONFIG_REVIEW_TOPIC_KEYWORDS = "review.topic_keywords";
static constexpr const char *CONFIG_REVIEW_GENRE = "review.genre";
static constexpr const char *CONFIG_REVIEW_MODEL = "review.model";
static constexpr const char *CONFIG_REVIEW_SKIP_CURATE = "review.skip_curate";

static void setComboCurrentTextIfExists(QComboBox *combo, const QString &value)
{
	if (!combo || value.trimmed().isEmpty())
		return;

	const int index = combo->findText(value, Qt::MatchFixedString);
	if (index >= 0)
		combo->setCurrentIndex(index);
}

class TimelineWidget final : public QWidget {
public:
	explicit TimelineWidget(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(34);
		setMouseTracking(true);
		setCursor(Qt::PointingHandCursor);
		seekThrottle.invalidate();
	}

	void setDuration(qint64 value)
	{
		durationMs = std::max<qint64>(0, value);
		positionMs = std::clamp(positionMs, qint64(0), durationMs);
		update();
	}

	void setPosition(qint64 value)
	{
		if (dragging)
			return;

		positionMs = clampPosition(value);
		update();
	}

	qint64 position() const { return positionMs; }

	void setMarkers(const QList<qint64> &newMarkers)
	{
		markers = newMarkers;
		update();
	}

	std::function<void(qint64)> previewSeekRequested;
	std::function<void(qint64)> commitSeekRequested;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);

		const QRectF track = trackRect();
		const qreal radius = track.height() / 2.0;

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(70, 70, 70));
		painter.drawRoundedRect(track, radius, radius);

		const qreal progressX = xForPosition(positionMs);
		QRectF progress = track;
		progress.setRight(progressX);
		painter.setBrush(QColor(220, 53, 69));
		painter.drawRoundedRect(progress, radius, radius);

		painter.setPen(QPen(QColor(20, 20, 20), 1));
		painter.setBrush(QColor(255, 193, 7));
		for (qint64 marker : markers) {
			if (durationMs <= 0 || marker < 0 || marker > durationMs)
				continue;

			const qreal x = xForPosition(marker);
			const qreal top = track.top() - 15.0;

			QPolygonF icon;
			icon << QPointF(x - 6.0, top) << QPointF(x + 6.0, top) << QPointF(x + 6.0, top + 9.0)
			     << QPointF(x + 2.0, top + 9.0) << QPointF(x, top + 14.0) << QPointF(x - 2.0, top + 9.0)
			     << QPointF(x - 6.0, top + 9.0);
			painter.drawPolygon(icon);
		}

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(245, 245, 245));
		painter.drawEllipse(QPointF(progressX, track.center().y()), dragging ? 8.0 : 6.0, dragging ? 8.0 : 6.0);
		painter.setPen(QPen(QColor(35, 35, 35), 1));
		painter.setBrush(Qt::NoBrush);
		painter.drawEllipse(QPointF(progressX, track.center().y()), dragging ? 8.0 : 6.0, dragging ? 8.0 : 6.0);
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		dragging = true;
		setPositionFromMouse(event->position().x(), true);
		event->accept();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!dragging)
			return;

		setPositionFromMouse(event->position().x(), false);
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (!dragging) {
			QWidget::mouseReleaseEvent(event);
			return;
		}

		setPositionFromMouse(event->position().x(), true);
		dragging = false;
		if (commitSeekRequested)
			commitSeekRequested(positionMs);
		update();
		event->accept();
	}

	void leaveEvent(QEvent *event) override { QWidget::leaveEvent(event); }

private:
	qint64 durationMs = 0;
	qint64 positionMs = 0;
	QList<qint64> markers;
	bool dragging = false;
	QElapsedTimer seekThrottle;

	QRectF trackRect() const { return QRectF(10.0, height() / 2.0 - 3.0, std::max(1, width() - 20), 6.0); }

	qint64 clampPosition(qint64 value) const
	{
		return std::clamp(value, qint64(0), std::max<qint64>(0, durationMs));
	}

	qreal xForPosition(qint64 value) const
	{
		const QRectF track = trackRect();
		if (durationMs <= 0)
			return track.left();

		const qreal ratio = qreal(clampPosition(value)) / qreal(durationMs);
		return track.left() + ratio * track.width();
	}

	qint64 positionForX(qreal x) const
	{
		const QRectF track = trackRect();
		const qreal clampedX = std::clamp(x, track.left(), track.right());
		const qreal ratio = (clampedX - track.left()) / std::max<qreal>(1.0, track.width());
		return clampPosition(static_cast<qint64>(ratio * qreal(durationMs)));
	}

	void setPositionFromMouse(qreal x, bool forceSeek)
	{
		positionMs = positionForX(x);
		update();

		if (!previewSeekRequested)
			return;

		const bool shouldSeek = forceSeek || !seekThrottle.isValid() || seekThrottle.elapsed() >= 70;
		if (!shouldSeek)
			return;

		seekThrottle.restart();
		previewSeekRequested(positionMs);
	}
};

class PlaybackIconButton final : public QWidget {
public:
	explicit PlaybackIconButton(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFixedSize(30, 30);
		setCursor(Qt::PointingHandCursor);
		setToolTip("Play");
	}

	void setPlaying(bool value)
	{
		playing = value;
		setToolTip(playing ? "Pause" : "Play");
		update();
	}

	std::function<void()> clicked;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(235, 235, 235));

		if (playing) {
			painter.drawRoundedRect(QRectF(9, 7, 4, 16), 1.5, 1.5);
			painter.drawRoundedRect(QRectF(17, 7, 4, 16), 1.5, 1.5);
		} else {
			QPolygonF triangle;
			triangle << QPointF(11, 7) << QPointF(23, 15) << QPointF(11, 23);
			painter.drawPolygon(triangle);
		}
	}

	void mousePressEvent(QMouseEvent *event) override { event->accept(); }

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (rect().contains(event->position().toPoint()) && clicked)
			clicked();
		event->accept();
	}

private:
	bool playing = false;
};

UploadReviewDialog::UploadReviewDialog(const QString &videoPath, QWidget *parent)
	: QDialog(parent),
	  videoPath(videoPath)
{
	setWindowTitle("Clip Cropper - Revisar vídeo");
	resize(920, 720);

	auto *mainLayout = new QVBoxLayout(this);

	videoWidget = new QVideoWidget(this);
	videoWidget->setMinimumHeight(320);

	player = new QMediaPlayer(this);
	audioOutput = new QAudioOutput(this);
	player->setAudioOutput(audioOutput);
	player->setVideoOutput(videoWidget);
	player->setSource(QUrl::fromLocalFile(videoPath));

	playPauseControl = new PlaybackIconButton(this);
	playPauseControl->clicked = [this]() {
		if (player->playbackState() == QMediaPlayer::PlayingState)
			player->pause();
		else
			player->play();
	};

	auto *btnAddMarker = new QPushButton("Adicionar marcação", this);
	auto *btnJumpToMarker = new QPushButton("Ir para marcação", this);
	auto *btnRemoveMarker = new QPushButton("Remover marcação", this);

	timeline = new TimelineWidget(this);
	timeline->previewSeekRequested = [this](qint64 positionMs) {
		seekToMilliseconds(positionMs);
	};
	timeline->commitSeekRequested = [this](qint64 positionMs) {
		seekToMilliseconds(positionMs);
	};

	currentTimeLabel = new QLabel("00:00:00", this);
	durationTimeLabel = new QLabel("00:00:00", this);
	selectedClipLabel = new QLabel("Clip: 00:00:00 → 00:01:30 (90s)", this);

	connect(player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
		playPauseControl->setPlaying(state == QMediaPlayer::PlayingState);
	});

	connect(btnAddMarker, &QPushButton::clicked, this, &UploadReviewDialog::addMarkerAtCurrentPosition);

	connect(btnJumpToMarker, &QPushButton::clicked, this, [this]() {
		const int row = clipTable->currentRow();
		if (row < 0 || !clipTable->item(row, 1))
			return;

		seekToSeconds(clipTable->item(row, 1)->text().toDouble());
	});

	connect(btnRemoveMarker, &QPushButton::clicked, this, [this]() {
		const int row = clipTable->currentRow();
		if (row >= 0) {
			clipTable->removeRow(row);
			refreshTimelineMarkers();
		}
	});

	connect(player, &QMediaPlayer::durationChanged, this, &UploadReviewDialog::updateTimelineDuration);
	connect(player, &QMediaPlayer::positionChanged, this, &UploadReviewDialog::updateTimelinePosition);

	auto *timelineLayout = new QHBoxLayout();
	timelineLayout->setContentsMargins(0, 0, 0, 0);
	timelineLayout->setSpacing(8);
	timelineLayout->addWidget(playPauseControl, 0, Qt::AlignVCenter);
	timelineLayout->addWidget(currentTimeLabel);
	timelineLayout->addWidget(timeline, 1);
	timelineLayout->addWidget(durationTimeLabel);

	auto *controlsLayout = new QHBoxLayout();
	controlsLayout->addWidget(btnAddMarker);
	controlsLayout->addWidget(btnJumpToMarker);
	controlsLayout->addWidget(btnRemoveMarker);
	controlsLayout->addStretch();

	clipTable = new QTableWidget(this);
	clipTable->setColumnCount(3);
	clipTable->setHorizontalHeaderLabels({"Marcação", "Start Sec", "End Sec"});
	clipTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	clipTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	clipTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	clipTable->setMinimumHeight(150);
	clipTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	clipTable->setSelectionMode(QAbstractItemView::SingleSelection);
	clipTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	clipTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	clipTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

	connect(clipTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
		if (row < 0 || !clipTable->item(row, 1))
			return;

		seekToSeconds(clipTable->item(row, 1)->text().toDouble());
	});

	topicKeywordsInput = new QLineEdit(this);
	topicKeywordsInput->setPlaceholderText("Ex: OpusClip, gameplay, tutorial");

	modelInput = new QComboBox(this);
	modelInput->addItems(QStringList{"ClipBasic", "ClipAnything"});

	genreInput = new QComboBox(this);
	genreInput->addItems(QStringList{"Auto", "Q&A", "Commentary", "Marketing", "Webinar", "Motivational speech",
					 "Podcast", "Academic", "Listicle", "Product reviews", "How-to", "Comedy",
					 "Sports commentary", "Church", "News", "Vlog", "Gaming", "Others"});

	skipCurateInput = new QCheckBox("Skip curate", this);

	auto *advancedTree = new AdvancedSettingsTree(this);
	advancedTree->addField("Model:", modelInput);
	advancedTree->addField("Topic keywords:", topicKeywordsInput);
	advancedTree->addField("Genre:", genreInput);
	advancedTree->addField("Skip curate:", skipCurateInput);

	loadSavedCurationOptions();

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
		saveCurationOptions();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	mainLayout->addWidget(new QLabel(QFileInfo(videoPath).fileName(), this));
	mainLayout->addWidget(videoWidget);
	mainLayout->addLayout(timelineLayout);
	mainLayout->addWidget(selectedClipLabel);
	mainLayout->addLayout(controlsLayout);
	mainLayout->addWidget(new QLabel("Marcações", this));
	mainLayout->addWidget(clipTable);
	mainLayout->addWidget(advancedTree);
	mainLayout->addWidget(buttons);
}

void UploadReviewDialog::addClipDuration(double startSec, double endSec)
{
	if (durationMs > 0) {
		const double durationSec = durationMs / 1000.0;
		startSec = std::clamp(startSec, 0.0, durationSec);
		endSec = std::clamp(endSec, startSec, durationSec);
	}

	if (endSec <= startSec)
		endSec = startSec + DefaultClipDurationSec;

	const int row = clipTable->rowCount();
	clipTable->insertRow(row);
	clipTable->setItem(
		row, 0, new QTableWidgetItem(QString("%1 → %2").arg(formatTimecode(startSec), formatTimecode(endSec))));
	clipTable->setItem(row, 1, new QTableWidgetItem(QString::number(startSec, 'f', 2)));
	clipTable->setItem(row, 2, new QTableWidgetItem(QString::number(endSec, 'f', 2)));
	clipTable->selectRow(row);
	refreshTimelineMarkers();
}

void UploadReviewDialog::addMarkerAtCurrentPosition()
{
	const double startSec = player->position() / 1000.0;
	addClipDuration(startSec, startSec + DefaultClipDurationSec);
}

void UploadReviewDialog::updateTimelinePosition(qint64 positionMs)
{
	if (!updatingTimelineFromPlayer)
		timeline->setPosition(positionMs);

	const double startSec = positionMs / 1000.0;
	currentTimeLabel->setText(formatTimecode(startSec));
	updateSelectedClipPreview(startSec);
}

void UploadReviewDialog::updateTimelineDuration(qint64 newDurationMs)
{
	durationMs = newDurationMs;
	timeline->setDuration(durationMs);
	durationTimeLabel->setText(formatTimecode(durationMs / 1000.0));
	refreshTimelineMarkers();
}

void UploadReviewDialog::updateSelectedClipPreview(double startSec)
{
	double endSec = startSec + DefaultClipDurationSec;
	if (durationMs > 0)
		endSec = std::min(endSec, durationMs / 1000.0);

	selectedClipLabel->setText(
		QString("Clip: %1 → %2 (90s)").arg(formatTimecode(startSec), formatTimecode(endSec)));
}

void UploadReviewDialog::seekToSeconds(double seconds)
{
	if (durationMs > 0)
		seconds = std::clamp(seconds, 0.0, durationMs / 1000.0);
	else
		seconds = std::max(0.0, seconds);

	seekToMilliseconds(static_cast<qint64>(seconds * 1000.0));
}

void UploadReviewDialog::seekToMilliseconds(qint64 positionMs)
{
	if (durationMs > 0)
		positionMs = std::clamp<qint64>(positionMs, 0, durationMs);
	else
		positionMs = std::max<qint64>(0, positionMs);

	updatingTimelineFromPlayer = true;
	player->setPosition(positionMs);
	timeline->setPosition(positionMs);
	updatingTimelineFromPlayer = false;

	const double seconds = positionMs / 1000.0;
	currentTimeLabel->setText(formatTimecode(seconds));
	updateSelectedClipPreview(seconds);
}

void UploadReviewDialog::refreshTimelineMarkers()
{
	QList<qint64> markers;
	markers.reserve(clipTable->rowCount());

	for (int row = 0; row < clipTable->rowCount(); ++row) {
		const auto *item = clipTable->item(row, 1);
		if (!item)
			continue;

		markers.append(static_cast<qint64>(item->text().toDouble() * 1000.0));
	}

	timeline->setMarkers(markers);
}

void UploadReviewDialog::loadSavedCurationOptions()
{
	const QString savedTopicKeywords = PluginConfig::getValue(CONFIG_REVIEW_TOPIC_KEYWORDS).trimmed();
	if (!savedTopicKeywords.isEmpty())
		topicKeywordsInput->setText(savedTopicKeywords);

	setComboCurrentTextIfExists(genreInput, PluginConfig::getValue(CONFIG_REVIEW_GENRE));
	setComboCurrentTextIfExists(modelInput, PluginConfig::getValue(CONFIG_REVIEW_MODEL));

	const QString savedSkipCurate = PluginConfig::getValue(CONFIG_REVIEW_SKIP_CURATE).trimmed().toLower();
	if (!savedSkipCurate.isEmpty())
		skipCurateInput->setChecked(savedSkipCurate == "true" || savedSkipCurate == "1" ||
					    savedSkipCurate == "yes");
}

void UploadReviewDialog::saveCurationOptions() const
{
	PluginConfig::setValue(CONFIG_REVIEW_TOPIC_KEYWORDS, topicKeywordsInput->text().trimmed());
	PluginConfig::setValue(CONFIG_REVIEW_GENRE, genreInput->currentText());
	PluginConfig::setValue(CONFIG_REVIEW_MODEL, modelInput->currentText());
	PluginConfig::setValue(CONFIG_REVIEW_SKIP_CURATE, skipCurateInput->isChecked() ? "true" : "false");
}

QString UploadReviewDialog::formatTimecode(double seconds) const
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

CurationSettings UploadReviewDialog::curationSettings() const
{
	CurationSettings settings;

	settings.rangeStartSec = 0;
	settings.rangeEndSec = durationMs > 0 ? durationMs / 1000.0 : 0;
	settings.genre = genreInput->currentText();
	settings.skipCurate = skipCurateInput->isChecked();
	settings.model = modelInput->currentText();

	const QStringList keywords = topicKeywordsInput->text().split(",", Qt::SkipEmptyParts);
	for (QString keyword : keywords)
		settings.topicKeywords.append(keyword.trimmed());

	for (int row = 0; row < clipTable->rowCount(); ++row) {
		const double start = clipTable->item(row, 1)->text().toDouble();
		const double end = clipTable->item(row, 2)->text().toDouble();

		if (end > start)
			settings.clipDurations.append({start, end});
	}

	return settings;
}
