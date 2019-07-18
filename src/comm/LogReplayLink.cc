/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include "LogReplayLink.h"
#include "LinkManager.h"
#include "QGCApplication.h"

#include <QFileInfo>
#include <QtEndian>
#include <QSignalSpy>

const char*  LogReplayLinkConfiguration::_logFilenameKey = "logFilename";

LogReplayLinkConfiguration::LogReplayLinkConfiguration(const QString& name)
    : LinkConfiguration(name)
{
    
}

LogReplayLinkConfiguration::LogReplayLinkConfiguration(LogReplayLinkConfiguration* copy)
    : LinkConfiguration(copy)
{
    _logFilename = copy->logFilename();
}

void LogReplayLinkConfiguration::copyFrom(LinkConfiguration *source)
{
    LinkConfiguration::copyFrom(source);
    LogReplayLinkConfiguration* ssource = dynamic_cast<LogReplayLinkConfiguration*>(source);
    if (ssource) {
        _logFilename = ssource->logFilename();
    } else {
        qWarning() << "Internal error";
    }
}

void LogReplayLinkConfiguration::saveSettings(QSettings& settings, const QString& root)
{
    settings.beginGroup(root);
    settings.setValue(_logFilenameKey, _logFilename);
    settings.endGroup();
}

void LogReplayLinkConfiguration::loadSettings(QSettings& settings, const QString& root)
{
    settings.beginGroup(root);
    _logFilename = settings.value(_logFilenameKey, "").toString();
    settings.endGroup();
}

void LogReplayLinkConfiguration::updateSettings(void)
{
    // Doesn't support changing filename on the fly is already connected
}

QString LogReplayLinkConfiguration::logFilenameShort(void)
{
    QFileInfo fi(_logFilename);
    return fi.fileName();
}

LogReplayLink::LogReplayLink(SharedLinkConfigurationPointer& config)
    : LinkInterface(config)
    , _logReplayConfig(qobject_cast<LogReplayLinkConfiguration*>(config.data()))
    , _connected(false)
    , _replayAccelerationFactor(1.0f)
{
    if (!_logReplayConfig) {
        qWarning() << "Internal error";
    }

    _errorTitle = tr("Log Replay Error");
    
    _readTickTimer.moveToThread(this);
    
    QObject::connect(&_readTickTimer, &QTimer::timeout, this, &LogReplayLink::_readNextLogEntry);
    QObject::connect(this, &LogReplayLink::_playOnThread, this, &LogReplayLink::_play);
    QObject::connect(this, &LogReplayLink::_pauseOnThread, this, &LogReplayLink::_pause);
    QObject::connect(this, &LogReplayLink::_setAccelerationFactorOnThread, this, &LogReplayLink::_setAccelerationFactor);
    
    moveToThread(this);
}

LogReplayLink::~LogReplayLink(void)
{
    _disconnect();
}

bool LogReplayLink::_connect(void)
{
    // Disallow replay when any links are connected
    if (qgcApp()->toolbox()->multiVehicleManager()->activeVehicle()) {
        emit communicationError(_errorTitle, tr("You must close all connections prior to replaying a log."));
        return false;
    }

    _mavlinkChannel = qgcApp()->toolbox()->linkManager()->_reserveMavlinkChannel();
    if (_mavlinkChannel == 0) {
        qWarning() << "No mavlink channels available";
        return false;
    }

    if (isRunning()) {
        quit();
        wait();
    }
    start(HighPriority);
    return true;
}

void LogReplayLink::_disconnect(void)
{
    if (_connected) {
        quit();
        wait();
        _connected = false;

        if (_mavlinkChannel != 0) {
            qgcApp()->toolbox()->linkManager()->_freeMavlinkChannel(_mavlinkChannel);
        }

        emit disconnected();
    }
}

void LogReplayLink::run(void)
{
    // Load the log file
    if (!_loadLogFile()) {
        return;
    }
    
    _connected = true;
    emit connected();
    
    // Start playback
    _play();

    // Run normal event loop until exit
    exec();
    
    _readTickTimer.stop();
}

