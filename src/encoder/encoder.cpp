/****************************************************************************
**
** QtMEL is a Qt Media Encoding Library that allows to encode video and audio streams
** Copyright (C) 2013 Kirill Bukaev(aka KIBSOFT).
** Contact: Kirill Bukaev (support@kibsoft.ru)
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
** Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public
** License along with this library; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
****************************************************************************/

#include "encoder.h"

#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif

//ffmpeg include files
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

#include <QMetaType>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QImage>

#define MAX_AUDIO_FRAME_SIZE 192000

class EncoderPrivate : public QObject {
    Q_OBJECT

public:
    explicit EncoderPrivate(Encoder *e, QObject *parent = 0);
    ~EncoderPrivate();

    void setFilePath(const QString &filePath);
    QString filePath() const;

    void setVideoSize(const QSize &size);
    QSize videoSize() const;

    void setFixedFrameRate(int frameRate);
    int fixedFrameRate() const;
    bool isFixedFrameRate() const;

    void setEncodingMode(Encoder::EncodingMode mode);
    Encoder::EncodingMode encodingMode() const;

    void setOutputPixelFormat(EncoderGlobal::PixelFormat format);
    EncoderGlobal::PixelFormat outputPixelFormat() const;

    void setVideoCodec(EncoderGlobal::VideoCodec codec);
    EncoderGlobal::VideoCodec videoCodec() const;

    void setAudioCodec(EncoderGlobal::AudioCodec codec);
    EncoderGlobal::AudioCodec audioCodec() const;

    void setVideoCodecSettings(const VideoCodecSettings &settings);
    VideoCodecSettings videoCodecSettings() const;

    void setAudioCodecSettings(const AudioCodecSettings &settings);
    AudioCodecSettings audioCodecSettings() const;

    int encodedFrameCount() const;
    int encodedAudioDataSize() const;

public Q_SLOTS:
    void start();
    void stop();

    void encodeVideoFrame(const QImage &frame, int pts);
    void encodeAudioData(const QByteArray &data);

private Q_SLOTS:
    void onError();

private:
    void initData();
    void initFfmpegStuff();
    void cleanup();

    bool createVideoStream();
    bool createAudioStream();

    bool openVideoStream();
    bool openAudioStream();

    bool convertImage(const QImage &image);
    EncoderGlobal::PixelFormat convertImagePixelFormat(QImage::Format format) const;

    void applyVideoCodecSettings();
    template <class T1, class T2> void setVideoCodecOption(T1 AVCodecContext::*option, T2 (VideoCodecSettings::*f)() const);

    void applyAudioCodecSettings();
    template <class T1, class T2> void setAudioCodecOption(T1 AVCodecContext::*option, T2 (AudioCodecSettings::*f)() const);

    Encoder *q_ptr;

    EncoderGlobal::PixelFormat m_outputPixelFormat;
    EncoderGlobal::VideoCodec m_videoCodecName;
    EncoderGlobal::AudioCodec m_audioCodecName;
    VideoCodecSettings m_videoSettings;
    AudioCodecSettings m_audioSettings;

    QString m_filePath;
    QSize m_videoSize;
    int m_fixedFrameRate;
    Encoder::EncodingMode m_encodingMode;

    int m_pts;
    int m_encodedFrameCount;
    int m_encodedAudioDataSize;

    //video stuff
    AVOutputFormat *m_outputFormat;
    AVFormatContext *m_formatContext;
    AVStream *m_videoStream;
    AVCodecContext *m_videoCodecContext;
    AVCodecParameters *m_videoCodecParams;
    AVCodec *m_videoCodec;
    AVFrame *m_videoPicture;
    SwsContext *m_imageConvertContext;

    //audio stuff
    AVStream *m_audioStream;
    AVCodecContext *m_audioCodecContext;
    AVCodecParameters *m_audioCodecParams;
    AVCodec *m_audioCodec;
    QByteArray m_audioInputBuffer;
    int m_audioSampleSize;

    mutable QMutex m_encodedFrameCountMutex;
    mutable QMutex m_encodedAudioDataSizeMutex;
};

