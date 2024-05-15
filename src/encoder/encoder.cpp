#include "encoder/encoder.hpp"

#include "formats/metadata.hpp"
#include "notifier/notifier.hpp"

#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QTime>
#include <QUrl>
#include <QVariant>

MediaEncoder::MediaEncoder(QObject* parent)
    : QObject { parent }

{
    ffmpeg->setProcessChannelMode(QProcess::MergedChannels);
    connect(ffmpeg, &QProcess::errorOccurred, [this](QProcess::ProcessError error) {
        emit encodingFailed(tr("Process %1").arg(QVariant::fromValue(error).toString()));
    });
}

MediaEncoder::~MediaEncoder()
{
    delete ffmpeg;
    delete ffprobe;
}

void MediaEncoder::Encode(const Options& options)
{
    if (!validateOptions(options)) {
        return;
    }

    Metadata metadata;

    if (!options.inputMetadata.has_value()) {
        std::variant<Metadata, Metadata::Error> result = getMetadata(options.inputPath);

        if (std::holds_alternative<Metadata::Error>(result)) {
            Metadata::Error error = std::get<Metadata::Error>(result);

            emit encodingFailed(error.summary, error.details);
            return;
        }

        metadata = std::get<Metadata>(result);
    } else {
        metadata = *options.inputMetadata;
    }

    ComputedOptions computed;

    if (options.audioCodec.has_value()) {
        if (!computeAudioBitrate(options, computed))
            return;
    }

    if (options.videoCodec.has_value() && options.sizeKbps.has_value()) {
        ComputeVideoBitrate(options, computed, metadata);
    }

    StartCompression(options, computed, metadata);
}

void MediaEncoder::StartCompression(const Options& options, const ComputedOptions& computed, const Metadata& metadata)
{
    emit encodingStarted(computed.videoBitrateKbps.value_or(0), computed.audioBitrateKbps.value_or(0));

    QString videoCodecParam = options.videoCodec.has_value() ? "-c:v " + options.videoCodec->libraryName
                                                             : "-vn";
    QString audioCodecParam = options.audioCodec.has_value() ? "-c:a " + options.audioCodec->libraryName
                                                             : "-an";
    QString videoBitrateParam = options.sizeKbps.has_value()
                                  ? "-b:v " + QString::number(*computed.videoBitrateKbps) + "k"
                                  : "";
    QString audioBitrateParam = computed.audioBitrateKbps.has_value()
                                  ? "-b:a " + QString::number(*computed.audioBitrateKbps) + "k"
                                  : "";
    QString formatParam = QString("-f %1").arg(options.container->formatName);

    // video filters
    QString aspectRatioFilter;
    QString scaleFilter;
    QString speedFilter;
    QString fpsFilter;
    double fps = *options.fps;

    if (options.outputWidth.has_value() && options.outputHeight.has_value()) {
        scaleFilter = QString("scale=%1:%2")
                          .arg(QString::number(*options.outputWidth), QString::number(*options.outputHeight));
        aspectRatioFilter = "setsar=1/1";
    } else if (options.outputWidth.has_value()) {
        scaleFilter = QString("scale=%1:-2").arg(QString::number(*options.outputWidth));
    } else if (options.outputHeight.has_value()) {
        scaleFilter = QString("scale=-1:%1").arg(QString::number(*options.outputHeight));
    }

    if (options.aspectRatio.has_value()) {
        aspectRatioFilter = QString("setsar=%1/%2")
                                .arg(QString::number(options.aspectRatio->y()), QString::number(options.aspectRatio->x()));
    }

    if (options.speed.has_value()) {
        speedFilter = QString("setpts=%1*PTS").arg(QString::number(1.0 / *options.speed));
        fps *= *options.speed;
    }

    if (options.fps.has_value()) {
        fpsFilter = "fps=" + QString::number(fps);
    }

    QString videoFiltersParam;
    QStringList videoFilters = QStringList { scaleFilter, aspectRatioFilter, speedFilter, fpsFilter };
    videoFilters.removeAll({});

    if (videoFilters.length() > 0)
        videoFiltersParam = "-filter:v " + videoFilters.join(',');

    // audio filters
    QString audioSpeedFilter;

    if (options.speed.has_value()) {
        audioSpeedFilter = "atempo=" + QString::number(*options.speed);
    }

    QString audioFiltersParam;
    QStringList audioFilters = QStringList { audioSpeedFilter };
    audioFilters.removeAll({});

    if (audioFilters.length() > 0)
        audioFiltersParam = "-filter:a " + audioFilters.join(',');

    auto maybeFileExtension = extensionForContainer(*options.container);
    if (std::holds_alternative<Message>(maybeFileExtension)) {
        emit encodingFailed(std::get<Message>(maybeFileExtension).message); // TODO: Use Message instead
        return;
    }
    QString fileExtension = std::get<QString>(maybeFileExtension);
    QString outputPath = options.outputPath + "." + fileExtension;

    QString command = QString(R"(ffmpeg%1 -i "%2" %3 %4 %5 %6 %7 %8 %9 %10 "%11" -y)")
                          .arg(IS_WINDOWS ? ".exe" : "", options.inputPath, videoCodecParam, audioCodecParam, videoBitrateParam, audioBitrateParam, videoFiltersParam, audioFiltersParam, *options.customArguments, formatParam, outputPath);

    *processUpdateConnection = connect(ffmpeg, &QProcess::readyRead, [metadata, this]() {
        UpdateProgress(metadata.durationSeconds);
    });

    *processFinishedConnection = connect(ffmpeg, &QProcess::finished, [=, this](int exitCode) {
        EndCompression(options, computed, outputPath, command, exitCode);
    });

    ffmpeg->startCommand(command);
}

