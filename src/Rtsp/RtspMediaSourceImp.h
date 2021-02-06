﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_
#define SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_

#include "Rtmp/amf.h"
#include "RtspMediaSource.h"
#include "RtspDemuxer.h"
#include "Common/MultiMediaSourceMuxer.h"
using namespace toolkit;

namespace mediakit {
class RtspMediaSourceImp : public RtspMediaSource, public Demuxer::Listener , public MultiMediaSourceMuxer::Listener  {
public:
    typedef std::shared_ptr<RtspMediaSourceImp> Ptr;

    /**
     * 构造函数
     * @param vhost 虚拟主机
     * @param app 应用名
     * @param id 流id
     * @param ringSize 环形缓存大小
     */
    RtspMediaSourceImp(const string &vhost, const string &app, const string &id, int ringSize = RTP_GOP_SIZE) : RtspMediaSource(vhost, app, id,ringSize) {
        _demuxer = std::make_shared<RtspDemuxer>();
        _demuxer->setTrackListener(this);
    }

    ~RtspMediaSourceImp() = default;

    /**
     * 设置sdp
     */
    void setSdp(const string &strSdp) override {
        _demuxer->loadSdp(strSdp);
        RtspMediaSource::setSdp(strSdp);
    }

    /**
     * 输入rtp并解析
     */
    void onWrite(RtpPacket::Ptr rtp, bool key_pos) override {
        if (_all_track_ready && !_muxer->isEnabled()) {
            //获取到所有Track后，并且未开启转协议，那么不需要解复用rtp
            //在关闭rtp解复用后，无法知道是否为关键帧，这样会导致无法秒开，或者开播花屏
            key_pos = rtp->type == TrackVideo;
        } else {
            //需要解复用rtp
            key_pos = _demuxer->inputRtp(rtp);
        }
        RtspMediaSource::onWrite(std::move(rtp), key_pos);
    }

    /**
     * 获取观看总人数，包括(hls/rtsp/rtmp)
     */
    int totalReaderCount() override{
        return readerCount() + (_muxer ? _muxer->totalReaderCount() : 0);
    }

    /**
     * 设置协议转换
     * @param enableHls  是否转换成hls
     * @param enableMP4  是否mp4录制
     */
    void setProtocolTranslation(bool enableHls,bool enableMP4){
        //不重复生成rtsp
        _muxer = std::make_shared<MultiMediaSourceMuxer>(getVhost(), getApp(), getId(), _demuxer->getDuration(), false, true, enableHls, enableMP4);
        _muxer->setMediaListener(getListener());
        _muxer->setTrackListener(static_pointer_cast<RtspMediaSourceImp>(shared_from_this()));
        //让_muxer对象拦截一部分事件(比如说录像相关事件)
        MediaSource::setListener(_muxer);

        for(auto &track : _demuxer->getTracks(false)){
            _muxer->addTrack(track);
            track->addDelegate(_muxer);
        }
    }

    /**
     * _demuxer触发的添加Track事件
     */
    void onAddTrack(const Track::Ptr &track) override {
        if(_muxer){
            _muxer->addTrack(track);
            track->addDelegate(_muxer);
        }
    }

    /**
     * _muxer触发的所有Track就绪的事件
     */
    void onAllTrackReady() override{
        _all_track_ready = true;
    }

    /**
     * 设置事件监听器
     * @param listener 监听器
     */
    void setListener(const std::weak_ptr<MediaSourceEvent> &listener) override{
        if (_muxer) {
            //_muxer对象不能处理的事件再给listener处理
            _muxer->setMediaListener(listener);
        } else {
            //未创建_muxer对象，事件全部给listener处理
            MediaSource::setListener(listener);
        }
    }

private:
    RtspDemuxer::Ptr _demuxer;
    MultiMediaSourceMuxer::Ptr _muxer;
    bool _all_track_ready = false;
};
} /* namespace mediakit */

#endif /* SRC_RTSP_RTSPTORTMPMEDIASOURCE_H_ */