void LogReplayLink::_replayError(const QString& errorMsg)
{
    qDebug() << _errorTitle << errorMsg;
    emit communicationError(_errorTitle, errorMsg);
}

/// Since this is log replay, we just drops writes on the floor
void LogReplayLink::_writeBytes(const QByteArray bytes)
{
    Q_UNUSED(bytes);
}

/// Parses a BigEndian quint64 timestamp
/// @return A Unix timestamp in microseconds UTC for found message or 0 if parsing failed
quint64 LogReplayLink::_parseTimestamp(const QByteArray& bytes)
{
    quint64 timestamp = qFromBigEndian(*((quint64*)(bytes.constData())));
    quint64 currentTimestamp = ((quint64)QDateTime::currentMSecsSinceEpoch()) * 1000;
    
    // Now if the parsed timestamp is in the future, it must be an old file where the timestamp was stored as
    // little endian, so switch it.
    if (timestamp > currentTimestamp) {
        timestamp = qbswap(timestamp);
    }
    
    return timestamp;
}

/// Reads the next mavlink message from the log
///     @param bytes[output] Bytes for mavlink message
/// @return Unix timestamp in microseconds UTC for NEXT mavlink message or 0 if no message found
quint64 LogReplayLink::_readNextMavlinkMessage(QByteArray& bytes)
{
    char                nextByte;
    mavlink_status_t    status;

    bytes.clear();

    while (_logFile.getChar(&nextByte)) { // Loop over every byte
        mavlink_message_t message;
        bool messageFound = mavlink_parse_char(_mavlinkChannel, nextByte, &message, &status);

        if (status.parse_state == MAVLINK_PARSE_STATE_GOT_STX) {
            // This is the possible beginning of a mavlink message, clear any partial bytes
            bytes.clear();
        }
        bytes.append(nextByte);

        if (messageFound) {
            // Return the timestamp for the next message
            QByteArray rawTime = _logFile.read(cbTimestamp);
            return _parseTimestamp(rawTime);
        }
    }

    return 0;
}

/// Seeks to the beginning of the next successfully parsed mavlink message in the log file.
///     @param nextMsg[output] Parsed next message that was found
/// @return A Unix timestamp in microseconds UTC for found message or 0 if parsing failed
quint64 LogReplayLink::_seekToNextMavlinkMessage(mavlink_message_t* nextMsg)
{
    char                nextByte;
    mavlink_status_t    status;
    qint64              messageStartPos = -1;

    mavlink_reset_channel_status(_mavlinkChannel);

    while (_logFile.getChar(&nextByte)) {
        bool messageFound = mavlink_parse_char(_mavlinkChannel, nextByte, nextMsg, &status);

        if (status.parse_state == MAVLINK_PARSE_STATE_GOT_STX) {
            // This is the possible beginning of a mavlink message
            messageStartPos = _logFile.pos() - 1;
        }
        
        // If we've found a message, jump back to the start of the message, grab the timestamp,
        // and go back to the end of this file.
        if (messageFound && messageStartPos != -1) {
            _logFile.seek(messageStartPos - cbTimestamp);
            QByteArray rawTime = _logFile.read(cbTimestamp);
            return _parseTimestamp(rawTime);
        }
    }
    
    return 0;
}

quint64 LogReplayLink::_findLastTimestamp(void)
{
    char                nextByte;
    mavlink_status_t    status;
    quint64             lastTimestamp = 0;
    mavlink_message_t   msg;

    // We read through the entire file looking for the last good timestamp. This can be somewhat slow, but trying to work from the
    // end of the file can be way slower due to all the seeking back and forth required. So instead we take the simple reliable approach.

    _logFile.reset();
    mavlink_reset_channel_status(_mavlinkChannel);

    while (_logFile.bytesAvailable() > cbTimestamp) {
        lastTimestamp = _parseTimestamp(_logFile.read(cbTimestamp));

        bool endOfMessage = false;
        while (!endOfMessage && _logFile.getChar(&nextByte)) {
            endOfMessage = mavlink_parse_char(_mavlinkChannel, nextByte, &msg, &status);
        }
    }

    return lastTimestamp;
}

