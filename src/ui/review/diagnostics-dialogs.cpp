#include "ui/review/diagnostics-dialogs.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ClipCropper::Review {
namespace {

static double diagnosticScore(const QJsonObject &diagnostic, const QString &name, double fallback = 0.0)
{
	const QJsonObject scores = diagnostic.value(QStringLiteral("scores")).toObject();
	if (scores.contains(name))
		return scores.value(name).toDouble(fallback);
	return fallback;
}

static QString diagnosticScoreLabel(double value)
{
	if (value >= 0.68)
		return QStringLiteral("bom");
	if (value >= 0.42)
		return QStringLiteral("ok / incerto");
	return QStringLiteral("fraco");
}

static bool diagnosticHasEvidence(const QJsonObject &diagnostic, const QString &needle)
{
	const QJsonArray evidence = diagnostic.value(QStringLiteral("evidence")).toArray();
	for (const QJsonValue &value : evidence) {
		if (value.toString().contains(needle, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

static QString diagnosticEvidenceText(const QJsonObject &diagnostic, int maxItems = 18)
{
	const QJsonArray evidence = diagnostic.value(QStringLiteral("evidence")).toArray();
	QStringList lines;
	for (int i = 0; i < evidence.size() && lines.size() < maxItems; ++i) {
		const QString item = evidence.at(i).toString().trimmed();
		if (!item.isEmpty())
			lines.append(QStringLiteral("- %1").arg(item));
	}
	return lines.join(QChar('\n'));
}

static QString diagnosticSummaryText(const ClipDuration &range, const QJsonObject &diagnostic,
				     const QString &startLabel, const QString &endLabel)
{
	const double durationSec = std::max(0.0, range.endSec - range.startSec);
	const bool rejected = diagnostic.value(QStringLiteral("rejected")).toBool(false);
	const QString rejectionReason = diagnostic.value(QStringLiteral("rejection_reason")).toString().trimmed();
	const QString source = diagnostic.value(QStringLiteral("source")).toString().trimmed();
	const QString kind = diagnostic.value(QStringLiteral("diagnostic_kind")).toString().trimmed();
	const double finalScore = diagnostic.value(QStringLiteral("final_score")).toDouble(0.0);
	const double opening = std::max(diagnosticScore(diagnostic, QStringLiteral("arcOpening")),
					diagnosticScore(diagnostic, QStringLiteral("semanticOpeningHook")));
	const double development = std::max(diagnosticScore(diagnostic, QStringLiteral("arcDevelopment")),
					    diagnosticScore(diagnostic, QStringLiteral("semanticDirectAnswer")));
	const double conclusion = std::max(diagnosticScore(diagnostic, QStringLiteral("arcConclusion")),
					   diagnosticScore(diagnostic, QStringLiteral("semanticEndingResolution")));
	const double hook = std::max(diagnosticScore(diagnostic, QStringLiteral("hook")),
				     diagnosticScore(diagnostic, QStringLiteral("semanticHook")));
	const double topicShift = std::max(diagnosticScore(diagnostic, QStringLiteral("semanticTopicShift")),
					   diagnosticScore(diagnostic, QStringLiteral("semanticEndingTopicShift")));
	const double viewerCue = std::max(diagnosticScore(diagnostic, QStringLiteral("viewerResponse")),
					  diagnosticScore(diagnostic, QStringLiteral("semanticViewerMessage")));
	const double metaNoise =
		std::max(diagnosticScore(diagnostic, QStringLiteral("semanticMetaNoise")),
			 std::max(diagnosticScore(diagnostic, QStringLiteral("semanticOpeningMetaNoise")),
				  diagnosticScore(diagnostic, QStringLiteral("semanticEndingMetaNoise"))));

	QString recommendation;
	if (rejected) {
		recommendation =
			QStringLiteral("Recomendação: ajustar ou rejeitar. O gate marcou o candidato como rejeitado");
		if (!rejectionReason.isEmpty())
			recommendation += QStringLiteral(" por '%1'").arg(rejectionReason);
		recommendation += QLatin1Char('.');
	} else if (conclusion < 0.42) {
		recommendation = QStringLiteral("Recomendação: revisar o final. O fechamento/conclusão parece fraco.");
	} else if (opening < 0.42 || hook < 0.42) {
		recommendation = QStringLiteral("Recomendação: revisar o início. O começo ou gancho parece fraco.");
	} else if (topicShift >= 0.60) {
		recommendation = QStringLiteral("Recomendação: ajustar ou rejeitar. Há risco de troca de tópico.");
	} else {
		recommendation = QStringLiteral(
			"Recomendação: provável candidato aprovável, mas revise o texto e o final antes de confirmar.");
	}

	QStringList lines;
	lines << QStringLiteral("Range: %1 → %2 (%3s)").arg(startLabel, endLabel).arg(durationSec, 0, 'f', 1);
	lines << QStringLiteral("Origem: %1").arg(source.isEmpty() ? QStringLiteral("desconhecida") : source);
	if (!kind.isEmpty())
		lines << QStringLiteral("Tipo de diagnóstico: %1").arg(kind);
	lines << QStringLiteral("Status: %1")
			 .arg(rejected ? QStringLiteral("rejeitado pelo gate")
				       : QStringLiteral("selecionado pelo pipeline"));
	if (!rejectionReason.isEmpty())
		lines << QStringLiteral("Motivo de rejeição: %1").arg(rejectionReason);
	lines << QStringLiteral("Score final: %1").arg(finalScore, 0, 'f', 3);
	lines << QString();
	lines << QStringLiteral("Resumo");
	lines << recommendation;
	lines << QString();
	lines << QStringLiteral("Começo");
	lines << QStringLiteral("- Início/hook de abertura: %1 (%2)")
			 .arg(diagnosticScoreLabel(opening))
			 .arg(opening, 0, 'f', 2);
	lines << QStringLiteral("- Gancho geral: %1 (%2)").arg(diagnosticScoreLabel(hook)).arg(hook, 0, 'f', 2);
	lines << QStringLiteral("- Indício de mensagem/resposta de viewer: %1 (%2)")
			 .arg(diagnosticScoreLabel(viewerCue))
			 .arg(viewerCue, 0, 'f', 2);
	lines << QString();
	lines << QStringLiteral("Meio");
	lines << QStringLiteral("- Desenvolvimento/resposta direta: %1 (%2)")
			 .arg(diagnosticScoreLabel(development))
			 .arg(development, 0, 'f', 2);
	lines << QStringLiteral("- Troca de tópico: %1 (%2)")
			 .arg(topicShift >= 0.60 ? QStringLiteral("risco") : QStringLiteral("baixo risco"))
			 .arg(topicShift, 0, 'f', 2);
	lines << QString();
	lines << QStringLiteral("Final");
	lines << QStringLiteral("- Conclusão/resolução: %1 (%2)")
			 .arg(diagnosticScoreLabel(conclusion))
			 .arg(conclusion, 0, 'f', 2);
	lines << QStringLiteral("- Ruído/meta no trecho: %1 (%2)")
			 .arg(metaNoise >= 0.60 ? QStringLiteral("risco") : QStringLiteral("baixo risco"))
			 .arg(metaNoise, 0, 'f', 2);

	const QString preview = diagnostic.value(QStringLiteral("timed_text_preview")).toString().trimmed().isEmpty()
					? diagnostic.value(QStringLiteral("text_preview")).toString().trimmed()
					: diagnostic.value(QStringLiteral("timed_text_preview")).toString().trimmed();
	if (!preview.isEmpty()) {
		lines << QString();
		lines << QStringLiteral("Texto do candidato");
		lines << preview.left(2200);
	}

	const QString evidence = diagnosticEvidenceText(diagnostic);
	if (!evidence.isEmpty()) {
		lines << QString();
		lines << QStringLiteral("Evidências brutas");
		lines << evidence;
	}

	if (diagnostic.isEmpty()) {
		lines.clear();
		lines << QStringLiteral("Range: %1 → %2 (%3s)").arg(startLabel, endLabel).arg(durationSec, 0, 'f', 1);
		lines << QStringLiteral(
			"Nenhum diagnóstico semanticamente associado foi encontrado para este marcador.");
		lines << QStringLiteral("Você ainda pode aprovar, rejeitar ou ajustar com feedback estruturado.");
	}

	return lines.join(QChar('\n'));
}

class MarkerDiagnosticsDialog final : public QDialog {
public:
	MarkerDiagnosticsDialog(const ClipDuration &range, const QJsonObject &diagnostic, const QString &startLabel,
				const QString &endLabel, QWidget *parent = nullptr)
		: QDialog(parent)
	{
		setWindowTitle(QStringLiteral("Marker diagnostics"));
		resize(760, 620);
		auto *layout = new QVBoxLayout(this);
		layout->addWidget(new QLabel(QStringLiteral("Diagnóstico do marcador selecionado"), this));
		auto *text = new QPlainTextEdit(this);
		text->setReadOnly(true);
		text->setPlainText(diagnosticSummaryText(range, diagnostic, startLabel, endLabel));
		layout->addWidget(text, 1);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		layout->addWidget(buttons);
	}
};

class StructuredFeedbackDialog final : public QDialog {
public:
	StructuredFeedbackDialog(const QString &decision, const ClipDuration &range, const QJsonObject &diagnostic,
				 const QString &startLabel, const QString &endLabel, QWidget *parent = nullptr)
		: QDialog(parent),
		  normalizedDecision(decision.trimmed().toLower()),
		  diagnosticSnapshot(diagnostic)
	{
		setWindowTitle(QStringLiteral("Structured marker feedback"));
		resize(560, 520);
		auto *layout = new QVBoxLayout(this);
		layout->addWidget(new QLabel(
			normalizedDecision == QStringLiteral("approved_adjusted")
				? QStringLiteral(
					  "Confirme que o range editado ficou bom como clip completo. Esses sinais serão usados como exemplo positivo.")
				: QStringLiteral("Marque os sinais que você percebeu neste candidato."),
			this));
		layout->addWidget(new QLabel(QStringLiteral("Range: %1 → %2 (%3s)")
						     .arg(startLabel, endLabel)
						     .arg(std::max(0.0, range.endSec - range.startSec), 0, 'f', 1),
					     this));

		auto *structureGroup = new QGroupBox(QStringLiteral("Estrutura do clip"), this);
		auto *structureLayout = new QVBoxLayout(structureGroup);
		hasBeginning = addBox(structureLayout, QStringLiteral("Tem começo/contexto claro"));
		hasDevelopment = addBox(structureLayout, QStringLiteral("Tem desenvolvimento/resposta"));
		hasConclusion = addBox(structureLayout, QStringLiteral("Tem conclusão/resolução"));
		hasSingleTopic = addBox(structureLayout, QStringLiteral("Mantém um único assunto"));
		hasSmoothEnding = addBox(structureLayout, QStringLiteral("Final fica suave/natural"));
		layout->addWidget(structureGroup);

		auto *qualityGroup = new QGroupBox(QStringLiteral("Qualidade e defeitos"), this);
		auto *qualityLayout = new QVBoxLayout(qualityGroup);
		hasGoodHook = addBox(qualityLayout, QStringLiteral("Tem gancho bom"));
		hasViewerCue =
			addBox(qualityLayout, QStringLiteral("Parece responder uma mensagem/pergunta de viewer"));
		goodTopicBadBoundary =
			addBox(qualityLayout, QStringLiteral("Assunto parece bom, mas o corte/boundary está ruim"));
		incompleteButRecoverable =
			addBox(qualityLayout, QStringLiteral("Arco incompleto, mas recuperável ajustando início/fim"));
		badTopic = addBox(qualityLayout, QStringLiteral("Tema/candidato não vale clip"));
		ignoreForTraining =
			addBox(qualityLayout, QStringLiteral("Diagnóstico inútil / ignorar para treino forte"));
		hasTopicShift = addBox(qualityLayout, QStringLiteral("Tem troca de tópico"));
		startsTooLate = addBox(qualityLayout, QStringLiteral("Começa tarde demais"));
		startsTooEarly = addBox(qualityLayout, QStringLiteral("Começa cedo demais"));
		endsTooEarly = addBox(qualityLayout, QStringLiteral("Termina cedo demais"));
		overextended = addBox(qualityLayout, QStringLiteral("Passa do ponto / continua depois da resolução"));
		hasMetaNoise =
			addBox(qualityLayout, QStringLiteral("Tem ruído, meta, intro, música ou gestão da live"));
		layout->addWidget(qualityGroup);

		notesInput = new QPlainTextEdit(this);
		notesInput->setPlaceholderText(
			QStringLiteral("Observações opcionais: por que aprovou, rejeitou ou ajustou?"));
		notesInput->setMaximumHeight(96);
		layout->addWidget(notesInput);

		prefillFromDiagnostic(diagnostic);
		if (normalizedDecision == QStringLiteral("approved_adjusted"))
			prefillAsCompletePositiveExample();
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons);
	}

	QJsonObject feedback() const
	{
		QJsonObject object;
		object.insert(QStringLiteral("schema_version"), 1);
		object.insert(QStringLiteral("decision"), normalizedDecision);
		if (normalizedDecision == QStringLiteral("approved_adjusted")) {
			object.insert(QStringLiteral("approved_corrected_range"), true);
			object.insert(QStringLiteral("semantic_positive_example"), true);
		}
		const bool forceCompletePositive = normalizedDecision == QStringLiteral("approved_adjusted");
		const bool explicitIgnore = normalizedDecision == QStringLiteral("ignored_diagnostic");
		object.insert(QStringLiteral("has_beginning"),
			      forceCompletePositive || (hasBeginning && hasBeginning->isChecked()));
		object.insert(QStringLiteral("has_development"),
			      forceCompletePositive || (hasDevelopment && hasDevelopment->isChecked()));
		object.insert(QStringLiteral("has_conclusion"),
			      forceCompletePositive || (hasConclusion && hasConclusion->isChecked()));
		object.insert(QStringLiteral("has_single_topic"),
			      forceCompletePositive || (hasSingleTopic && hasSingleTopic->isChecked()));
		object.insert(QStringLiteral("has_smooth_ending"),
			      forceCompletePositive || (hasSmoothEnding && hasSmoothEnding->isChecked()));
		object.insert(QStringLiteral("has_good_hook"),
			      forceCompletePositive || (hasGoodHook && hasGoodHook->isChecked()));
		object.insert(QStringLiteral("has_viewer_cue"),
			      forceCompletePositive || (hasViewerCue && hasViewerCue->isChecked()));
		const bool goodTopicBoundary = goodTopicBadBoundary && goodTopicBadBoundary->isChecked();
		const bool incompleteRecoverable = incompleteButRecoverable && incompleteButRecoverable->isChecked();
		const bool userIgnored = explicitIgnore || (ignoreForTraining && ignoreForTraining->isChecked());
		const bool topicBad = (forceCompletePositive || userIgnored) ? false
									     : (badTopic && badTopic->isChecked());
		object.insert(QStringLiteral("ignore_for_training"), userIgnored);
		object.insert(QStringLiteral("weak_negative"),
			      userIgnored || (normalizedDecision == QStringLiteral("disliked") && !topicBad &&
					      isLowSignalDiagnostic(diagnosticSnapshot)));
		object.insert(QStringLiteral("good_topic_bad_boundary"), userIgnored ? false : goodTopicBoundary);
		object.insert(QStringLiteral("incomplete_but_recoverable"),
			      userIgnored ? false : incompleteRecoverable);
		object.insert(QStringLiteral("bad_topic"), topicBad);
		object.insert(QStringLiteral("boundary_recoverable"),
			      !userIgnored && !topicBad && (goodTopicBoundary || incompleteRecoverable));
		object.insert(QStringLiteral("has_topic_shift"),
			      forceCompletePositive ? false : (hasTopicShift && hasTopicShift->isChecked()));
		object.insert(QStringLiteral("starts_too_late"),
			      forceCompletePositive ? false : (startsTooLate && startsTooLate->isChecked()));
		object.insert(QStringLiteral("starts_too_early"),
			      forceCompletePositive ? false : (startsTooEarly && startsTooEarly->isChecked()));
		object.insert(QStringLiteral("ends_too_early"),
			      forceCompletePositive ? false : (endsTooEarly && endsTooEarly->isChecked()));
		object.insert(QStringLiteral("overextended_after_resolution"),
			      forceCompletePositive ? false : (overextended && overextended->isChecked()));
		object.insert(QStringLiteral("has_meta_noise"),
			      forceCompletePositive ? false : (hasMetaNoise && hasMetaNoise->isChecked()));
		const QString notes = notesInput ? notesInput->toPlainText().trimmed() : QString{};
		if (!notes.isEmpty())
			object.insert(QStringLiteral("notes"), notes.left(1200));
		return object;
	}

private:
	QString normalizedDecision;
	QJsonObject diagnosticSnapshot;
	QCheckBox *hasBeginning = nullptr;
	QCheckBox *hasDevelopment = nullptr;
	QCheckBox *hasConclusion = nullptr;
	QCheckBox *hasSingleTopic = nullptr;
	QCheckBox *hasSmoothEnding = nullptr;
	QCheckBox *hasGoodHook = nullptr;
	QCheckBox *hasViewerCue = nullptr;
	QCheckBox *goodTopicBadBoundary = nullptr;
	QCheckBox *incompleteButRecoverable = nullptr;
	QCheckBox *badTopic = nullptr;
	QCheckBox *ignoreForTraining = nullptr;
	QCheckBox *hasTopicShift = nullptr;
	QCheckBox *startsTooLate = nullptr;
	QCheckBox *startsTooEarly = nullptr;
	QCheckBox *endsTooEarly = nullptr;
	QCheckBox *overextended = nullptr;
	QCheckBox *hasMetaNoise = nullptr;
	QPlainTextEdit *notesInput = nullptr;

	static QCheckBox *addBox(QVBoxLayout *layout, const QString &label)
	{
		auto *box = new QCheckBox(label);
		layout->addWidget(box);
		return box;
	}

	static bool isLowSignalDiagnostic(const QJsonObject &diagnostic)
	{
		const QString reason = diagnostic.value(QStringLiteral("rejection_reason")).toString();
		return reason.contains(QStringLiteral("too_short"), Qt::CaseInsensitive) ||
		       reason.contains(QStringLiteral("novelty_exploration_review_required"), Qt::CaseInsensitive) ||
		       reason.contains(QStringLiteral("incomplete_viewer_arc"), Qt::CaseInsensitive);
	}

	void prefillAsCompletePositiveExample()
	{
		if (hasBeginning)
			hasBeginning->setChecked(true);
		if (hasDevelopment)
			hasDevelopment->setChecked(true);
		if (hasConclusion)
			hasConclusion->setChecked(true);
		if (hasSingleTopic)
			hasSingleTopic->setChecked(true);
		if (hasSmoothEnding)
			hasSmoothEnding->setChecked(true);
		if (hasGoodHook)
			hasGoodHook->setChecked(true);
		if (hasViewerCue)
			hasViewerCue->setChecked(true);
		if (badTopic)
			badTopic->setChecked(false);
		if (ignoreForTraining)
			ignoreForTraining->setChecked(false);
		if (hasTopicShift)
			hasTopicShift->setChecked(false);
		if (startsTooLate)
			startsTooLate->setChecked(false);
		if (startsTooEarly)
			startsTooEarly->setChecked(false);
		if (endsTooEarly)
			endsTooEarly->setChecked(false);
		if (overextended)
			overextended->setChecked(false);
		if (hasMetaNoise)
			hasMetaNoise->setChecked(false);
	}

	void prefillFromDiagnostic(const QJsonObject &diagnostic)
	{
		const double opening = std::max(diagnosticScore(diagnostic, QStringLiteral("arcOpening")),
						diagnosticScore(diagnostic, QStringLiteral("semanticOpeningHook")));
		const double development =
			std::max(diagnosticScore(diagnostic, QStringLiteral("arcDevelopment")),
				 diagnosticScore(diagnostic, QStringLiteral("semanticDirectAnswer")));
		const double conclusion =
			std::max(diagnosticScore(diagnostic, QStringLiteral("arcConclusion")),
				 diagnosticScore(diagnostic, QStringLiteral("semanticEndingResolution")));
		const double hook = std::max(diagnosticScore(diagnostic, QStringLiteral("hook")),
					     diagnosticScore(diagnostic, QStringLiteral("semanticHook")));
		const double topicShift =
			std::max(diagnosticScore(diagnostic, QStringLiteral("semanticTopicShift")),
				 diagnosticScore(diagnostic, QStringLiteral("semanticEndingTopicShift")));
		const double viewerCue = std::max(diagnosticScore(diagnostic, QStringLiteral("viewerResponse")),
						  diagnosticScore(diagnostic, QStringLiteral("semanticViewerMessage")));
		const double metaNoise =
			std::max(diagnosticScore(diagnostic, QStringLiteral("semanticMetaNoise")),
				 std::max(diagnosticScore(diagnostic, QStringLiteral("semanticOpeningMetaNoise")),
					  diagnosticScore(diagnostic, QStringLiteral("semanticEndingMetaNoise"))));

		if (hasBeginning)
			hasBeginning->setChecked(opening >= 0.42 ||
						 diagnosticHasEvidence(diagnostic, QStringLiteral("opening")));
		if (hasDevelopment)
			hasDevelopment->setChecked(development >= 0.42 ||
						   diagnosticHasEvidence(diagnostic, QStringLiteral("direct_answer")));
		if (hasConclusion)
			hasConclusion->setChecked(conclusion >= 0.42 ||
						  diagnosticHasEvidence(diagnostic, QStringLiteral("resolution")));
		if (hasSingleTopic)
			hasSingleTopic->setChecked(topicShift < 0.60);
		if (hasSmoothEnding)
			hasSmoothEnding->setChecked(conclusion >= 0.52 && topicShift < 0.60);
		if (hasGoodHook)
			hasGoodHook->setChecked(hook >= 0.48);
		if (hasViewerCue)
			hasViewerCue->setChecked(viewerCue >= 0.42 ||
						 diagnosticHasEvidence(diagnostic, QStringLiteral("viewer")));

		const QString rejection = diagnostic.value(QStringLiteral("rejection_reason")).toString();
		const bool incompleteArc =
			rejection.contains(QStringLiteral("incomplete_viewer_arc"), Qt::CaseInsensitive);
		const bool looksRecoverable = incompleteArc && topicShift < 0.70 && metaNoise < 0.70;
		if (goodTopicBadBoundary)
			goodTopicBadBoundary->setChecked(looksRecoverable);
		if (incompleteButRecoverable)
			incompleteButRecoverable->setChecked(looksRecoverable);
		if (badTopic)
			badTopic->setChecked(!looksRecoverable &&
					     diagnosticScore(diagnostic, QStringLiteral("semanticClipValue")) > 0.0 &&
					     diagnosticScore(diagnostic, QStringLiteral("semanticClipValue")) < 0.25);
		if (ignoreForTraining && normalizedDecision == QStringLiteral("ignored_diagnostic"))
			ignoreForTraining->setChecked(true);
		else if (ignoreForTraining && normalizedDecision == QStringLiteral("disliked") &&
			 isLowSignalDiagnostic(diagnostic))
			ignoreForTraining->setChecked(true);

		if (hasTopicShift)
			hasTopicShift->setChecked(topicShift >= 0.60);
		if (hasMetaNoise)
			hasMetaNoise->setChecked(metaNoise >= 0.60 ||
						 diagnosticHasEvidence(diagnostic, QStringLiteral("meta")));

		if (endsTooEarly)
			endsTooEarly->setChecked(rejection.contains(QStringLiteral("incomplete"), Qt::CaseInsensitive));
		if (overextended)
			overextended->setChecked(
				rejection.contains(QStringLiteral("overextended"), Qt::CaseInsensitive));
	}
};

} // namespace

void showMarkerDiagnosticsDialog(const ClipDuration &range, const QJsonObject &diagnostic, const QString &startLabel,
				 const QString &endLabel, QWidget *parent)
{
	MarkerDiagnosticsDialog dialog(range, diagnostic, startLabel, endLabel, parent);
	dialog.exec();
}

std::optional<QJsonObject> collectStructuredFeedbackDialog(const QString &decision, const ClipDuration &range,
							   const QJsonObject &diagnostic, const QString &startLabel,
							   const QString &endLabel, QWidget *parent)
{
	StructuredFeedbackDialog dialog(decision, range, diagnostic, startLabel, endLabel, parent);
	if (dialog.exec() != QDialog::Accepted)
		return std::nullopt;
	return dialog.feedback();
}

} // namespace ClipCropper::Review
