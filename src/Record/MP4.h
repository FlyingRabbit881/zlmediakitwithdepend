﻿/*
 * Copyright (c) 2016 The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/xia-chu/ZLMediaKit).
 *
 * Use of this source code is governed by MIT license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_MP4_H
#define ZLMEDIAKIT_MP4_H
#ifdef ENABLE_MP4
#include <memory>
#include <string>
#include "mov-writer.h"
#include "mov-reader.h"
#include "mpeg4-hevc.h"
#include "mpeg4-avc.h"
#include "mpeg4-aac.h"
#include "mov-buffer.h"
#include "mov-format.h"
using namespace std;
namespace mediakit {

//以下是fmp4/mov的通用接口，简单包装了ireader/media-server的接口
typedef struct mp4_writer_t mp4_writer_t;
mp4_writer_t* mp4_writer_create(int is_fmp4, const struct mov_buffer_t *buffer, void* param, int flags);
void mp4_writer_destroy(mp4_writer_t* mp4);
int mp4_writer_add_audio(mp4_writer_t* mp4, uint8_t object, int channel_count, int bits_per_sample, int sample_rate, const void* extra_data, size_t extra_data_size);
int mp4_writer_add_video(mp4_writer_t* mp4, uint8_t object, int width, int height, const void* extra_data, size_t extra_data_size);
int mp4_writer_add_subtitle(mp4_writer_t* mp4, uint8_t object, const void* extra_data, size_t extra_data_size);
int mp4_writer_write(mp4_writer_t* mp4, int track, const void* data, size_t bytes, int64_t pts, int64_t dts, int flags);
int mp4_writer_write_l(mp4_writer_t* mp4, int track, const void* data, size_t bytes, int64_t pts, int64_t dts, int flags, int add_nalu_size);
int mp4_writer_save_segment(mp4_writer_t* mp4);
int mp4_writer_init_segment(mp4_writer_t* mp4);

//mp4文件IO的抽象接口类
class MP4FileIO : public std::enable_shared_from_this<MP4FileIO> {
public:
    using Ptr = std::shared_ptr<MP4FileIO>;
    using Writer = std::shared_ptr<mp4_writer_t>;
    using Reader = std::shared_ptr<mov_reader_t>;

    MP4FileIO() = default;
    virtual ~MP4FileIO() = default;

    /**
     * 创建mp4复用器
     * @param flags 支持0、MOV_FLAG_FASTSTART、MOV_FLAG_SEGMENT
     * @param is_fmp4 是否为fmp4还是普通mp4
     * @return mp4复用器
     */
    virtual Writer createWriter(int flags, bool is_fmp4 = false);

    /**
     * 创建mp4解复用器
     * @return mp4解复用器
     */
    virtual Reader createReader();

    /**
     * 获取文件读写位置
     */
    virtual size_t onTell() = 0;

    /**
     * seek至文件某处
     * @param offset 文件偏移量
     * @return 是否成功(0成功)
     */
    virtual int onSeek(size_t offset) = 0;

    /**
     * 从文件读取一定数据
     * @param data 数据存放指针
     * @param bytes 指针长度
     * @return 是否成功(0成功)
     */
    virtual int onRead(void *data, size_t bytes) = 0;

    /**
     * 写入文件一定数据
     * @param data 数据指针
     * @param bytes 数据长度
     * @return 是否成功(0成功)
     */
    virtual int onWrite(const void *data, size_t bytes) = 0;
};

//磁盘MP4文件类
class MP4FileDisk : public MP4FileIO {
public:
    using Ptr = std::shared_ptr<MP4FileDisk>;
    MP4FileDisk() = default;
    ~MP4FileDisk() override = default;

    /**
     * 打开磁盘文件
     * @param file 文件路径
     * @param mode fopen的方式
     */
    void openFile(const char *file, const char *mode);

    /**
     * 关闭磁盘文件
     */
    void closeFile();

protected:
    size_t onTell() override;
    int onSeek(size_t offset) override;
    int onRead(void *data, size_t bytes) override;
    int onWrite(const void *data, size_t bytes) override;

private:
    std::shared_ptr<FILE> _file;
};

class MP4FileMemory : public MP4FileIO{
public:
    using Ptr = std::shared_ptr<MP4FileMemory>;
    MP4FileMemory() = default;
    ~MP4FileMemory() override = default;

    /**
     * 获取文件大小
     */
    size_t fileSize() const;

    /**
     * 获取并清空文件缓存
     */
    string getAndClearMemory();

protected:
    size_t onTell() override;
    int onSeek(size_t offset) override;
    int onRead(void *data, size_t bytes) override;
    int onWrite(const void *data, size_t bytes) override;

private:
    size_t _offset = 0;
    string _memory;
};

}//namespace mediakit
#endif //NABLE_MP4RECORD
#endif //ZLMEDIAKIT_MP4_H