EncoderPrivate::EncoderPrivate(Encoder *e, QObject *parent)
    : QObject(parent)
{
    //get the pointer to the public class
    q_ptr = e;

    initData();
    initFfmpegStuff();

    //cleanup ffmpeg stuff on error
    connect(q_ptr, SIGNAL(error(Encoder::Error)), this, SLOT(onError()));
}

EncoderPrivate::~EncoderPrivate()
{
}

void EncoderPrivate::setFilePath(const QString &fileName)
{
    if (m_filePath != fileName) {
        m_filePath = fileName;
    }
}

QString EncoderPrivate::filePath() const
{
    return m_filePath;
}

void EncoderPrivate::setVideoSize(const QSize &size)
{
    if (m_videoSize != size) {
        m_videoSize = size;
    }
}

QSize EncoderPrivate::videoSize() const
{
    return m_videoSize;
}

void EncoderPrivate::setFixedFrameRate(int frameRate)
{
    if (m_fixedFrameRate != frameRate) {
        m_fixedFrameRate = frameRate;
    }
}

int EncoderPrivate::fixedFrameRate() const
{
    return m_fixedFrameRate;
}

bool EncoderPrivate::isFixedFrameRate() const
{
    return fixedFrameRate() != -1;
}

void EncoderPrivate::setEncodingMode(Encoder::EncodingMode mode)
{
    if (m_encodingMode != mode) {
        m_encodingMode = mode;
    }
}

Encoder::EncodingMode EncoderPrivate::encodingMode() const
{
    return m_encodingMode;
}

void EncoderPrivate::setOutputPixelFormat(EncoderGlobal::PixelFormat format)
{
    if (m_outputPixelFormat != format) {
        m_outputPixelFormat = format;
    }
}

EncoderGlobal::PixelFormat EncoderPrivate::outputPixelFormat() const
{
    return m_outputPixelFormat;
}

void EncoderPrivate::setVideoCodec(EncoderGlobal::VideoCodec codec)
{
    if (m_videoCodecName != codec) {
        m_videoCodecName = codec;
    }
}

EncoderGlobal::VideoCodec EncoderPrivate::videoCodec() const
{
    return m_videoCodecName;
}

void EncoderPrivate::setAudioCodec(EncoderGlobal::AudioCodec codec)
{
    if (m_audioCodecName != codec) {
        m_audioCodecName = codec;
    }
}

EncoderGlobal::AudioCodec EncoderPrivate::audioCodec() const
{
    return m_audioCodecName;
}

void EncoderPrivate::setVideoCodecSettings(const VideoCodecSettings &settings)
{
    m_videoSettings = settings;
}

VideoCodecSettings EncoderPrivate::videoCodecSettings() const
{
    return m_videoSettings;
}

void EncoderPrivate::setAudioCodecSettings(const AudioCodecSettings &settings)
{
    m_audioSettings = settings;
}

AudioCodecSettings EncoderPrivate::audioCodecSettings() const
{
    return m_audioSettings;
}

int EncoderPrivate::encodedFrameCount() const
{
    QMutexLocker locker(&m_encodedFrameCountMutex);
    return m_encodedFrameCount;
}

int EncoderPrivate::encodedAudioDataSize() const
{
    QMutexLocker locker(&m_encodedAudioDataSizeMutex);
    return m_encodedAudioDataSize;
}