void MediaEncoder::UpdateProgress(double mediaDuration)
{
    QString line = QString(ffmpeg->readAll());
    QRegularExpression regex("time=([0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9])");
    QRegularExpressionMatch match = regex.match(line);

    output += line;

    if (!match.hasMatch())
        return;

    QTime timestamp = QTime::fromString(match.captured(1));
    int currentDuration = timestamp.second() + timestamp.minute() * 60 + timestamp.hour() * 3600;
    int progressPercent = currentDuration * 100 / mediaDuration;

    emit encodingProgressUpdate(progressPercent);
}

void MediaEncoder::EndCompression(const Options& options, const ComputedOptions& computed, QString outputPath, QString command, int exitCode)
{
    if (exitCode != 0) {
        disconnect(*processUpdateConnection);
        disconnect(*processFinishedConnection);

        emit encodingFailed(parseOutput(), command + "\n\n" + output);
        output.clear();
        return;
    }

    QFile media(outputPath);
    if (!media.open(QIODevice::ReadOnly)) {
        disconnect(*processUpdateConnection);
        disconnect(*processFinishedConnection);
        emit encodingFailed("Could not open the compressed media.", media.errorString());
        output.clear();
        media.close();
        return;
    }

    media.close();
    disconnect(*processUpdateConnection);
    disconnect(*processFinishedConnection);
    emit encodingSucceeded(options, computed, media);
    output.clear();
}

QString MediaEncoder::getAvailableFormats()
{
    ffmpeg->startCommand(QString("ffmpeg%1 -encoders").arg(IS_WINDOWS ? ".exe" : ""));
    ffmpeg->waitForFinished();
    return ffmpeg->readAllStandardOutput();
}

bool MediaEncoder::computeAudioBitrate(const Options& options, ComputedOptions& computed)
{
    double audioBitrateKbps = qMax(options.minAudioBitrateKbps, options.audioQualityPercent.value_or(1) * options.maxAudioBitrateKbps);
    // TODO: Using the strategy pattern, specialize certain codecs to use different bitrate formulas

    computed.audioBitrateKbps = audioBitrateKbps;
    return true;
}

double MediaEncoder::computePixelRatio(const Options& options, const Metadata& metadata)
{
    double pixelRatio = 1;
    int outputWidth;
    int outputHeight;

    long inputPixelCount = metadata.width * metadata.height;

    if (options.outputWidth.has_value()) {
        outputHeight = *options.outputHeight;
        outputWidth = outputHeight * metadata.aspectRatioX / metadata.aspectRatioY;
    } else {
        outputWidth = *options.outputWidth;
        outputHeight = outputWidth * metadata.aspectRatioX / metadata.aspectRatioY;
    }

    double outputPixelCount = outputWidth * outputHeight;

    // TODO: Add option to enable bitrate compensation even when upscaling (will result in bigger files)
    if (outputPixelCount > 0 && outputPixelCount < inputPixelCount)
        pixelRatio = outputPixelCount / inputPixelCount;

    return pixelRatio;
}

