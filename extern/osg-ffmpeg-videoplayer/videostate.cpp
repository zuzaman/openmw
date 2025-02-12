#include "videostate.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

#include <osg/Texture2D>
#include <utility>

#if defined(_MSC_VER)
    #pragma warning (push)
    #pragma warning (disable : 4244)
#endif

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/time.h>
}

#if defined(_MSC_VER)
    #pragma warning (pop)
#endif

static const char* flushString = "FLUSH";
struct FlushPacket : AVPacket
{
    FlushPacket()
        : AVPacket()
    {
        data = ( (uint8_t*)flushString);
    }
};

static FlushPacket flush_pkt;

#include "videoplayer.hpp"
#include "audiodecoder.hpp"
#include "audiofactory.hpp"

namespace
{
    const int MAX_AUDIOQ_SIZE = (5 * 16 * 1024);
    const int MAX_VIDEOQ_SIZE = (5 * 256 * 1024);

    struct AVPacketUnref
    {
        void operator()(AVPacket* packet) const
        {
            av_packet_unref(packet);
        }
    };

    struct AVFrameFree
    {
        void operator()(AVFrame* frame) const
        {
            av_frame_free(&frame);
        }
    };
}

namespace Video
{

struct PacketListFree
{
    void operator()(Video::PacketList* list) const
    {
        av_packet_free(&list->pkt);
        av_free(&list);
    }
};

VideoState::VideoState()
    : mAudioFactory(nullptr)
    , format_ctx(nullptr)
    , video_ctx(nullptr)
    , audio_ctx(nullptr)
    , av_sync_type(AV_SYNC_DEFAULT)
    , audio_st(nullptr)
    , video_st(nullptr), frame_last_pts(0.0)
    , video_clock(0.0), sws_context(nullptr)
    , sws_context_w(0), sws_context_h(0)
    , pictq_size(0), pictq_rindex(0), pictq_windex(0)
    , mSeekRequested(false)
    , mSeekPos(0)
    , mVideoEnded(false)
    , mPaused(false)
    , mQuit(false)
{
    mFlushPktData = flush_pkt.data;

// This is not needed any more above FFMpeg version 4.0
#if LIBAVCODEC_VERSION_INT < 3805796
    av_register_all();
#endif
}

VideoState::~VideoState()
{
    deinit();
}

void VideoState::setAudioFactory(MovieAudioFactory *factory)
{
    mAudioFactory = factory;
}


void PacketQueue::put(AVPacket *pkt)
{
    std::unique_ptr<PacketList, PacketListFree> pkt1(static_cast<PacketList*>(av_malloc(sizeof(PacketList))));
    if(!pkt1) throw std::bad_alloc();


    if(pkt == &flush_pkt)
        pkt1->pkt = pkt;
    else
    {
        pkt1->pkt = av_packet_alloc();
        av_packet_move_ref(pkt1->pkt, pkt);
    }

    pkt1->next = nullptr;

    std::lock_guard<std::mutex> lock(this->mutex);
    PacketList* ptr = pkt1.release();
    if(!last_pkt)
        this->first_pkt = ptr;
    else
        this->last_pkt->next = ptr;
    this->last_pkt = ptr;
    this->nb_packets++;
    this->size += ptr->pkt->size;
    this->cond.notify_one();
}

int PacketQueue::get(AVPacket *pkt, VideoState *is)
{
    std::unique_lock<std::mutex> lock(this->mutex);
    while(!is->mQuit)
    {
        PacketList *pkt1 = this->first_pkt;
        if(pkt1)
        {
            this->first_pkt = pkt1->next;
            if(!this->first_pkt)
                this->last_pkt = nullptr;
            this->nb_packets--;
            this->size -= pkt1->pkt->size;

            av_packet_unref(pkt);
            av_packet_move_ref(pkt, pkt1->pkt);
            av_free(pkt1);

            return 1;
        }

        if(this->flushing)
            break;
        this->cond.wait(lock);
    }

    return -1;
}

void PacketQueue::flush()
{
    this->flushing = true;
    this->cond.notify_one();
}

void PacketQueue::clear()
{
    PacketList *pkt, *pkt1;

    std::lock_guard<std::mutex> lock(this->mutex);
    for(pkt = this->first_pkt; pkt != nullptr; pkt = pkt1)
    {
        pkt1 = pkt->next;
        if (pkt->pkt->data != flush_pkt.data)
            av_packet_unref(pkt->pkt);
        av_freep(&pkt);
    }
    this->last_pkt = nullptr;
    this->first_pkt = nullptr;
    this->nb_packets = 0;
    this->size = 0;
}

int VideoPicture::set_dimensions(int w, int h) {
  if (this->rgbaFrame != nullptr && this->rgbaFrame->width == w &&
      this->rgbaFrame->height == h) {
    return 0;
  }

  std::unique_ptr<AVFrame, VideoPicture::AVFrameDeleter> frame{
      av_frame_alloc()};
  if (frame == nullptr) {
    std::cerr << "av_frame_alloc failed" << std::endl;
    return -1;
  }

  constexpr AVPixelFormat kPixFmt = AV_PIX_FMT_RGBA;
  frame->format = kPixFmt;
  frame->width = w;
  frame->height = h;
  if (av_image_alloc(frame->data, frame->linesize, frame->width, frame->height,
                     kPixFmt, 1) < 0) {
    std::cerr << "av_image_alloc failed" << std::endl;
    return -1;
  }

  this->rgbaFrame = std::move(frame);
  return 0;
}

void VideoPicture::AVFrameDeleter::operator()(AVFrame* frame) const
{
    av_freep(frame->data);
    av_frame_free(&frame);
}

int VideoState::istream_read(void *user_data, uint8_t *buf, int buf_size)
{
    try
    {
        std::istream& stream = *static_cast<VideoState*>(user_data)->stream;
        stream.clear();
        stream.read((char*)buf, buf_size);
        return stream.gcount();
    }
    catch (std::exception& )
    {
        return 0;
    }
}

int VideoState::istream_write(void *, uint8_t *, int)
{
    throw std::runtime_error("can't write to read-only stream");
}

int64_t VideoState::istream_seek(void *user_data, int64_t offset, int whence)
{
    std::istream& stream = *static_cast<VideoState*>(user_data)->stream;

    whence &= ~AVSEEK_FORCE;

    stream.clear();

    if(whence == AVSEEK_SIZE)
    {
        size_t prev = stream.tellg();
        stream.seekg(0, std::ios_base::end);
        size_t size = stream.tellg();
        stream.seekg(prev, std::ios_base::beg);
        return size;
    }

    if(whence == SEEK_SET)
        stream.seekg(offset, std::ios_base::beg);
    else if(whence == SEEK_CUR)
        stream.seekg(offset, std::ios_base::cur);
    else if(whence == SEEK_END)
        stream.seekg(offset, std::ios_base::end);
    else
        return -1;

    return stream.tellg();
}

void VideoState::video_display(VideoPicture *vp)
{
    if(this->video_ctx->width != 0 && this->video_ctx->height != 0)
    {
        if (!mTexture.get())
        {
            mTexture = new osg::Texture2D;
            mTexture->setDataVariance(osg::Object::DYNAMIC);
            mTexture->setResizeNonPowerOfTwoHint(false);
            mTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            mTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        }

        osg::ref_ptr<osg::Image> image = new osg::Image;

        image->setImage(this->video_ctx->width, this->video_ctx->height,
                        1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, vp->rgbaFrame->data[0], osg::Image::NO_DELETE);

        mTexture->setImage(image);
    }
}

void VideoState::video_refresh()
{
    std::lock_guard<std::mutex> lock(this->pictq_mutex);
    if(this->pictq_size == 0)
        return;

    if (this->av_sync_type == AV_SYNC_VIDEO_MASTER)
    {
        VideoPicture* vp = &this->pictq[this->pictq_rindex];
        this->video_display(vp);

        this->pictq_rindex = (this->pictq_rindex+1) % this->pictq.size();
        this->frame_last_pts = vp->pts;
        this->pictq_size--;
        this->pictq_cond.notify_one();
    }
    else
    {
        const float threshold = 0.03f;
        if (this->pictq[pictq_rindex].pts > this->get_master_clock() + threshold)
            return; // not ready yet to show this picture

        // TODO: the conversion to RGBA is done in the decoding thread, so if a picture is skipped here, then it was
        // unnecessarily converted. But we may want to replace the conversion by a pixel shader anyway (see comment in queue_picture)
        int i=0;
        for (; i<this->pictq_size-1; ++i)
        {
            if (this->pictq[this->pictq_rindex].pts + threshold <= this->get_master_clock())
                this->pictq_rindex = (this->pictq_rindex+1) % this->pictq.size(); // not enough time to show this picture
            else
                break;
        }

        assert (this->pictq_rindex < this->pictq.size());
        VideoPicture* vp = &this->pictq[this->pictq_rindex];

        this->video_display(vp);

        this->frame_last_pts = vp->pts;

        this->pictq_size -= i;
        // update queue for next picture
        this->pictq_size--;
        this->pictq_rindex = (this->pictq_rindex+1) % this->pictq.size();
        this->pictq_cond.notify_one();
    }
}


int VideoState::queue_picture(const AVFrame &pFrame, double pts)
{
    VideoPicture *vp;

    /* wait until we have a new pic */
    {
        std::unique_lock<std::mutex> lock(this->pictq_mutex);
        while(this->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !this->mQuit)
            this->pictq_cond.wait_for(lock, std::chrono::milliseconds(1));
    }
    if(this->mQuit)
        return -1;

    std::lock_guard<std::mutex> lock(this->pictq_mutex);

    // windex is set to 0 initially
    vp = &this->pictq[this->pictq_windex];

    // Convert the image into RGBA format
    // TODO: we could do this in a pixel shader instead, if the source format
    // matches a commonly used format (ie YUV420P)
    const int w = pFrame.width;
    const int h = pFrame.height;
    if(this->sws_context == nullptr || this->sws_context_w != w || this->sws_context_h != h)
    {
        if (this->sws_context != nullptr)
            sws_freeContext(this->sws_context);
        this->sws_context = sws_getContext(w, h, this->video_ctx->pix_fmt,
                                           w, h, AV_PIX_FMT_RGBA, SWS_BICUBIC,
                                           nullptr, nullptr, nullptr);
        if(this->sws_context == nullptr)
            throw std::runtime_error("Cannot initialize the conversion context!\n");
        this->sws_context_w = w;
        this->sws_context_h = h;
    }

    vp->pts = pts;
    if (vp->set_dimensions(w, h) < 0)
        return -1;

    sws_scale(this->sws_context, pFrame.data, pFrame.linesize,
              0, this->video_ctx->height, vp->rgbaFrame->data, vp->rgbaFrame->linesize);

    // now we inform our display thread that we have a pic ready
    this->pictq_windex = (this->pictq_windex+1) % this->pictq.size();
    this->pictq_size++;

    return 0;
}

double VideoState::synchronize_video(const AVFrame &src_frame, double pts)
{
    double frame_delay;

    /* if we have pts, set video clock to it */
    if(pts != 0)
        this->video_clock = pts;
    else
        pts = this->video_clock;

    /* update the video clock */
    frame_delay = av_q2d(this->video_ctx->pkt_timebase);

    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src_frame.repeat_pict * (frame_delay * 0.5);
    this->video_clock += frame_delay;

    return pts;
}

class VideoThread
{
public:
    explicit VideoThread(VideoState* self)
        : mVideoState(self)
        , mThread([this]
        {
            try
            {
                run();
            }
            catch(std::exception& e)
            {
                std::cerr << "An error occurred playing the video: " << e.what () << std::endl;
            }
        })
    {
    }