void EncoderPrivate::start()
{
    //check input data
    if (!(videoSize().width() > 0 && videoSize().height() > 0)) {
        q_ptr->setError(Encoder::InvalidVideoSizeError, tr("Video size is invalid. Width and height must be greater than 0."));
        return;
    }

    int width = videoSize().width();
    int height = videoSize().height();
    if ((width % 4 != 0 && width % 8 != 0 && width % 16 != 0)
            || (height % 4 != 0 && height % 8 != 0 && height % 16 != 0)) {
        q_ptr->setError(Encoder::InvalidVideoSizeError, tr("Video size dimensions must be multiple of 4,8 or 16."));
        return;
    }

    if (filePath().isEmpty()) {
        q_ptr->setError(Encoder::InvalidFilePathError, tr("File path is not set."));
        return;
    }

    //init ffmpeg stuff
    avcodec_register_all();
    av_register_all();

    // TODO: !
    m_outputFormat = av_guess_format(NULL, filePath().toUtf8().constData(), NULL);
    if (!m_outputFormat) {
        q_ptr->setError(Encoder::InvalidOutputFormatError, tr("Unable to get an output format by passed filename."));
        return;
    }

    m_formatContext = avformat_alloc_context();

    m_formatContext->oformat = m_outputFormat;

    if (encodingMode() == Encoder::VideoMode
            || encodingMode() == Encoder::VideoAudioMode) {
        if (!createVideoStream())
            return;

        if (!openVideoStream())
            return;
    }

    if (encodingMode() == Encoder::AudioMode
            || encodingMode() == Encoder::VideoAudioMode) {
        if (!createAudioStream())
            return;

        if (!openAudioStream())
            return;
    }

    if (avio_open(&m_formatContext->pb, filePath().toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
        q_ptr->setError(Encoder::FileOpenError, QString(tr("Unable to open: %1")).arg(filePath()));
        return;
    }

    int rs = avformat_write_header(m_formatContext, 0);
    if (rs ) {
        q_ptr->setError(Encoder::WriteHeaderError, QString(tr("Unable to wreite header formar")));
        return;
    }

    Q_EMIT q_ptr->started();
    q_ptr->setState(Encoder::ActiveState);
}

void EncoderPrivate::stop()
{
    Q_EMIT q_ptr->stopped();
    q_ptr->setState(Encoder::StoppedState);

    av_write_trailer(m_formatContext);

    cleanup();
}

void EncoderPrivate::encodeVideoFrame(const QImage &frame, int pts)
{
    if (pts) {
        if (convertImage(frame)) {
            int rs = avcodec_send_frame(m_videoCodecContext, m_videoPicture);
            if (rs == 0) {

                AVPacket pkt;
                av_init_packet(&pkt);
                avcodec_receive_packet(m_videoCodecContext, &pkt);
                av_write_frame(m_formatContext, &pkt);

                QMutexLocker locker(&m_encodedFrameCountMutex);
                ++m_encodedFrameCount;
            }
        }
    }
}

void EncoderPrivate::encodeAudioData(const QByteArray &data)
{
    QByteArray bytesBuffer = data;
    int bytesReady = data.size();

    QByteArray samples;

    do
    {
        samples.clear();

        // TODO: rewrite
        if (m_audioInputBuffer.size() + bytesReady == m_audioSampleSize) {
            samples.append(m_audioInputBuffer + bytesBuffer);
            m_audioInputBuffer.clear();
        } else if (m_audioInputBuffer.size() + bytesReady > m_audioSampleSize) {
            m_audioInputBuffer.append(bytesBuffer);
            samples.append(m_audioInputBuffer.left(m_audioSampleSize));
            m_audioInputBuffer.remove(0, m_audioSampleSize);
        } else if (m_audioInputBuffer.size() + bytesReady < m_audioSampleSize) {
            m_audioInputBuffer.append(bytesBuffer);
            return;
        }

        bytesReady = 0;
        bytesBuffer.clear();

        /* frame containing input raw audio */
         AVFrame *frame = av_frame_alloc();
        if (!frame) {
                exit(1);
        }

        /* setup the data pointers in the AVFrame */
        int ret = avcodec_fill_audio_frame(frame, m_audioCodecContext->channels, m_audioCodecContext->sample_fmt,
                                           (const uint8_t*)samples.data(), samples.size(), 0);
        if (ret < 0) {
            fprintf(stderr, "Could not setup audio frame\n");
            exit(1);
        }

        ret = avcodec_send_frame(m_audioCodecContext, frame);

        if (ret == 0) {

            AVPacket pkt;
            avcodec_receive_packet(m_audioCodecContext, &pkt);
            av_write_frame(m_formatContext, &pkt);

            QMutexLocker locker(&m_encodedAudioDataSizeMutex);
            m_encodedAudioDataSize += m_audioSampleSize;
        }
    } while (m_audioInputBuffer.size() > m_audioSampleSize);
}

void EncoderPrivate::onError()
{
    q_ptr->setState(Encoder::StoppedState);

    cleanup();
}

void EncoderPrivate::initData()
{
    m_outputPixelFormat = AVPixelFormat::AV_PIX_FMT_NONE;
    m_videoCodecName = EncoderGlobal::DEFAULT_VIDEO_CODEC;
    m_audioCodecName = EncoderGlobal::DEFAULT_AUDIO_CODEC;

    m_fixedFrameRate = -1;
    m_encodingMode = Encoder::VideoAudioMode;
}


void EncoderPrivate::initFfmpegStuff()
{
    m_pts = 0;
    m_encodedFrameCount = 0;
    m_encodedAudioDataSize = 0;

    //video stuff
    m_outputFormat = NULL;
    m_formatContext = NULL;
    m_videoStream = NULL;
    m_videoCodecContext = NULL;
    m_videoCodecParams = NULL;
    m_videoCodec = NULL;

    //audio stuff
    m_audioStream = NULL;
    m_audioCodecContext = NULL;
    m_audioCodecParams = NULL;
    m_audioCodec = NULL;
    m_audioSampleSize = 0;

    m_imageConvertContext = NULL;
    m_videoPicture = NULL;
}

void EncoderPrivate::cleanup()
{
    //close codecs
    if (m_videoCodecContext != NULL) {
        avcodec_close(m_videoCodecContext);
        av_free(m_videoCodecContext);
    }

    if (m_audioCodecContext != NULL) {
        avcodec_close(m_audioCodecContext);
        av_free(m_audioCodecContext);
    }

    if (m_videoStream != NULL)
        av_free(m_videoStream);

    if (m_audioStream != NULL)
        av_free(m_audioStream);

    //remove subsidiary objects
    if (m_imageConvertContext != NULL)
        sws_freeContext(m_imageConvertContext);

    if (m_formatContext != NULL) {
        avio_close(m_formatContext->pb);
        av_free(m_formatContext);
    }

    initFfmpegStuff();
}

bool EncoderPrivate::createVideoStream()
{
    // TODO: pass AVCodec
    m_videoStream = avformat_new_stream(m_formatContext,0);
    if(!m_videoStream ) {
        q_ptr->setError(Encoder::InvalidVideoStreamError, tr("Unable to add video stream."));
        return false;
    }

    //set up codec
    m_videoCodecParams = m_videoStream->codecpar;
    m_videoCodecParams->codec_id = (videoCodec() == EncoderGlobal::DEFAULT_VIDEO_CODEC) ? m_outputFormat->video_codec : static_cast<AVCodecID>(videoCodec());
    m_videoCodecParams->codec_type = AVMEDIA_TYPE_VIDEO;
    m_videoCodecParams->width = videoSize().width();
    m_videoCodecParams->height = videoSize().height();
    // m_videoCodecParams->pix_fmt = outputPixelFormat();
    // m_videoCodecParams->time_base.den = fixedFrameRate() != -1 ? fixedFrameRate() : 1000;
    // m_videoCodecParams->time_base.num = 1;

    applyVideoCodecSettings();

    return true;
}

bool EncoderPrivate::createAudioStream()
{
    m_audioStream = avformat_new_stream(m_formatContext, NULL);
    if(!m_audioStream ) {
        q_ptr->setError(Encoder::InvalidAudioStreamError, tr("Unable to add audio stream."));
        return false;
    }

    //set up codec
    m_audioCodecParams = m_audioStream->codecpar;
    m_audioCodecParams->codec_id = (audioCodec() == EncoderGlobal::DEFAULT_AUDIO_CODEC) ? m_outputFormat->audio_codec : static_cast<AVCodecID>(audioCodec());
    m_audioCodecParams->codec_type = AVMEDIA_TYPE_AUDIO;

    applyAudioCodecSettings();

    return true;
}

bool EncoderPrivate::openVideoStream()
{
    m_videoCodec = avcodec_find_encoder(m_videoCodecParams->codec_id);
    if (!m_videoCodec) {
        q_ptr->setError(Encoder::VideoEncoderNotFoundError, tr("Unable to find video encoder by codec id."));
        return false;
    }

    m_videoCodecContext = avcodec_alloc_context3(m_videoCodec);
    if (!m_videoCodecContext) {
        q_ptr->setError(Encoder::InvalidAudioCodecError, tr("Unable to alloc video codec context."));
        return false;
    }

    // open the codec
    if (avcodec_open2(m_videoCodecContext, m_videoCodec, NULL) < 0) {
        q_ptr->setError(Encoder::InvalidVideoCodecError, tr("Unable to open video codec."));
        return false;
    }

    return true;
}


bool EncoderPrivate::openAudioStream()
{
    m_audioCodec = avcodec_find_encoder(m_audioCodecParams->codec_id);
    if (!m_audioCodec) {
        q_ptr->setError(Encoder::AudioEncoderNotFoundError, tr("Unable to find audio encoder by codec id."));
        return false;
    }

    m_audioCodecContext = avcodec_alloc_context3(m_audioCodec);
    if (!m_audioCodecContext) {
        q_ptr->setError(Encoder::InvalidAudioCodecError, tr("Unable to alloc audio codec context."));
        return false;
    }

    // open the codec
    if (avcodec_open2(m_audioCodecContext, m_audioCodec, NULL) < 0) {
        q_ptr->setError(Encoder::InvalidAudioCodecError, tr("Unable to open audio codec."));
        return false;
    }

    m_audioSampleSize = 2 * m_audioCodecContext->frame_size * m_audioCodecContext->channels;

    return true;
}

bool EncoderPrivate::convertImage(const QImage &image)
{
    EncoderGlobal::PixelFormat inputFormat = convertImagePixelFormat(image.format());
    if (inputFormat == AVPixelFormat::AV_PIX_FMT_NONE) {
        q_ptr->setError(Encoder::InvalidInputPixelFormat, tr("Could not convert input pixel format to the ffmpeg's format."));
        return false;
    }

    m_imageConvertContext = sws_getCachedContext(m_imageConvertContext, image.width(), image.height(),
                                                 (EncoderGlobal::PixelFormat)inputFormat, image.width(), image.height(), m_videoCodecContext->pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);

    if (m_imageConvertContext == NULL) {
        q_ptr->setError(Encoder::InvalidConversionContext, tr("Could not initialize conversion context."));
        return false;
    }

    uint8_t *srcplanes[3];
    srcplanes[0]=(uint8_t*)image.bits();
    srcplanes[1]=0;
    srcplanes[2]=0;

    int srcstride[3];
    srcstride[0]=image.bytesPerLine();
    srcstride[1]=0;
    srcstride[2]=0;

    sws_scale(m_imageConvertContext, srcplanes, srcstride,0, image.height(), m_videoPicture->data, m_videoPicture->linesize);

    return true;
}

EncoderGlobal::PixelFormat EncoderPrivate::convertImagePixelFormat(QImage::Format format) const
{
    EncoderGlobal::PixelFormat newFormat;

    switch (format) {
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGB32:
        newFormat = AVPixelFormat::AV_PIX_FMT_BGRA;
        break;

    case QImage::Format_RGB16:
        newFormat = AVPixelFormat::AV_PIX_FMT_RGB565LE;
        break;

    case QImage::Format_RGB888:
        newFormat = AVPixelFormat::AV_PIX_FMT_RGB24;
        break;

    case QImage::Format_Mono:
    case QImage::Format_MonoLSB:
        newFormat = AVPixelFormat::AV_PIX_FMT_MONOWHITE;
        break;

    default:
        newFormat = AVPixelFormat::AV_PIX_FMT_NONE;
        break;
    }

    return newFormat;
}

void EncoderPrivate::applyVideoCodecSettings()
{
    setVideoCodecOption<int64_t, int>(&AVCodecContext::bit_rate, &VideoCodecSettings::bitrate);
    setVideoCodecOption<int, int>(&AVCodecContext::gop_size, &VideoCodecSettings::gopSize);
    setVideoCodecOption<int, int>(&AVCodecContext::qmin, &VideoCodecSettings::minimumQuantizer);
    setVideoCodecOption<int, int>(&AVCodecContext::qmax, &VideoCodecSettings::minimumQuantizer);
    setVideoCodecOption<int, int>(&AVCodecContext::max_qdiff, &VideoCodecSettings::maximumQuantizerDifference);
//    setVideoCodecOption<int, EncoderGlobal::CoderType>(&AVCodecContext::coder_type, &VideoCodecSettings::coderType);
    setVideoCodecOption<int, int>(&AVCodecContext::me_cmp, &VideoCodecSettings::motionEstimationComparison);
//    setVideoCodecOption<int, EncoderGlobal::Partitions>(&AVCodecContext::partitions, &VideoCodecSettings::partitions);
//    setVideoCodecOption<int, EncoderGlobal::MotionEstimationAlgorithm>(&AVCodecContext::me_method, &VideoCodecSettings::motionEstimationMethod);
    setVideoCodecOption<int, int>(&AVCodecContext::me_subpel_quality, &VideoCodecSettings::subpixelMotionEstimationQuality);
    setVideoCodecOption<int, int>(&AVCodecContext::me_range, &VideoCodecSettings::motionEstimationRange);
    setVideoCodecOption<int, int>(&AVCodecContext::keyint_min, &VideoCodecSettings::minimumKeyframeInterval);
//    setVideoCodecOption<int, int>(&AVCodecContext::scenechange_threshold, &VideoCodecSettings::sceneChangeThreshold);
    setVideoCodecOption<float, float>(&AVCodecContext::i_quant_factor, &VideoCodecSettings::iQuantFactor);
//    setVideoCodecOption<int, int>(&AVCodecContext::b_frame_strategy, &VideoCodecSettings::bFrameStrategy);
    setVideoCodecOption<float, float>(&AVCodecContext::qcompress, &VideoCodecSettings::quantizerCurveCompressionFactor);
    setVideoCodecOption<int, int>(&AVCodecContext::max_b_frames, &VideoCodecSettings::maximumBFrames);
    setVideoCodecOption<int, int>(&AVCodecContext::refs, &VideoCodecSettings::referenceFrameCount);
//    setVideoCodecOption<int, EncoderGlobal::MotionVectorPredictionMode>(&AVCodecContext::directpred, &VideoCodecSettings::directMvPredictionMode);
    setVideoCodecOption<int, int>(&AVCodecContext::trellis, &VideoCodecSettings::trellis);
//    setVideoCodecOption<int, EncoderGlobal::WeightedPredictionMethod>(&AVCodecContext::weighted_p_pred, &VideoCodecSettings::pFramePredictionAnalysisMethod);
//    setVideoCodecOption<int, int>(&AVCodecContext::rc_lookahead, &VideoCodecSettings::rcLookahead);
    setVideoCodecOption<int, EncoderGlobal::Flags>(&AVCodecContext::flags, &VideoCodecSettings::flags);
    setVideoCodecOption<int, EncoderGlobal::Flags2>(&AVCodecContext::flags2, &VideoCodecSettings::flags2);
}

template <class T1, class T2>
void EncoderPrivate::setVideoCodecOption(T1 AVCodecContext::*option, T2 (VideoCodecSettings::*f)() const)
{
    T2 value = (m_videoSettings.*f)();
    if ((int)value != -1) {
        m_videoCodecContext->*option = (m_videoSettings.*f)();
    }
}

void EncoderPrivate::applyAudioCodecSettings()
{
    if (m_audioSettings.sampleFormat() != -1) {
        m_audioCodecContext->sample_fmt = static_cast<AVSampleFormat>(m_audioSettings.sampleFormat());
    }

    setAudioCodecOption<int64_t, int>(&AVCodecContext::bit_rate, &AudioCodecSettings::bitrate);
    setAudioCodecOption<int, int>(&AVCodecContext::sample_rate, &AudioCodecSettings::sampleRate);
    setAudioCodecOption<int, int>(&AVCodecContext::channels, &AudioCodecSettings::channelCount);
}

template <class T1, class T2>
void EncoderPrivate::setAudioCodecOption(T1 AVCodecContext::*option, T2 (AudioCodecSettings::*f)() const)
{
    T2 value = (m_audioSettings.*f)();
    if (value != -1) {
        m_audioCodecContext->*option = (m_audioSettings.*f)();
    }
}


//---------------------------------------------------------------------------------
// Encoder implementation
//---------------------------------------------------------------------------------


Encoder::Encoder(QObject *parent) :
    QObject(parent)
  , d_ptr(new EncoderPrivate(this))
  , m_encoderThread(new QThread(this))
  , m_state(Encoder::StoppedState)
  , m_error(Encoder::NoError)
{
    qRegisterMetaType<Encoder::Error>("Encoder::Error");
    qRegisterMetaType<Encoder::State>("Encoder::State");

    d_ptr->moveToThread(m_encoderThread);
    m_encoderThread->start();
}

Encoder::~Encoder()
{
    m_encoderThread->terminate();
    delete d_ptr;
}

void Encoder::setFilePath(const QString &filePath)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setFilePath(filePath);
}