bool LogReplayLink::_loadLogFile(void)
{
    QString errorMsg;
    QString logFilename = _logReplayConfig->logFilename();
    QFileInfo logFileInfo;
    int logDurationSecondsTotal;
    
    if (_logFile.isOpen()) {
        errorMsg = tr("Attempt to load new log while log being played");
        goto Error;
    }
    
    _logFile.setFileName(logFilename);
    if (!_logFile.open(QFile::ReadOnly)) {
        errorMsg = tr("Unable to open log file: '%1', error: %2").arg(logFilename).arg(_logFile.errorString());
        goto Error;
    }
    logFileInfo.setFile(logFilename);
    _logFileSize = logFileInfo.size();
    
    _logTimestamped = logFilename.endsWith(".tlog");
    
    if (_logTimestamped) {
        quint64 startTimeUSecs = _parseTimestamp(_logFile.read(cbTimestamp));
        quint64 endTimeUSecs = _findLastTimestamp();

        if (endTimeUSecs <= startTimeUSecs) {
            errorMsg = tr("The log file '%1' is corrupt or empty.").arg(logFilename);
            goto Error;
        }
        
        // Remember the start and end time so we can move around this _logFile with the slider.
        _logEndTimeUSecs = endTimeUSecs;
        _logStartTimeUSecs = startTimeUSecs;
        _logDurationUSecs = endTimeUSecs - startTimeUSecs;
        _logCurrentTimeUSecs = startTimeUSecs;

        // Reset our log file so when we go to read it for the first time, we start at the beginning.
        _logFile.reset();
        
        logDurationSecondsTotal = (_logDurationUSecs) / 1000000;
    } else {
        // Load in binary mode. In this mode, files should be have a filename postfix
        // of the baud rate they were recorded at, like `test_run_115200.bin`. Then on
        // playback, the datarate is equal to set to this value.
        
        
        // Set baud rate if any present. Otherwise we default to 57600.
        QStringList parts = logFileInfo.baseName().split("_");
        _binaryBaudRate = _defaultBinaryBaudRate;
        if (parts.count() > 1)
        {
            bool ok;
            int rate = parts.last().toInt(&ok);
            // 9600 baud to 100 MBit
            if (ok && (rate > 9600 && rate < 100000000))
            {
                // Accept this as valid baudrate
                _binaryBaudRate = rate;
            }
        }
        
        logDurationSecondsTotal = logFileInfo.size() / (_binaryBaudRate / 10);
    }
    
    emit logFileStats(_logTimestamped, logDurationSecondsTotal, _binaryBaudRate);
    
    return true;
    
Error:
    if (_logFile.isOpen()) {
        _logFile.close();
    }
    _replayError(errorMsg);
    return false;
}