    ~VideoThread()
    {
        mThread.join();
    }

    void run()
    {
        VideoState* self = mVideoState;
        AVPacket* packetData = av_packet_alloc();
        std::unique_ptr<AVPacket, AVPacketUnref> packet(packetData);
        std::unique_ptr<AVFrame, AVFrameFree> pFrame{av_frame_alloc()};

        while(self->videoq.get(packet.get(), self) >= 0)
        {
            if(packet->data == flush_pkt.data)
            {
                avcodec_flush_buffers(self->video_ctx);

                self->pictq_mutex.lock();
                self->pictq_size = 0;
                self->pictq_rindex = 0;
                self->pictq_windex = 0;
                self->pictq_mutex.unlock();

                self->frame_last_pts = packet->pts * av_q2d((*self->video_st)->time_base);
                continue;
            }

            // Decode video frame
            int ret = avcodec_send_packet(self->video_ctx, packet.get());
            // EAGAIN is not expected
            if (ret < 0)
                throw std::runtime_error("Error decoding video frame");

            while (!ret)
            {
                ret = avcodec_receive_frame(self->video_ctx, pFrame.get());
                if (!ret)
                {
                    double pts = pFrame->best_effort_timestamp;
                    pts *= av_q2d((*self->video_st)->time_base);

                    pts = self->synchronize_video(*pFrame, pts);

                    if(self->queue_picture(*pFrame, pts) < 0)
                        break;
                }
            }
        }
    }

private:
    VideoState* mVideoState;
    std::thread mThread;
};

class ParseThread
{
public:
    explicit ParseThread(VideoState* self)
        : mVideoState(self)
        , mThread([this] { run(); })
    {
    }