QString Encoder::filePath() const
{
    return d_ptr->filePath();
}

void Encoder::setVideoSize(const QSize &size)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setVideoSize(size);
}

QSize Encoder::videoSize() const
{
    return d_ptr->videoSize();
}

void Encoder::setFixedFrameRate(int frameRate)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setFixedFrameRate(frameRate);
}

int Encoder::fixedFrameRate() const
{
    return d_ptr->fixedFrameRate();
}

void Encoder::setEncodingMode(Encoder::EncodingMode mode)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setEncodingMode(mode);
}

Encoder::EncodingMode Encoder::encodingMode() const
{
    return d_ptr->encodingMode();
}

void Encoder::setOutputPixelFormat(EncoderGlobal::PixelFormat format)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setOutputPixelFormat(format);
}

EncoderGlobal::PixelFormat Encoder::outputPixelFormat() const
{
    return d_ptr->outputPixelFormat();
}

void Encoder::setVideoCodec(EncoderGlobal::VideoCodec codec)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setVideoCodec(codec);
}

EncoderGlobal::VideoCodec Encoder::videoCodec() const
{
    return d_ptr->videoCodec();
}

void Encoder::setAudioCodec(EncoderGlobal::AudioCodec codec)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setAudioCodec(codec);
}

EncoderGlobal::AudioCodec Encoder::audioCodec() const
{
    return d_ptr->audioCodec();
}