/// This function will read the next available log entry. It will then start
/// the _readTickTimer timer to read the new log entry at the appropriate time.
/// It might not perfectly match the timing of the log file, but it will never
/// induce a static drift into the log file replay.
void LogReplayLink::_readNextLogEntry(void)
{
    QByteArray bytes;

    // If we have a file with timestamps, try and pace this out following the time differences
    // between the timestamps and the current playback speed.
    if (_logTimestamped) {
        // Now parse MAVLink messages, grabbing their timestamps as we go. We stop once we
        // have at least 3ms until the next one.
        
        // We track what the next execution time should be in milliseconds, which we use to set
        // the next timer interrupt.
        int timeToNextExecutionMSecs = 0;
        
        while (timeToNextExecutionMSecs < 3) {
            // Read the next mavlink message from the log
            qint64 nextTimeUSecs = _readNextMavlinkMessage(bytes);
            emit bytesReceived(this, bytes);
            emit playbackPercentCompleteChanged(((float)(_logCurrentTimeUSecs - _logStartTimeUSecs) / (float)_logDurationUSecs) * 100);
            
            if (_logFile.atEnd()) {
                _finishPlayback();
                return;
            }
            
            _logCurrentTimeUSecs = nextTimeUSecs;
            
            // Calculate how long we should wait in real time until parsing this message.
            // We pace ourselves relative to the start time of playback to fix any drift (initially set in play())
            qint64 timeDiffMSecs = ((_logCurrentTimeUSecs - _logStartTimeUSecs) / 1000) / _replayAccelerationFactor;
            quint64 desiredPacedTimeMSecs = _playbackStartTimeMSecs + timeDiffMSecs;
            quint64 currentTimeMSecs = (quint64)QDateTime::currentMSecsSinceEpoch();
            timeToNextExecutionMSecs = desiredPacedTimeMSecs - currentTimeMSecs;
        }

        _signalCurrentLogTimeSecs();

        // And schedule the next execution of this function.
        _readTickTimer.start(timeToNextExecutionMSecs);
    }
    else
    {
        // Binary format - read at fixed rate
        const int len = 100;
        QByteArray chunk = _logFile.read(len);
        
        emit bytesReceived(this, chunk);
        emit playbackPercentCompleteChanged(((float)_logFile.pos() / (float)_logFileSize) * 100);
        
        // Check if reached end of file before reading next timestamp
        if (chunk.length() < len || _logFile.atEnd())
        {
            _finishPlayback();
            return;
        }
    }
    
}

void LogReplayLink::_play(void)
{
    qgcApp()->toolbox()->linkManager()->setConnectionsSuspended(tr("Connect not allowed during Flight Data replay."));
#ifndef __mobile__
    qgcApp()->toolbox()->mavlinkProtocol()->suspendLogForReplay(true);
#endif
    
    // Make sure we aren't at the end of the file, if we are, reset to the beginning and play from there.
    if (_logFile.atEnd()) {
        _resetPlaybackToBeginning();
    }
    
    // Always correct the current start time such that the next message will play immediately at playback.
    // We do this by subtracting the current file playback offset  from now()
    _playbackStartTimeMSecs = (quint64)QDateTime::currentMSecsSinceEpoch() - ((_logCurrentTimeUSecs - _logStartTimeUSecs) / 1000);
    
    // Start timer
    if (_logTimestamped) {
        _readTickTimer.start(1);
    } else {
        // Read len bytes at a time
        int len = 100;
        // Calculate the number of times to read 100 bytes per second
        // to guarantee the baud rate, then divide 1000 by the number of read
        // operations to obtain the interval in milliseconds
        int interval = 1000 / ((_binaryBaudRate / 10) / len);
        _readTickTimer.start(interval / _replayAccelerationFactor);
    }
    
    emit playbackStarted();
}

void LogReplayLink::_pause(void)
{
    qgcApp()->toolbox()->linkManager()->setConnectionsAllowed();
#ifndef __mobile__
    qgcApp()->toolbox()->mavlinkProtocol()->suspendLogForReplay(false);
#endif
    
    _readTickTimer.stop();
    
    emit playbackPaused();
}

void LogReplayLink::_resetPlaybackToBeginning(void)
{
    if (_logFile.isOpen()) {
        _logFile.reset();
    }
    
    // And since we haven't starting playback, clear the time of initial playback and the current timestamp.
    _playbackStartTimeMSecs = 0;
    _logCurrentTimeUSecs = _logStartTimeUSecs;
}