    ~ParseThread()
    {
        mThread.join();
    }

    void run()
    {
        VideoState* self = mVideoState;

        AVFormatContext *pFormatCtx = self->format_ctx;
        AVPacket* packetData = av_packet_alloc();
        std::unique_ptr<AVPacket, AVPacketUnref> packet(packetData);

        try
        {
            if(!self->video_st && !self->audio_st)
                throw std::runtime_error("No streams to decode");

            // main decode loop
            while(!self->mQuit)
            {
                if(self->mSeekRequested)
                {
                    uint64_t seek_target = self->mSeekPos;
                    int streamIndex = -1;

                    int videoStreamIndex = -1;
                    int audioStreamIndex = -1;
                    if (self->video_st)
                        videoStreamIndex = self->video_st - self->format_ctx->streams;
                    if (self->audio_st)
                        audioStreamIndex = self->audio_st - self->format_ctx->streams;

                    if(videoStreamIndex >= 0)
                        streamIndex = videoStreamIndex;
                    else if(audioStreamIndex >= 0)
                        streamIndex = audioStreamIndex;

                    uint64_t timestamp = seek_target;

                    // QtCreator's highlighter doesn't like AV_TIME_BASE_Q's {} initializer for some reason
                    AVRational avTimeBaseQ = AVRational(); // = AV_TIME_BASE_Q;
                    avTimeBaseQ.num = 1;
                    avTimeBaseQ.den = AV_TIME_BASE;

                    if(streamIndex >= 0)
                        timestamp = av_rescale_q(seek_target, avTimeBaseQ, self->format_ctx->streams[streamIndex]->time_base);

                    // AVSEEK_FLAG_BACKWARD appears to be needed, otherwise ffmpeg may seek to a keyframe *after* the given time
                    // we want to seek to any keyframe *before* the given time, so we can continue decoding as normal from there on
                    if(av_seek_frame(self->format_ctx, streamIndex, timestamp, AVSEEK_FLAG_BACKWARD) < 0)
                    {
// In the FFMpeg 4.0 a "filename" field was replaced by "url"
#if LIBAVCODEC_VERSION_INT < 3805796
                        std::cerr << "Error seeking " << self->format_ctx->filename << std::endl;
#else
                        std::cerr << "Error seeking " << self->format_ctx->url << std::endl;
#endif
                    }
                    else
                    {
                        // Clear the packet queues and put a special packet with the new clock time
                        if(audioStreamIndex >= 0)
                        {
                            self->audioq.clear();
                            flush_pkt.pts = av_rescale_q(seek_target, avTimeBaseQ,
                                self->format_ctx->streams[audioStreamIndex]->time_base);
                            self->audioq.put(&flush_pkt);
                        }
                        if(videoStreamIndex >= 0)
                        {
                            self->videoq.clear();
                            flush_pkt.pts = av_rescale_q(seek_target, avTimeBaseQ,
                                self->format_ctx->streams[videoStreamIndex]->time_base);
                            self->videoq.put(&flush_pkt);
                        }
                        self->pictq_mutex.lock();
                        self->pictq_size = 0;
                        self->pictq_rindex = 0;
                        self->pictq_windex = 0;
                        self->pictq_mutex.unlock();
                        self->mExternalClock.set(seek_target);
                    }
                    self->mSeekRequested = false;
                }


                if((self->audio_st && self->audioq.size > MAX_AUDIOQ_SIZE) ||
                   (self->video_st && self->videoq.size > MAX_VIDEOQ_SIZE))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                if(av_read_frame(pFormatCtx, packet.get()) < 0)
                {
                    if (self->audioq.nb_packets == 0 && self->videoq.nb_packets == 0 && self->pictq_size == 0)
                        self->mVideoEnded = true;
                    continue;
                }
                else
                    self->mVideoEnded = false;

                // Is this a packet from the video stream?
                if(self->video_st && packet->stream_index == self->video_st-pFormatCtx->streams)
                    self->videoq.put(packet.get());
                else if(self->audio_st && packet->stream_index == self->audio_st-pFormatCtx->streams)
                    self->audioq.put(packet.get());
                else
                    av_packet_unref(packet.get());
            }
        }
        catch(std::exception& e) {
            std::cerr << "An error occurred playing the video: " << e.what () << std::endl;
        }

        self->mQuit = true;
    }

private:
    VideoState* mVideoState;
    std::thread mThread;
};


bool VideoState::update()
{
    this->video_refresh();
    return !this->mVideoEnded;
}


int VideoState::stream_open(int stream_index, AVFormatContext *pFormatCtx)
{
    if(stream_index < 0 || stream_index >= static_cast<int>(pFormatCtx->nb_streams))
        return -1;

    // Get a pointer to the codec context for the video stream
    const AVCodec *codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
    if(!codec)
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch(pFormatCtx->streams[stream_index]->codecpar->codec_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        this->audio_st = pFormatCtx->streams + stream_index;

        // Get a pointer to the codec context for the video stream
        this->audio_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(this->audio_ctx, pFormatCtx->streams[stream_index]->codecpar);

// This is not needed any more above FFMpeg version 4.0
#if LIBAVCODEC_VERSION_INT < 3805796
        av_codec_set_pkt_timebase(this->audio_ctx, pFormatCtx->streams[stream_index]->time_base);
#endif

        if (avcodec_open2(this->audio_ctx, codec, nullptr) < 0)
        {
            fprintf(stderr, "Unsupported codec!\n");
            return -1;
        }

        if (!mAudioFactory)
        {
            std::cerr << "No audio factory registered, can not play audio stream" << std::endl;
            avcodec_free_context(&this->audio_ctx);
            this->audio_st = nullptr;
            return -1;
        }

        mAudioDecoder = mAudioFactory->createDecoder(this);
        if (!mAudioDecoder)
        {
            std::cerr << "Failed to create audio decoder, can not play audio stream" << std::endl;
            avcodec_free_context(&this->audio_ctx);
            this->audio_st = nullptr;
            return -1;
        }
        mAudioDecoder->setupFormat();
        break;

    case AVMEDIA_TYPE_VIDEO:
        this->video_st = pFormatCtx->streams + stream_index;

        // Get a pointer to the codec context for the video stream
        this->video_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(this->video_ctx, pFormatCtx->streams[stream_index]->codecpar);

// This is not needed any more above FFMpeg version 4.0
#if LIBAVCODEC_VERSION_INT < 3805796
        av_codec_set_pkt_timebase(this->video_ctx, pFormatCtx->streams[stream_index]->time_base);
#endif

        if (avcodec_open2(this->video_ctx, codec, nullptr) < 0)
        {
            fprintf(stderr, "Unsupported codec!\n");
            return -1;
        }

        this->video_thread = std::make_unique<VideoThread>(this);
        break;

    default:
        break;
    }

    return 0;
}

void VideoState::init(std::shared_ptr<std::istream> inputstream, const std::string &name)
{
    int video_index = -1;
    int audio_index = -1;
    unsigned int i;

    this->av_sync_type = AV_SYNC_DEFAULT;
    this->mQuit = false;

    this->stream = std::move(inputstream);
    if(!this->stream)
        throw std::runtime_error("Failed to open video resource");

    AVIOContext *ioCtx = avio_alloc_context(nullptr, 0, 0, this, istream_read, istream_write, istream_seek);
    if(!ioCtx) throw std::runtime_error("Failed to allocate AVIOContext");

    this->format_ctx = avformat_alloc_context();
    if(this->format_ctx)
        this->format_ctx->pb = ioCtx;

    // Open video file
    ///
    /// format_ctx->pb->buffer must be freed by hand,
    /// if not, valgrind will show memleak, see:
    ///
    /// https://trac.ffmpeg.org/ticket/1357
    ///
    if(!this->format_ctx || avformat_open_input(&this->format_ctx, name.c_str(), nullptr, nullptr))
    {
        if (this->format_ctx != nullptr)
        {
          if (this->format_ctx->pb != nullptr)
          {
              av_freep(&this->format_ctx->pb->buffer);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 80, 100)
              avio_context_free(&this->format_ctx->pb);
#else
              av_freep(&this->format_ctx->pb);
#endif
          }
        }
        // "Note that a user-supplied AVFormatContext will be freed on failure."
        this->format_ctx = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 80, 100)
        avio_context_free(&ioCtx);
#else
        av_freep(&ioCtx);
#endif
        throw std::runtime_error("Failed to open video input");
    }

    // Retrieve stream information
    if(avformat_find_stream_info(this->format_ctx, nullptr) < 0)
        throw std::runtime_error("Failed to retrieve stream information");

    // Dump information about file onto standard error
    av_dump_format(this->format_ctx, 0, name.c_str(), 0);

    for(i = 0;i < this->format_ctx->nb_streams;i++)
    {
        if(this->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index < 0)
            video_index = i;
        if(this->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_index < 0)
            audio_index = i;
    }

    mExternalClock.set(0);

    if(audio_index >= 0)
        this->stream_open(audio_index, this->format_ctx);

    if(video_index >= 0)
    {
        this->stream_open(video_index, this->format_ctx);
    }


    this->parse_thread = std::make_unique<ParseThread>(this);
}

