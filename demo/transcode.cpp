// demo/transcode.cpp
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "media/ffmpeg_allocator.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }
    const char* input_file = argv[1];
    const char* output_file = argv[2];

    // 初始化 FFmpeg 网络（如果需要）
    avformat_network_init();

    // 打开输入
    AVFormatContext* in_fmt_ctx = nullptr;
    if (avformat_open_input(&in_fmt_ctx, input_file, nullptr, nullptr) < 0) {
        fprintf(stderr, "Could not open input file: %s\n", input_file);
        return 1;
    }
    if (avformat_find_stream_info(in_fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "Could not find stream info\n");
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // 查找视频流
    int video_stream = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream < 0) {
        fprintf(stderr, "No video stream found\n");
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // 初始化解码器
    AVCodecParameters* codecpar = in_fmt_ctx->streams[video_stream]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "Decoder not found\n");
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        fprintf(stderr, "Could not allocate decoder context\n");
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    avcodec_parameters_to_context(dec_ctx, codecpar);

    // ★ 替换解码器的 get_buffer2 回调
    // dec_ctx->get_buffer2 = custom_get_buffer2;

    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
        fprintf(stderr, "Could not open decoder\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    printf("Input: %s, %dx%d, codec: %s\n", 
           input_file, dec_ctx->width, dec_ctx->height, decoder->name);

    // 准备输出
    AVFormatContext* out_fmt_ctx = nullptr;
    avformat_alloc_output_context2(&out_fmt_ctx, nullptr, nullptr, output_file);
    if (!out_fmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        fprintf(stderr, "H.264 encoder not found\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    AVStream* out_stream = avformat_new_stream(out_fmt_ctx, encoder);
    if (!out_stream) {
        fprintf(stderr, "Could not create output stream\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        fprintf(stderr, "Could not allocate encoder context\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // 配置编码器
    enc_ctx->width = dec_ctx->width;
    enc_ctx->height = dec_ctx->height;
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = (AVRational){1, 25};
    enc_ctx->framerate = (AVRational){25, 1};
    enc_ctx->bit_rate = 400000;
    enc_ctx->gop_size = 12;
    enc_ctx->max_b_frames = 1;

    // 设置 preset 和 tune
    av_opt_set(enc_ctx->priv_data, "preset", "fast", 0);
    av_opt_set(enc_ctx->priv_data, "tune", "zerolatency", 0);

    out_stream->time_base = enc_ctx->time_base;

    if (avcodec_open2(enc_ctx, encoder, nullptr) < 0) {
        fprintf(stderr, "Could not open encoder\n");
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);

    // 打开输出文件
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file: %s\n", output_file);
            avcodec_free_context(&enc_ctx);
            avformat_free_context(out_fmt_ctx);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_fmt_ctx);
            return 1;
        }
    }

    // 写文件头
    if (avformat_write_header(out_fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "Could not write header\n");
        avio_closep(&out_fmt_ctx->pb);
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    printf("Output: %s, encoder: %s\n", output_file, encoder->name);

    // 准备帧
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* enc_frame = av_frame_alloc();
    
    if (!packet || !frame || !enc_frame) {
        fprintf(stderr, "Could not allocate frames\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // ★ 先用 av_frame_get_buffer 获取 FFmpeg 算好的大小
    enc_frame->format = enc_ctx->pix_fmt;
    enc_frame->width = enc_ctx->width;
    enc_frame->height = enc_ctx->height;
    
    if (av_frame_get_buffer(enc_frame, 0) < 0) {
        fprintf(stderr, "Could not get buffer info for encode frame\n");
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&enc_frame);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }
    
    // 保存 FFmpeg 算好的 linesize 和大小
    int linesizes[4] = {0};
    int actual_sizes[4] = {0};
    for (int i = 0; i < 4; i++) {
        linesizes[i] = enc_frame->linesize[i];
        if (enc_frame->buf[i]) {
            actual_sizes[i] = enc_frame->buf[i]->size;  // FFmpeg 自己算的准确大小
        }
    }
    
    // 释放默认分配的缓冲区
    av_frame_unref(enc_frame);
    
    enc_frame->format = enc_ctx->pix_fmt;
    enc_frame->width = enc_ctx->width;
    enc_frame->height = enc_ctx->height;
    
    // 用内存池重新分配，使用 FFmpeg 计算的准确大小
    for (int i = 0; i < 4 && actual_sizes[i] > 0; i++) {
        AVBufferRef* buf = av_buffer_alloc_from_pool(actual_sizes[i]);
        if (!buf) {
            fprintf(stderr, "Could not allocate encode frame buffer for plane %d\n", i);
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&enc_frame);
            avformat_free_context(out_fmt_ctx);
            avcodec_free_context(&enc_ctx);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_fmt_ctx);
            return 1;
        }
        enc_frame->buf[i] = buf;
        enc_frame->data[i] = buf->data;
        enc_frame->linesize[i] = linesizes[i];
    }
    
    enc_frame->extended_data = enc_frame->data;

    // 创建颜色空间转换器
    SwsContext* sws_ctx = sws_getContext(
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    
    if (!sws_ctx) {
        fprintf(stderr, "Could not create scale context\n");
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&enc_frame);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&enc_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return 1;
    }

    // ========== 帧处理循环 ==========
    int frame_count = 0;
    int64_t pts = 0;
    
    while (av_read_frame(in_fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream) {
            if (avcodec_send_packet(dec_ctx, packet) < 0) {
                fprintf(stderr, "Error sending packet to decoder\n");
                break;
            }
            
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                // 颜色空间转换
                sws_scale(sws_ctx, 
                         (const uint8_t * const*)frame->data, 
                         frame->linesize, 
                         0,
                         dec_ctx->height, 
                         enc_frame->data, 
                         enc_frame->linesize);
                
                // 设置正确的 PTS
                enc_frame->pts = pts++;
                
                // 发送帧到编码器
                int send_ret = avcodec_send_frame(enc_ctx, enc_frame);
                if (send_ret < 0) {
                    fprintf(stderr, "Error sending frame to encoder: %d\n", send_ret);
                    break;
                }
                
                // 接收编码后的包
                AVPacket *enc_pkt = av_packet_alloc();
                if (!enc_pkt) continue;
                
                int recv_ret = 0;
                while (recv_ret == 0) {
                    recv_ret = avcodec_receive_packet(enc_ctx, enc_pkt);
                    if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                        break;
                    }
                    if (recv_ret < 0) {
                        fprintf(stderr, "Error receiving packet from encoder: %d\n", recv_ret);
                        break;
                    }
                    
                    enc_pkt->stream_index = out_stream->index;
                    av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);
                    
                    int write_ret = av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
                    if (write_ret < 0) {
                        fprintf(stderr, "Error writing frame: %d\n", write_ret);
                    } else {
                        frame_count++;
                    }
                }
                av_packet_free(&enc_pkt);
            }
        }
        av_packet_unref(packet);
        
        if (frame_count >= 125) {
            printf("Reached 125 frames (5 seconds), stopping...\n");
            break;
        }
    }

    printf("Processed %d frames\n", frame_count);

    // 刷新编码器
    avcodec_send_frame(enc_ctx, nullptr);
    
    AVPacket *enc_pkt = av_packet_alloc();
    if (enc_pkt) {
        int recv_ret = 0;
        while (recv_ret >= 0) {
            recv_ret = avcodec_receive_packet(enc_ctx, enc_pkt);
            if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
                break;
            }
            if (recv_ret < 0) {
                fprintf(stderr, "Error flushing encoder: %d\n", recv_ret);
                break;
            }
            
            enc_pkt->stream_index = out_stream->index;
            av_packet_rescale_ts(enc_pkt, enc_ctx->time_base, out_stream->time_base);
            av_interleaved_write_frame(out_fmt_ctx, enc_pkt);
            av_packet_unref(enc_pkt);
            
            frame_count++;
        }
        av_packet_free(&enc_pkt);
    }

    printf("Total frames written: %d\n", frame_count);

    // 写文件尾
    av_write_trailer(out_fmt_ctx);

    // ★ 打印内存池统计
    print_mempool_stats();

    // 清理
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_frame_free(&enc_frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out_fmt_ctx->pb);
    }
    
    avformat_free_context(out_fmt_ctx);
    avformat_close_input(&in_fmt_ctx);
    avformat_network_deinit();

    printf("Transcoding completed successfully!\n");
    return 0;
}