void LogReplayLink::movePlayhead(qreal percentComplete)
{
    if (isPlaying()) {
        _pauseOnThread();
        QSignalSpy waitForPause(this, SIGNAL(playbackPaused));
        waitForPause.wait();
        if (_readTickTimer.isActive()) {
            return;
        }
    }

    if (percentComplete < 0) {
        percentComplete = 0;
    }
    if (percentComplete > 100) {
        percentComplete = 100;
    }
    
    qreal percentCompleteMult = percentComplete / 100.0;
    
    if (_logTimestamped) {
        // But if we have a timestamped MAVLink log, then actually aim to hit that percentage in terms of
        // time through the file.
        qint64 newFilePos = (qint64)(percentCompleteMult * (qreal)_logFile.size());
        
        // Now seek to the appropriate position, failing gracefully if we can't.
        if (!_logFile.seek(newFilePos)) {
            _replayError(tr("Unable to seek to new position"));
            return;
        }
        
        // But we do align to the next MAVLink message for consistency.
        mavlink_message_t dummy;
        _logCurrentTimeUSecs = _seekToNextMavlinkMessage(&dummy);
        
        // Now calculate the current file location based on time.
        qreal newRelativeTimeUSecs = (qreal)(_logCurrentTimeUSecs - _logStartTimeUSecs);
        
        // Calculate the effective baud rate of the file in bytes/s.
        qreal baudRate = _logFile.size() / (qreal)_logDurationUSecs / 1e6;
        
        // And the desired time is:
        qreal desiredTimeUSecs = percentCompleteMult * _logDurationUSecs;
        
        // And now jump the necessary number of bytes in the proper direction
        qint64 offset = (newRelativeTimeUSecs - desiredTimeUSecs) * baudRate;
        if (!_logFile.seek(_logFile.pos() + offset)) {
            _replayError(tr("Unable to seek to new position"));
            return;
        }
        
        // And scan until we reach the start of a MAVLink message. We make sure to record this timestamp for
        // smooth jumping around the file.
        _logCurrentTimeUSecs = _seekToNextMavlinkMessage(&dummy);
        _signalCurrentLogTimeSecs();

        // Now update the UI with our actual final position.
        newRelativeTimeUSecs = (qreal)(_logCurrentTimeUSecs - _logStartTimeUSecs);
        percentComplete = (newRelativeTimeUSecs / _logDurationUSecs) * 100;
        emit playbackPercentCompleteChanged(percentComplete);
    } else {
        // If we're working with a non-timestamped file, we just jump to that percentage of the file,
        // align to the next MAVLink message and roll with it. No reason to do anything more complicated.
        qint64 newFilePos = (qint64)(percentCompleteMult * (qreal)_logFile.size());
        
        // Now seek to the appropriate position, failing gracefully if we can't.
        if (!_logFile.seek(newFilePos)) {
            _replayError(tr("Unable to seek to new position"));
            return;
        }
        
        // But we do align to the next MAVLink message for consistency.
        mavlink_message_t dummy;
        _seekToNextMavlinkMessage(&dummy);
    }
}

void LogReplayLink::_setAccelerationFactor(int factor)
{
    // factor: -100: 0.01X, 0: 1.0X, 100: 100.0X
    
    if (factor < 0) {
        _replayAccelerationFactor = 0.01f;
        factor -= -100;
        if (factor > 0) {
            _replayAccelerationFactor *= (float)factor;
        }
    } else if (factor > 0) {
        _replayAccelerationFactor = 1.0f * (float)factor;
    } else {
        _replayAccelerationFactor = 1.0f;
    }
    
    // Update timer interval
    if (!_logTimestamped) {
        // Read len bytes at a time
        int len = 100;
        // Calculate the number of times to read 100 bytes per second
        // to guarantee the baud rate, then divide 1000 by the number of read
        // operations to obtain the interval in milliseconds
        int interval = 1000 / ((_binaryBaudRate / 10) / len);
        _readTickTimer.stop();
        _readTickTimer.start(interval / _replayAccelerationFactor);
    }
}

/// @brief Called when playback is complete
void LogReplayLink::_finishPlayback(void)
{
    _pause();
    
    emit playbackAtEnd();
}