std::variant<QString, Message> MediaEncoder::extensionForContainer(const Container& container)
{
    QProcess process;
    QString command = QString("ffmpeg -hide_banner -h muxer=%1").arg(container.formatName);

    process.startCommand(command);

    bool result = process.waitForFinished(10000);
    if (!result) {
        return Message(
            Severity::Critical,
            tr("Failed to query file extension for container"),
            tr("FFmpeg did not respond in time to query the file extension for container %1.")
                .arg(container.formatName),
            process.readAllStandardError()
        );
    }

    QString output = process.readAllStandardOutput();
    static QRegularExpression regex(R"(Common extensions: (.+(?=\.)))");
    QRegularExpressionMatch match = regex.match(output);

    if (!match.hasMatch()) {
        return Message(
            Severity::Critical,
            tr("Failed to query file extension for container"),
            tr("FFmpeg did not return a file extension for container %1.")
                .arg(container.formatName),
            output
        );
    }

    QStringList extensions = match.captured(1).split(",");

    if (extensions.isEmpty()) {
        return Message(
            Severity::Critical,
            tr("Failed to query file extension for container"),
            tr("FFmpeg did not return a file extension for container %1.")
                .arg(container.formatName),
            output
        );
    }

    return extensions.first().trimmed();
}

void MediaEncoder::ComputeVideoBitrate(const Options& options, ComputedOptions& computed, const Metadata& metadata)
{
    double audioBitrateKbps = computed.audioBitrateKbps.value_or(0);

    double pixelRatio = computePixelRatio(options, metadata);
    double bitrateKbps = *options.sizeKbps / metadata.durationSeconds * (1.0 - options.overshootCorrectionPercent);

    computed.videoBitrateKbps = qMax(options.minVideoBitrateKbps, pixelRatio * (bitrateKbps - audioBitrateKbps));
}

QString MediaEncoder::parseOutput()
{
    QStringList split = output.split("Press [q] to stop, [?] for help");
    if (split.length() == 1)
        split = output.split("[0][0][0][0]");

    return split.last()
        .replace(
            QRegularExpression(
                R"((\[.*\]|(?:Conversion failed!)|(?:v\d\.\d.*)|(?: (?:\s)+)|(?:- (?:\s)+(?1))))"
            ),
            ""
        )
        .trimmed();
}

bool MediaEncoder::validateOptions(const Options& options)
{
    QString error;

    if (!options.videoCodec.has_value() && !options.audioCodec.has_value())
        error = tr("Neither a video nor an audio codec was selected.");

    if (!options.videoCodec.has_value() && !options.container.has_value())
        error = tr("A container was selected but audio-only encoding was specified.");

    if (options.outputWidth.has_value() && options.outputWidth < 0) {
        error = tr("Output width must be greater than 0, but was %1")
                    .arg(QString::number(*options.outputWidth));
    }
    if (options.outputHeight.has_value() && options.outputHeight < 0) {
        error = tr("Output height must be greater than 0, but was %1")
                    .arg(QString::number(*options.outputHeight));
    }

    auto aspect = options.aspectRatio;
    if (aspect.has_value() && aspect->x() <= 0) {
        error = tr("Invalid horizontal aspect %1").arg(QString::number(aspect->x()));
    }
    if (aspect.has_value() && aspect->y() <= 0) {
        error = tr("Invalid horizontal aspect %1").arg(QString::number(aspect->x()));
    }

    auto fps = options.fps;
    if (fps.has_value() && fps.value() < 0) {
        error = QString("Value for frames per seconds '%1' is out of range.")
                    .arg(QString::number(*fps));
    }

    auto speed = options.speed;
    if (speed.has_value() && speed.value() < 0) {
        error = QString("Value for speed '%1' is out of range.").arg(QString::number(*fps));
    }

    if (options.customArguments.has_value()
        && QRegularExpression("[^ -/a-z0-9]").match(*options.customArguments).hasMatch()) {
        error = tr("Custom parameters contain invalid characters.");
    }

    if (!error.isEmpty()) {
        emit encodingFailed(error, tr("rip bozo"));
        return false;
    }

    return true;
}

std::variant<Metadata, Metadata::Error> MediaEncoder::getMetadata(const QString& path)
{
    ffprobe->startCommand(
        QString(R"(ffprobe%1 -v error -print_format json -show_format -show_streams "%2")")
            .arg(IS_WINDOWS ? ".exe" : "", path)
    );
    ffprobe->waitForFinished();

    QByteArray data = ffprobe->readAll();
    Metadata::Builder builder;

    emit metadataComputed();
    return builder.fromJson(data);
}
