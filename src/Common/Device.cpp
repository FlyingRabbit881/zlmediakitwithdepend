﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Device.h"
#include "Util/logger.h"
#include "Util/base64.h"
#include "Extension/AAC.h"
#include "Extension/Opus.h"
#include "Extension/G711.h"
#include "Extension/H264.h"
#include "Extension/H265.h"
#ifdef ENABLE_FAAC
#include "Codec/AACEncoder.h"
#endif //ENABLE_FAAC

#ifdef ENABLE_X264
#include "Codec/H264Encoder.h"
#endif //ENABLE_X264
using namespace toolkit;

namespace mediakit {

DevChannel::DevChannel(const string &vhost, const string &app, const string &stream_id,
                       float duration, bool enable_hls, bool enable_mp4) :
        MultiMediaSourceMuxer(vhost, app, stream_id, duration, true, true, enable_hls, enable_mp4) {}

DevChannel::~DevChannel() {}

void DevChannel::inputYUV(char* apcYuv[3], int aiYuvLen[3], uint32_t uiStamp) {
#ifdef ENABLE_X264
    //TimeTicker1(50);
    if (!_pH264Enc) {
        _pH264Enc.reset(new H264Encoder());
        if (!_pH264Enc->init(_video->iWidth, _video->iHeight, _video->iFrameRate)) {
            _pH264Enc.reset();
            WarnL << "H264Encoder init failed!";
        }
    }
    if (_pH264Enc) {
        H264Encoder::H264Frame *pOut;
        int iFrames = _pH264Enc->inputData(apcYuv, aiYuvLen, uiStamp, &pOut);
        for (int i = 0; i < iFrames; i++) {
            inputH264((char *) pOut[i].pucData, pOut[i].iLength, uiStamp);
        }
    }
#else
    WarnL << "h264编码未启用,该方法无效,编译时请打开ENABLE_X264选项";
#endif //ENABLE_X264
}

void DevChannel::inputPCM(char* pcData, int iDataLen, uint32_t uiStamp) {
#ifdef ENABLE_FAAC
    if (!_pAacEnc) {
        _pAacEnc.reset(new AACEncoder());
        if (!_pAacEnc->init(_audio->iSampleRate, _audio->iChannel, _audio->iSampleBit)) {
            _pAacEnc.reset();
            WarnL << "AACEncoder init failed!";
        }
    }
    if (_pAacEnc) {
        unsigned char *pucOut;
        int iRet = _pAacEnc->inputData(pcData, iDataLen, &pucOut);
        if (iRet > 7) {
            inputAAC((char *) pucOut + 7, iRet - 7, uiStamp, (char *)pucOut);
        }
    }
#else
    WarnL << "aac编码未启用,该方法无效,编译时请打开ENABLE_FAAC选项";
#endif //ENABLE_FAAC
}

void DevChannel::inputH264(const char *data, int len, uint32_t dts, uint32_t pts) {
    if(dts == 0){
        dts = (uint32_t)_aTicker[0].elapsedTime();
    }
    if(pts == 0){
        pts = dts;
    }

    //由于rtmp/hls/mp4需要缓存时间戳相同的帧，
    //所以使用FrameNoCacheAble类型的帧反而会在转换成FrameCacheAble时多次内存拷贝
    //在此处只拷贝一次，性能开销更低
    H264Frame::Ptr frame = std::make_shared<H264Frame>();
    frame->_dts = dts;
    frame->_pts = pts;
    frame->_buffer.assign(data, len);
    frame->_prefix_size = prefixSize(data,len);
    inputFrame(frame);
}

void DevChannel::inputH265(const char *data, int len, uint32_t dts, uint32_t pts) {
    if(dts == 0){
        dts = (uint32_t)_aTicker[0].elapsedTime();
    }
    if(pts == 0){
        pts = dts;
    }

    //由于rtmp/hls/mp4需要缓存时间戳相同的帧，
    //所以使用FrameNoCacheAble类型的帧反而会在转换成FrameCacheAble时多次内存拷贝
    //在此处只拷贝一次，性能开销更低
    H265Frame::Ptr frame = std::make_shared<H265Frame>();
    frame->_dts = dts;
    frame->_pts = pts;
    frame->_buffer.assign(data, len);
    frame->_prefix_size = prefixSize(data,len);
    inputFrame(frame);
}

class FrameAutoDelete : public FrameFromPtr{
public:
    template <typename ... ARGS>
    FrameAutoDelete(ARGS && ...args) : FrameFromPtr(std::forward<ARGS>(args)...){}

    ~FrameAutoDelete() override {
        delete [] _ptr;
    };

    bool cacheAble() const override {
        return true;
    }
};

void DevChannel::inputAAC(const char *data_without_adts, int len, uint32_t dts, const char *adts_header){
    if (dts == 0) {
        dts = (uint32_t) _aTicker[1].elapsedTime();
    }

    if (adts_header) {
        if (adts_header + ADTS_HEADER_LEN == data_without_adts) {
            //adts头和帧在一起
            inputFrame(std::make_shared<FrameFromPtr>(_audio->codecId, (char *) data_without_adts - ADTS_HEADER_LEN, len + ADTS_HEADER_LEN, dts, 0, ADTS_HEADER_LEN));
        } else {
            //adts头和帧不在一起
            char *data_with_adts = new char[len + ADTS_HEADER_LEN];
            memcpy(data_with_adts, adts_header, ADTS_HEADER_LEN);
            memcpy(data_with_adts + ADTS_HEADER_LEN, data_without_adts, len);
            inputFrame(std::make_shared<FrameAutoDelete>(_audio->codecId, data_with_adts, len + ADTS_HEADER_LEN, dts, 0, ADTS_HEADER_LEN));
        }
    } else {
        //没有adts头
        inputFrame(std::make_shared<FrameFromPtr>(_audio->codecId, (char *) data_without_adts, len, dts, 0, 0));
    }
}

void DevChannel::inputAudio(const char *data, int len, uint32_t dts){
    if (dts == 0) {
        dts = (uint32_t) _aTicker[1].elapsedTime();
    }
    inputFrame(std::make_shared<FrameFromPtr>(_audio->codecId, (char *) data, len, dts, 0));
}

void DevChannel::initVideo(const VideoInfo &info) {
    _video = std::make_shared<VideoInfo>(info);
    switch (info.codecId){
        case CodecH265 : addTrack(std::make_shared<H265Track>()); break;
        case CodecH264 : addTrack(std::make_shared<H264Track>()); break;
        default: WarnL << "不支持该类型的视频编码类型:" << info.codecId; break;
    }
}

void DevChannel::initAudio(const AudioInfo &info) {
    _audio = std::make_shared<AudioInfo>(info);
    switch (info.codecId) {
        case CodecAAC : addTrack(std::make_shared<AACTrack>()); break;
        case CodecG711A :
        case CodecG711U : addTrack(std::make_shared<G711Track>(info.codecId, info.iSampleRate, info.iChannel, info.iSampleBit)); break;
        case CodecOpus : addTrack(std::make_shared<OpusTrack>()); break;
        default: WarnL << "不支持该类型的音频编码类型:" << info.codecId; break;
    }
}

MediaOriginType DevChannel::getOriginType(MediaSource &sender) const {
    return MediaOriginType::device_chn;
}

} /* namespace mediakit */