void VideoState::deinit()
{
    this->mQuit = true;

    this->audioq.flush();
    this->videoq.flush();

    mAudioDecoder.reset();

    if (this->parse_thread)
    {
        this->parse_thread.reset();
    }
    if (this->video_thread)
    {
        this->video_thread.reset();
    }

    if(this->audio_ctx)
        avcodec_free_context(&this->audio_ctx);
    this->audio_st = nullptr;
    this->audio_ctx = nullptr;
    if(this->video_ctx)
        avcodec_free_context(&this->video_ctx);
    this->video_st = nullptr;
    this->video_ctx = nullptr;

    if(this->sws_context)
        sws_freeContext(this->sws_context);
    this->sws_context = nullptr;

    if(this->format_ctx)
    {
        ///
        /// format_ctx->pb->buffer must be freed by hand,
        /// if not, valgrind will show memleak, see:
        ///
        /// https://trac.ffmpeg.org/ticket/1357
        ///
        if (this->format_ctx->pb != nullptr)
        {
            av_freep(&this->format_ctx->pb->buffer);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 80, 100)
            avio_context_free(&this->format_ctx->pb);
#else
            av_freep(&this->format_ctx->pb);
#endif
        }
        avformat_close_input(&this->format_ctx);
    }

    if (mTexture)
    {
        // reset Image separately, it's pointing to *this and there might still be outside references to mTexture
        mTexture->setImage(nullptr);
        mTexture = nullptr;
    }

    // Deallocate RGBA frame queue.
    for (auto & i : this->pictq)
        i.rgbaFrame = nullptr;

}

