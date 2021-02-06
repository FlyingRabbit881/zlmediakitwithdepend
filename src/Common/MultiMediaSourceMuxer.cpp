﻿/*
* Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
*
* This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
*
* Use of this source code is governed by MIT license that can be found in the
* LICENSE file in the root of the source tree. All contributing project authors
* may be found in the AUTHORS file in the root of the source tree.
*/

#include <math.h>
#include "Common/config.h"
#include "MultiMediaSourceMuxer.h"
namespace mediakit {

///////////////////////////////MultiMuxerPrivate//////////////////////////////////

MultiMuxerPrivate::~MultiMuxerPrivate() {}
MultiMuxerPrivate::MultiMuxerPrivate(const string &vhost, const string &app, const string &stream, float dur_sec,
                                     bool enable_rtsp, bool enable_rtmp, bool enable_hls, bool enable_mp4) {
    _stream_url = vhost + " " + app + " " + stream;
    if (enable_rtmp) {
        _rtmp = std::make_shared<RtmpMediaSourceMuxer>(vhost, app, stream, std::make_shared<TitleMeta>(dur_sec));
    }
    if (enable_rtsp) {
        _rtsp = std::make_shared<RtspMediaSourceMuxer>(vhost, app, stream, std::make_shared<TitleSdp>(dur_sec));
    }

    if (enable_hls) {
        _hls = dynamic_pointer_cast<HlsRecorder>(Recorder::createRecorder(Recorder::type_hls, vhost, app, stream));
    }

    if (enable_mp4) {
        _mp4 = Recorder::createRecorder(Recorder::type_mp4, vhost, app, stream);
    }

    _ts = std::make_shared<TSMediaSourceMuxer>(vhost, app, stream);

#if defined(ENABLE_MP4)
    _fmp4 = std::make_shared<FMP4MediaSourceMuxer>(vhost, app, stream);
#endif
}

void MultiMuxerPrivate::resetTracks() {
    if (_rtmp) {
        _rtmp->resetTracks();
    }
    if (_rtsp) {
        _rtsp->resetTracks();
    }
    if (_ts) {
        _ts->resetTracks();
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->resetTracks();
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    auto hls = _hls;
    if (hls) {
        hls->resetTracks();
    }

    auto mp4 = _mp4;
    if (mp4) {
        mp4->resetTracks();
    }
}

void MultiMuxerPrivate::setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    _listener = listener;
    if (_rtmp) {
        _rtmp->setListener(listener);
    }
    if (_rtsp) {
        _rtsp->setListener(listener);
    }
    if (_ts) {
        _ts->setListener(listener);
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->setListener(listener);
    }
#endif
    auto hls = _hls;
    if (hls) {
        hls->setListener(listener);
    }
}

int MultiMuxerPrivate::totalReaderCount() const {
    auto hls = _hls;
    return (_rtsp ? _rtsp->readerCount() : 0) +
           (_rtmp ? _rtmp->readerCount() : 0) +
           (_ts ? _ts->readerCount() : 0) +
#if defined(ENABLE_MP4)
           (_fmp4 ? _fmp4->readerCount() : 0) +
#endif
           (hls ? hls->readerCount() : 0);
}

static std::shared_ptr<MediaSinkInterface> makeRecorder(const vector<Track::Ptr> &tracks, Recorder::type type, const string &custom_path, MediaSource &sender){
    auto recorder = Recorder::createRecorder(type, sender.getVhost(), sender.getApp(), sender.getId(), custom_path);
    for (auto &track : tracks) {
        recorder->addTrack(track);
    }
    return recorder;
}

//此函数可能跨线程调用
bool MultiMuxerPrivate::setupRecord(MediaSource &sender, Recorder::type type, bool start, const string &custom_path){
    switch (type) {
        case Recorder::type_hls : {
            if (start && !_hls) {
                //开始录制
                auto hls = dynamic_pointer_cast<HlsRecorder>(makeRecorder(getTracks(true), type, custom_path, sender));
                if (hls) {
                    //设置HlsMediaSource的事件监听器
                    hls->setListener(_listener);
                }
                _hls = hls;
            } else if (!start && _hls) {
                //停止录制
                _hls = nullptr;
            }
            return true;
        }
        case Recorder::type_mp4 : {
            if (start && !_mp4) {
                //开始录制
                _mp4 = makeRecorder(getTracks(true), type, custom_path, sender);
            } else if (!start && _mp4) {
                //停止录制
                _mp4 = nullptr;
            }
            return true;
        }
        default : return false;
    }
}

//此函数可能跨线程调用
bool MultiMuxerPrivate::isRecording(MediaSource &sender, Recorder::type type){
    switch (type){
        case Recorder::type_hls :
            return _hls ? true : false;
        case Recorder::type_mp4 :
            return _mp4 ? true : false;
        default:
            return false;
    }
}

void MultiMuxerPrivate::setTimeStamp(uint32_t stamp) {
    if (_rtmp) {
        _rtmp->setTimeStamp(stamp);
    }
    if (_rtsp) {
        _rtsp->setTimeStamp(stamp);
    }
}

void MultiMuxerPrivate::setTrackListener(Listener *listener) {
    _track_listener = listener;
}

void MultiMuxerPrivate::onTrackReady(const Track::Ptr &track) {
    if (_rtmp) {
        _rtmp->addTrack(track);
    }
    if (_rtsp) {
        _rtsp->addTrack(track);
    }
    if (_ts) {
        _ts->addTrack(track);
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->addTrack(track);
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    auto hls = _hls;
    if (hls) {
        hls->addTrack(track);
    }
    auto mp4 = _mp4;
    if (mp4) {
        mp4->addTrack(track);
    }
}

bool MultiMuxerPrivate::isEnabled(){
    auto hls = _hls;
    return (_rtmp ? _rtmp->isEnabled() : false) ||
           (_rtsp ? _rtsp->isEnabled() : false) ||
           (_ts ? _ts->isEnabled() : false) ||
#if defined(ENABLE_MP4)
           (_fmp4 ? _fmp4->isEnabled() : false) ||
#endif
           (hls ? hls->isEnabled() : false) || _mp4;
}

void MultiMuxerPrivate::onTrackFrame(const Frame::Ptr &frame) {
    if (_rtmp) {
        _rtmp->inputFrame(frame);
    }
    if (_rtsp) {
        _rtsp->inputFrame(frame);
    }
    if (_ts) {
        _ts->inputFrame(frame);
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->inputFrame(frame);
    }
#endif

    //拷贝智能指针，目的是为了防止跨线程调用设置录像相关api导致的线程竞争问题
    //此处使用智能指针拷贝来确保线程安全，比互斥锁性能更优
    auto hls = _hls;
    if (hls) {
        hls->inputFrame(frame);
    }
    auto mp4 = _mp4;
    if (mp4) {
        mp4->inputFrame(frame);
    }
}

static string getTrackInfoStr(const TrackSource *track_src){
    _StrPrinter codec_info;
    auto tracks = track_src->getTracks(true);
    for (auto &track : tracks) {
        auto codec_type = track->getTrackType();
        codec_info << track->getCodecName();
        switch (codec_type) {
            case TrackAudio : {
                auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                codec_info << "["
                           << audio_track->getAudioSampleRate() << "/"
                           << audio_track->getAudioChannel() << "/"
                           << audio_track->getAudioSampleBit() << "] ";
                break;
            }
            case TrackVideo : {
                auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                codec_info << "["
                           << video_track->getVideoWidth() << "/"
                           << video_track->getVideoHeight() << "/"
                           << round(video_track->getVideoFps()) << "] ";
                break;
            }
            default:
                break;
        }
    }
    return std::move(codec_info);
}

void MultiMuxerPrivate::onAllTrackReady() {
    if (_rtmp) {
        _rtmp->onAllTrackReady();
    }
    if (_rtsp) {
        _rtsp->onAllTrackReady();
    }
#if defined(ENABLE_MP4)
    if (_fmp4) {
        _fmp4->onAllTrackReady();
    }
#endif
    if (_track_listener) {
        _track_listener->onAllTrackReady();
    }
    InfoL << "stream: " << _stream_url << " , codec info: " << getTrackInfoStr(this);
}

///////////////////////////////MultiMediaSourceMuxer//////////////////////////////////

MultiMediaSourceMuxer::~MultiMediaSourceMuxer() {}

MultiMediaSourceMuxer::MultiMediaSourceMuxer(const string &vhost, const string &app, const string &stream, float dur_sec,
                                             bool enable_rtsp, bool enable_rtmp, bool enable_hls, bool enable_mp4) {
    _muxer.reset(new MultiMuxerPrivate(vhost, app, stream, dur_sec, enable_rtsp, enable_rtmp, enable_hls, enable_mp4));
    _muxer->setTrackListener(this);
}

void MultiMediaSourceMuxer::setMediaListener(const std::weak_ptr<MediaSourceEvent> &listener) {
    setDelegate(listener);
    //拦截事件
    _muxer->setMediaListener(shared_from_this());
}

void MultiMediaSourceMuxer::setTrackListener(const std::weak_ptr<MultiMuxerPrivate::Listener> &listener) {
    _track_listener = listener;
}

int MultiMediaSourceMuxer::totalReaderCount() const {
    return _muxer->totalReaderCount();
}

void MultiMediaSourceMuxer::setTimeStamp(uint32_t stamp) {
    _muxer->setTimeStamp(stamp);
}

vector<Track::Ptr> MultiMediaSourceMuxer::getTracks(MediaSource &sender, bool trackReady) const {
    return _muxer->getTracks(trackReady);
}

int MultiMediaSourceMuxer::totalReaderCount(MediaSource &sender) {
    auto listener = getDelegate();
    if (!listener) {
        return totalReaderCount();
    }
    return listener->totalReaderCount(sender);
}

bool MultiMediaSourceMuxer::setupRecord(MediaSource &sender, Recorder::type type, bool start, const string &custom_path) {
    return _muxer->setupRecord(sender, type, start, custom_path);
}

bool MultiMediaSourceMuxer::isRecording(MediaSource &sender, Recorder::type type) {
    return _muxer->isRecording(sender,type);
}

void MultiMediaSourceMuxer::startSendRtp(MediaSource &sender, const string &dst_url, uint16_t dst_port, const string &ssrc, bool is_udp, uint16_t src_port, const function<void(uint16_t local_port, const SockException &ex)> &cb){
#if defined(ENABLE_RTPPROXY)
    RtpSender::Ptr rtp_sender = std::make_shared<RtpSender>(atoi(ssrc.data()));
    weak_ptr<MultiMediaSourceMuxer> weak_self = shared_from_this();
    rtp_sender->startSend(dst_url, dst_port, is_udp, src_port, [weak_self, rtp_sender, cb, ssrc](uint16_t local_port, const SockException &ex) {
        cb(local_port, ex);
        auto strong_self = weak_self.lock();
        if (!strong_self || ex) {
            return;
        }
        for (auto &track : strong_self->_muxer->getTracks(false)) {
            rtp_sender->addTrack(track);
        }
        rtp_sender->addTrackCompleted();
        lock_guard<mutex> lck(strong_self->_rtp_sender_mtx);
        strong_self->_rtp_sender[ssrc] = rtp_sender;
    });
#else
    cb(0, SockException(Err_other, "该功能未启用，编译时请打开ENABLE_RTPPROXY宏"));
#endif//ENABLE_RTPPROXY
}

bool MultiMediaSourceMuxer::stopSendRtp(MediaSource &sender, const string& ssrc){
#if defined(ENABLE_RTPPROXY)
    if (ssrc.empty()) {
        //关闭全部
        lock_guard<mutex> lck(_rtp_sender_mtx);
        auto size = _rtp_sender.size();
        _rtp_sender.clear();
        return size;
    }
    //关闭特定的
    lock_guard<mutex> lck(_rtp_sender_mtx);
    return _rtp_sender.erase(ssrc);
#else
    return false;
#endif//ENABLE_RTPPROXY
}

void MultiMediaSourceMuxer::addTrack(const Track::Ptr &track) {
    if (CodecL16 == track->getCodecId()) {
        WarnL << "L16音频格式目前只支持RTSP协议推流拉流!!!";
        return;
    }
    _muxer->addTrack(track);
}

void MultiMediaSourceMuxer::addTrackCompleted() {
    _muxer->addTrackCompleted();
}

void MultiMediaSourceMuxer::onAllTrackReady(){
    _muxer->setMediaListener(shared_from_this());
    auto listener = _track_listener.lock();
    if(listener){
        listener->onAllTrackReady();
    }
}

void MultiMediaSourceMuxer::resetTracks() {
    _muxer->resetTracks();
}

//该类实现frame级别的时间戳覆盖
class FrameModifyStamp : public Frame{
public:
    typedef std::shared_ptr<FrameModifyStamp> Ptr;
    FrameModifyStamp(const Frame::Ptr &frame, Stamp &stamp){
        _frame = frame;
        //覆盖时间戳
        stamp.revise(frame->dts(), frame->pts(), _dts, _pts, true);
    }
    ~FrameModifyStamp() override {}

    uint32_t dts() const override{
        return (uint32_t)_dts;
    }

    uint32_t pts() const override{
        return (uint32_t)_pts;
    }

    size_t prefixSize() const override {
        return _frame->prefixSize();
    }

    bool keyFrame() const override {
        return _frame->keyFrame();
    }

    bool configFrame() const override {
        return _frame->configFrame();
    }

    bool cacheAble() const override {
        return _frame->cacheAble();
    }

    char *data() const override {
        return _frame->data();
    }

    size_t size() const override {
        return _frame->size();
    }

    CodecId getCodecId() const override {
        return _frame->getCodecId();
    }
private:
    int64_t _dts;
    int64_t _pts;
    Frame::Ptr _frame;
};

void MultiMediaSourceMuxer::inputFrame(const Frame::Ptr &frame_in) {
    GET_CONFIG(bool, modify_stamp, General::kModifyStamp);
    auto frame = frame_in;
    if (modify_stamp) {
        //开启了时间戳覆盖
        frame = std::make_shared<FrameModifyStamp>(frame, _stamp[frame->getTrackType()]);
    }
    _muxer->inputFrame(frame);

#if defined(ENABLE_RTPPROXY)
    lock_guard<mutex> lck(_rtp_sender_mtx);
    for (auto &pr : _rtp_sender) {
        pr.second->inputFrame(frame);
    }
#endif //ENABLE_RTPPROXY

}

bool MultiMediaSourceMuxer::isEnabled(){
    GET_CONFIG(uint32_t, stream_none_reader_delay_ms, General::kStreamNoneReaderDelayMS);
    if (!_is_enable || _last_check.elapsedTime() > stream_none_reader_delay_ms) {
        //无人观看时，每次检查是否真的无人观看
        //有人观看时，则延迟一定时间检查一遍是否无人观看了(节省性能)
#if defined(ENABLE_RTPPROXY)
        _is_enable = (_muxer->isEnabled() || _rtp_sender.size());
#else
        _is_enable = _muxer->isEnabled();
#endif //ENABLE_RTPPROXY
        if (_is_enable) {
            //无人观看时，不刷新计时器,因为无人观看时每次都会检查一遍，所以刷新计数器无意义且浪费cpu
            _last_check.resetTime();
        }
    }
    return _is_enable;
}


}//namespace mediakit