void LogReplayLink::_signalCurrentLogTimeSecs(void)
{
    emit currentLogTimeSecs((_logCurrentTimeUSecs - _logStartTimeUSecs) / 1000000);
}

LogReplayLinkController::LogReplayLinkController(void)
    : _link             (nullptr)
    , _isPlaying        (false)
    , _percentComplete  (0)
    , _playheadSecs     (0)
{

}

void LogReplayLinkController::setLink(LogReplayLink* link)
{
    if (_link) {
        disconnect(_link);
        _isPlaying = false;
        _percentComplete = 0;
        _playheadTime.clear();
        _totalTime.clear();
        _link = nullptr;
        emit isPlayingChanged(false);
        emit percentCompleteChanged(0);
        emit playheadTimeChanged(QString());
        emit totalTimeChanged(QString());
        emit linkChanged(nullptr);
    }


    if (link) {
        _link = link;
        connect(_link, &LogReplayLink::logFileStats,                      this, &LogReplayLinkController::_logFileStats);
        connect(_link, &LogReplayLink::playbackStarted,                   this, &LogReplayLinkController::_playbackStarted);
        connect(_link, &LogReplayLink::playbackPaused,                    this, &LogReplayLinkController::_playbackPaused);
        connect(_link, &LogReplayLink::playbackPercentCompleteChanged,    this, &LogReplayLinkController::_playbackPercentCompleteChanged);
        connect(_link, &LogReplayLink::currentLogTimeSecs,                this, &LogReplayLinkController::_currentLogTimeSecs);
        connect(_link, &LogReplayLink::disconnected,                      this, &LogReplayLinkController::_linkDisconnected);
        emit linkChanged(_link);
    }
}

void LogReplayLinkController::setIsPlaying(bool isPlaying)
{
    if (isPlaying) {
        _link->play();
    } else {
        _link->pause();
    }
}

void LogReplayLinkController::setPercentComplete(qreal percentComplete)
{
    _link->movePlayhead(percentComplete);
}

void LogReplayLinkController::_logFileStats(bool logTimestamped, int logDurationSecs, int binaryBaudRate)
{
    Q_UNUSED(logTimestamped);
    Q_UNUSED(binaryBaudRate);

    _totalTime = _secondsToHMS(logDurationSecs);
    emit totalTimeChanged(_totalTime);
}

void LogReplayLinkController::_playbackStarted(void)
{
    _isPlaying = true;
    emit isPlayingChanged(true);
}

void LogReplayLinkController::_playbackPaused(void)
{
    _isPlaying = false;
    emit isPlayingChanged(true);
}

void LogReplayLinkController::_playbackAtEnd(void)
{
    _isPlaying = false;
    emit isPlayingChanged(true);
}

void LogReplayLinkController::_playbackPercentCompleteChanged(qreal percentComplete)
{
    _percentComplete = percentComplete;
    emit percentCompleteChanged(_percentComplete);
}

void LogReplayLinkController::_currentLogTimeSecs(int secs)
{
    if (_playheadSecs != secs) {
        _playheadSecs = secs;
        _playheadTime = _secondsToHMS(secs);
        emit playheadTimeChanged(_playheadTime);
    }
}

void LogReplayLinkController::_linkDisconnected(void)
{
    setLink(nullptr);
}

QString LogReplayLinkController::_secondsToHMS(int seconds)
{
    int secondsPart  = seconds;
    int minutesPart  = secondsPart / 60;
    int hoursPart    = minutesPart / 60;
    secondsPart -= 60 * minutesPart;
    minutesPart -= 60 * hoursPart;

    return tr("%1h:%2m:%3s").arg(hoursPart, 2).arg(minutesPart, 2).arg(secondsPart, 2);
}

void LogReplayLink::_signalCurrentLogTimeSecs(void)
{
    emit currentLogTimeSecs((_logCurrentTimeUSecs - _logStartTimeUSecs) / 1000000);
}