void Encoder::setVideoCodecSettings(const VideoCodecSettings &settings)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setVideoCodecSettings(settings);
}

VideoCodecSettings Encoder::videoCodecSettings() const
{
    return d_ptr->videoCodecSettings();
}

void Encoder::setAudioCodecSettings(const AudioCodecSettings &settings)
{
    if (state() != Encoder::ActiveState)
        d_ptr->setAudioCodecSettings(settings);
}

AudioCodecSettings Encoder::audioCodecSettings() const
{
    return d_ptr->audioCodecSettings();
}

int Encoder::encodedFrameCount() const
{
    return d_ptr->encodedFrameCount();
}

int Encoder::encodedAudioDataSize() const
{
    return d_ptr->encodedAudioDataSize();
}

Encoder::State Encoder::state() const
{
    return m_state;
}

Encoder::Error Encoder::error() const
{
    return m_error;
}

void Encoder::setState(Encoder::State state)
{
    if (m_state != state) {
        m_state = state;
        Q_EMIT stateChanged(state);
    }
}

QString Encoder::errorString() const
{
    return m_errorString;
}

void Encoder::start()
{
    if (state() != Encoder::ActiveState)
        QMetaObject::invokeMethod(d_ptr, "start", Qt::QueuedConnection);
}

void Encoder::stop()
{
    if (state() == Encoder::ActiveState)
        QMetaObject::invokeMethod(d_ptr, "stop", Qt::QueuedConnection);
}

void Encoder::encodeVideoFrame(const QImage &frame, int pts)
{
    if (state() == Encoder::ActiveState
            && (encodingMode() == Encoder::VideoMode || encodingMode() == Encoder::VideoAudioMode)) {
        QMetaObject::invokeMethod(d_ptr, "encodeVideoFrame", Qt::QueuedConnection,
                                  Q_ARG(QImage, frame),
                                  Q_ARG(int, pts));
    }
}

void Encoder::encodeAudioData(const QByteArray &data)
{
    if (state() == Encoder::ActiveState
            && (encodingMode() == Encoder::AudioMode || encodingMode() == Encoder::VideoAudioMode)) {
        QMetaObject::invokeMethod(d_ptr, "encodeAudioData", Qt::QueuedConnection,
                                  Q_ARG(QByteArray, data));
    }
}

void Encoder::setError(Encoder::Error errorCode, const QString &errorString)
{
    m_error = errorCode;
    m_errorString = errorString;

    Q_EMIT error(errorCode);
}

#include "encoder.moc"
