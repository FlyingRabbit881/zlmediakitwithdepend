﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "AACRtp.h"

namespace mediakit{

AACRtpEncoder::AACRtpEncoder(uint32_t ui32Ssrc,
                             uint32_t ui32MtuSize,
                             uint32_t ui32SampleRate,
                             uint8_t ui8PayloadType,
                             uint8_t ui8Interleaved) :
        RtpInfo(ui32Ssrc,
                ui32MtuSize,
                ui32SampleRate,
                ui8PayloadType,
                ui8Interleaved){
}

void AACRtpEncoder::inputFrame(const Frame::Ptr &frame) {
    GET_CONFIG(uint32_t, cycleMS, Rtp::kCycleMS);
    auto uiStamp = frame->dts();
    auto pcData = frame->data() + frame->prefixSize();
    auto iLen = frame->size() - frame->prefixSize();

    uiStamp %= cycleMS;
    auto *ptr = (char *) pcData;
    auto iSize = iLen;
    while (iSize > 0) {
        if (iSize <= _ui32MtuSize - 20) {
            _aucSectionBuf[0] = 0;
            _aucSectionBuf[1] = 16;
            _aucSectionBuf[2] = (iLen >> 5) & 0xFF;
            _aucSectionBuf[3] = ((iLen & 0x1F) << 3) & 0xFF;
            memcpy(_aucSectionBuf + 4, ptr, iSize);
            makeAACRtp(_aucSectionBuf, iSize + 4, true, uiStamp);
            break;
        }
        _aucSectionBuf[0] = 0;
        _aucSectionBuf[1] = 16;
        _aucSectionBuf[2] = ((iLen) >> 5) & 0xFF;
        _aucSectionBuf[3] = ((iLen & 0x1F) << 3) & 0xFF;
        memcpy(_aucSectionBuf + 4, ptr, _ui32MtuSize - 20);
        makeAACRtp(_aucSectionBuf, _ui32MtuSize - 16, false, uiStamp);
        ptr += (_ui32MtuSize - 20);
        iSize -= (_ui32MtuSize - 20);
    }
}

void AACRtpEncoder::makeAACRtp(const void *data, size_t len, bool mark, uint32_t uiStamp) {
    RtpCodec::inputRtp(makeRtp(getTrackType(), data, len, mark, uiStamp), false);
}

/////////////////////////////////////////////////////////////////////////////////////

AACRtpDecoder::AACRtpDecoder(const Track::Ptr &track) {
    auto aacTrack = dynamic_pointer_cast<AACTrack>(track);
    if (!aacTrack || !aacTrack->ready()) {
        WarnL << "该aac track无效!";
    } else {
        _aac_cfg = aacTrack->getAacCfg();
    }
    obtainFrame();
}

AACRtpDecoder::AACRtpDecoder() {
    obtainFrame();
}

void AACRtpDecoder::obtainFrame() {
    //从缓存池重新申请对象，防止覆盖已经写入环形缓存的对象
    _frame = ResourcePoolHelper<FrameImp>::obtainObj();
    _frame->_prefix_size = 0;
    _frame->_buffer.clear();
    _frame->_codec_id = CodecAAC;
}

bool AACRtpDecoder::inputRtp(const RtpPacket::Ptr &rtppack, bool key_pos) {
    //rtp数据开始部分
    uint8_t *ptr = (uint8_t *) rtppack->data() + rtppack->offset;
    //rtp数据末尾
    uint8_t *end = (uint8_t *) rtppack->data() + rtppack->size();
    //首2字节表示Au-Header的个数，单位bit，所以除以16得到Au-Header个数
    uint16_t au_header_count = ((ptr[0] << 8) | ptr[1]) >> 4;
    //记录au_header起始指针
    uint8_t *au_header_ptr = ptr + 2;
    ptr = au_header_ptr +  au_header_count * 2;

    if (end < ptr) {
        //数据不够
        return false;
    }

    if (!_last_dts) {
        //记录第一个时间戳
        _last_dts = rtppack->timeStamp;
    }

    //每个audio unit时间戳增量
    auto dts_inc = (rtppack->timeStamp - _last_dts) / au_header_count;
    if (dts_inc < 0 && dts_inc > 100) {
        //时间戳增量异常，忽略
        dts_inc = 0;
    }

    for (int i = 0; i < au_header_count; ++i) {
        // 之后的2字节是AU_HEADER,其中高13位表示一帧AAC负载的字节长度，低3位无用
        uint16_t size = ((au_header_ptr[0] << 8) | au_header_ptr[1]) >> 3;
        if (ptr + size > end) {
            //数据不够
            break;
        }

        if (size) {
            //设置aac数据
            _frame->_buffer.assign((char *) ptr, size);
            //设置当前audio unit时间戳
            _frame->_dts = _last_dts + i * dts_inc;
            ptr += size;
            au_header_ptr += 2;
            flushData();
        }
    }
    //记录上次时间戳
    _last_dts = rtppack->timeStamp;
    return false;
}

void AACRtpDecoder::flushData() {
    //插入adts头
    char adts_header[32] = {0};
    auto size = dumpAacConfig(_aac_cfg, _frame->_buffer.size(), (uint8_t *) adts_header, sizeof(adts_header));
    if (size > 0) {
        //插入adts头
        _frame->_buffer.insert(0, adts_header, size);
        _frame->_prefix_size = size;
    }
    RtpCodec::inputFrame(_frame);
    obtainFrame();
}

}//namespace mediakit