double VideoState::get_external_clock()
{
    return mExternalClock.get() / 1000000.0;
}

double VideoState::get_master_clock()
{
    if(this->av_sync_type == AV_SYNC_VIDEO_MASTER)
        return this->get_video_clock();
    if(this->av_sync_type == AV_SYNC_AUDIO_MASTER)
        return this->get_audio_clock();
    return this->get_external_clock();
}

double VideoState::get_video_clock() const
{
    return this->frame_last_pts;
}

double VideoState::get_audio_clock()
{
    if (!mAudioDecoder)
        return 0.0;
    return mAudioDecoder->getAudioClock();
}

void VideoState::setPaused(bool isPaused)
{
    this->mPaused = isPaused;
    mExternalClock.setPaused(isPaused);
}

void VideoState::seekTo(double time)
{
    time = std::max(0.0, time);
    time = std::min(getDuration(), time);
    mSeekPos = (uint64_t) (time * AV_TIME_BASE);
    mSeekRequested = true;
}

double VideoState::getDuration() const
{
    return this->format_ctx->duration / 1000000.0;
}


ExternalClock::ExternalClock()
    : mTimeBase(av_gettime())
    , mPausedAt(0)
    , mPaused(false)
{
}

void ExternalClock::setPaused(bool paused)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mPaused == paused)
        return;
    if (paused)
    {
        mPausedAt = av_gettime() - mTimeBase;
    }
    else
        mTimeBase = av_gettime() - mPausedAt;
    mPaused = paused;
}

uint64_t ExternalClock::get()
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mPaused)
        return mPausedAt;
    else
        return av_gettime() - mTimeBase;
}

void ExternalClock::set(uint64_t time)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mTimeBase = av_gettime() - time;
    mPausedAt = time;
}

}

